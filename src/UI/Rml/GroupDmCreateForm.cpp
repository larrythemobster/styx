#include "GroupDmCreateForm.hpp"
#include "Core/TokenUtils.hpp"

#include "Platform/Native.hpp"
#include "Platform/Utf8.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

#include <algorithm>
#include <format>
#include <optional>
#include <utility>

namespace Styx::UI::Rml {

namespace {

void setControlDisabled(::Rml::Element *element, bool disabled)
{
    if (disabled)
        element->SetAttribute("disabled", "");
    else
        element->RemoveAttribute("disabled");
    element->SetPseudoClass("disabled", disabled);
}

std::optional<Core::Snowflake> resolveUserId(::Rml::Element *element)
{
    const auto id = Core::TokenUtils::parseId(element->GetAttribute<std::string>("data-user-id", ""));
    if (!id || *id == 0)
        return std::nullopt;
    return Core::Snowflake(*id);
}

::Rml::Element *findActionElement(::Rml::Element *element)
{
    for (auto *cur = element; cur; cur = cur->GetParentNode()) {
        if (cur->HasAttribute("data-action"))
            return cur;
    }
    return nullptr;
}

} // namespace

GroupDmCreateForm::GroupDmCreateForm(std::function<void()> repaint)
    : m_repaint(std::move(repaint))
{
}

GroupDmCreateForm::~GroupDmCreateForm()
{
    setDocument(nullptr);
}

void GroupDmCreateForm::setDocument(::Rml::ElementDocument *document)
{
    if (doc == document)
        return;

    if (doc) {
        doc->RemoveEventListener("click", this);
        doc->RemoveEventListener("change", this);
        doc->RemoveEventListener("keydown", this, true);
    }

    doc = document;

    if (doc) {
        doc->AddEventListener("click", this);
        doc->AddEventListener("change", this);
        doc->AddEventListener("keydown", this, true);

        populateControls();
    }
}

void GroupDmCreateForm::reset(std::vector<GroupDmFriend> friends, std::size_t maximumSelections, bool allowEmpty)
{
    m_friends = std::move(friends);
    m_maximumSelections = maximumSelections;
    m_allowEmpty = allowEmpty;
    m_query.clear();
    m_selected.clear();
    m_pending = false;
    m_error.clear();

    populateControls();
}

void GroupDmCreateForm::setCandidates(std::vector<GroupDmFriend> friends)
{
    if (m_pending)
        return;

    m_friends = std::move(friends);

    std::erase_if(m_selected, [this](const Core::Snowflake &id) {
        return std::none_of(m_friends.begin(), m_friends.end(), [&](const GroupDmFriend &f) {
            return f.userId == id;
        });
    });

    if (doc) {
        rebuildFriendRows();
        updateContextAndCount();
        updateSubmitButton();
    }

    if (m_repaint)
        m_repaint();
}

const std::vector<Core::Snowflake> &GroupDmCreateForm::selected() const
{
    return m_selected;
}

void GroupDmCreateForm::setPending(bool pending)
{
    if (m_pending == pending)
        return;

    m_pending = pending;
    if (!doc)
        return;

    if (m_pending)
        doc->Focus();

    updateAllDisabledStates();
    if (m_repaint)
        m_repaint();
}

void GroupDmCreateForm::setError(std::string_view error)
{
    m_error = std::string(error);
    if (!doc)
        return;

    if (auto *errorEl = doc->GetElementById("group-dm-error")) {
        errorEl->SetInnerRML(Platform::htmlEscape(m_error));
        errorEl->SetProperty("display", m_error.empty() ? "none" : "block");
    }
    if (m_repaint)
        m_repaint();
}

void GroupDmCreateForm::focusSearch()
{
    if (!doc)
        return;

    if (m_pending)
        doc->Focus();
    else if (auto *search = doc->GetElementById("group-dm-search"))
        search->Focus();
}

void GroupDmCreateForm::ProcessEvent(::Rml::Event &event)
{
    const auto &type = event.GetType();

    if (type == "keydown") {
        const int key = event.GetParameter<int>("key_identifier", 0);
        if (key == ::Rml::Input::KI_ESCAPE) {
            event.StopPropagation();
            cancelRequested.publish();
            return;
        }

        if (key == ::Rml::Input::KI_RETURN || key == ::Rml::Input::KI_NUMPADENTER) {
            auto *target = event.GetTargetElement();
            if (target && target->GetId() == "group-dm-search") {
                event.StopPropagation();
                if (!m_pending && canSubmit())
                    submitRequested.publish();
                return;
            }
        }
        return;
    }

    if (type == "change") {
        if (m_pending)
            return;

        auto *target = event.GetTargetElement();
        if (!target || target->GetId() != "group-dm-search")
            return;

        if (auto *input = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(target)) {
            const std::string val = event.GetParameter<::Rml::String>("value", input->GetValue());
            if (val != m_query) {
                m_query = val;
                rebuildFriendRows();
                if (m_repaint)
                    m_repaint();
            }
        }
        return;
    }

    if (type == "click") {
        auto *actionEl = findActionElement(event.GetTargetElement());
        if (!actionEl)
            return;

        const std::string action = actionEl->GetAttribute<std::string>("data-action", "");
        if (action == "group-dm-cancel") {
            cancelRequested.publish();
            return;
        }

        if (m_pending || actionEl->HasAttribute("disabled"))
            return;

        if (action == "group-dm-create") {
            if (!canSubmit())
                return;
            submitRequested.publish();
            return;
        }

        if (action == "group-dm-toggle") {
            auto userIdOpt = resolveUserId(actionEl);
            if (!userIdOpt)
                return;
            toggleFriend(*userIdOpt);
            return;
        }
    }
}

void GroupDmCreateForm::toggleFriend(const Core::Snowflake &userId)
{
    if (m_pending)
        return;

    auto itCandidate = std::find_if(m_friends.begin(), m_friends.end(), [&](const GroupDmFriend &f) {
        return f.userId == userId;
    });
    if (itCandidate == m_friends.end())
        return;

    auto it = std::find(m_selected.begin(), m_selected.end(), userId);
    if (it != m_selected.end()) {
        m_selected.erase(it);
    } else {
        if (m_selected.size() >= m_maximumSelections)
            return;
        m_selected.push_back(userId);
    }

    if (!m_error.empty())
        setError({});

    rebuildFriendRows();
    updateContextAndCount();
    updateSubmitButton();

    if (m_repaint)
        m_repaint();
}

bool GroupDmCreateForm::canSubmit() const
{
    if (m_selected.size() > m_maximumSelections)
        return false;
    if (!m_allowEmpty && m_selected.empty())
        return false;
    return true;
}

void GroupDmCreateForm::updateSubmitButton()
{
    if (!doc)
        return;

    if (auto *submitBtn = doc->GetElementById("group-dm-submit")) {
        const bool enabled = !m_pending && canSubmit();
        setControlDisabled(submitBtn, !enabled);
    }
}

void GroupDmCreateForm::updateContextAndCount()
{
    if (!doc)
        return;

    if (auto *countEl = doc->GetElementById("group-dm-count"))
        countEl->SetInnerRML(std::format("{}/{}", m_selected.size(), m_maximumSelections));

    if (auto *contextEl = doc->GetElementById("group-dm-context")) {
        const std::size_t remaining = (m_selected.size() < m_maximumSelections) ? (m_maximumSelections - m_selected.size()) : 0;
        if (m_selected.empty()) {
            if (m_maximumSelections == 1)
                contextEl->SetInnerRML("You can add 1 friend.");
            else
                contextEl->SetInnerRML(std::format("You can add up to {} friends.", m_maximumSelections));
        } else if (remaining == 0) {
            contextEl->SetInnerRML(std::format("You can add up to {} friends.", m_maximumSelections));
        } else if (remaining == 1) {
            contextEl->SetInnerRML("You can add 1 more friend.");
        } else {
            contextEl->SetInnerRML(std::format("You can add up to {} more friends.", remaining));
        }
    }
}

void GroupDmCreateForm::updateAllDisabledStates()
{
    if (!doc)
        return;

    if (auto *search = doc->GetElementById("group-dm-search"))
        setControlDisabled(search, m_pending);

    updateSubmitButton();

    const bool atCapacity = m_selected.size() >= m_maximumSelections;
    ::Rml::ElementList friendBtns;
    doc->QuerySelectorAll(friendBtns, ".group-dm-friend");
    for (auto *btn : friendBtns) {
        auto userIdOpt = resolveUserId(btn);
        const bool isSelected = userIdOpt && (std::find(m_selected.begin(), m_selected.end(), *userIdOpt) != m_selected.end());
        const bool disabled = m_pending || (!isSelected && atCapacity);
        setControlDisabled(btn, disabled);
    }

    if (auto *statusEl = doc->GetElementById("group-dm-status")) {
        statusEl->SetProperty("display", m_pending ? "block" : "none");
        statusEl->SetInnerRML(m_pending ? "Creating group DM..." : "");
    }
}

void GroupDmCreateForm::rebuildFriendRows()
{
    if (!doc)
        return;

    auto *friendsHost = doc->GetElementById("group-dm-friends");
    if (!friendsHost)
        return;

    std::string focusedId;
    if (auto *focused = doc->GetContext()->GetFocusElement())
        focusedId = focused->GetId();

    const std::string queryFolded = Platform::caseFold(Platform::trim(m_query));
    const bool atCapacity = m_selected.size() >= m_maximumSelections;

    std::string html;
    html.reserve(m_friends.size() * 256);
    for (const auto &f : m_friends) {
        if (!queryFolded.empty()) {
            const bool matches = Platform::caseFold(f.displayName).find(queryFolded) != std::string::npos
                              || Platform::caseFold(f.username).find(queryFolded) != std::string::npos;
            if (!matches)
                continue;
        }

        const bool isSelected = std::find(m_selected.begin(), m_selected.end(), f.userId) != m_selected.end();
        const bool disabled = m_pending || (!isSelected && atCapacity);
        const std::string idStr = f.userId.toString();

        std::string title = f.displayName;
        if (!f.username.empty() && f.username != f.displayName)
            title += " (@" + f.username + ")";

        html += std::format(
            "<button id=\"group-dm-friend-{0}\" class=\"group-dm-friend{1}\" data-action=\"group-dm-toggle\" data-user-id=\"{0}\" title=\"{2}\"{3}>"
            "<div class=\"group-dm-friend-left\">"
            "<img class=\"group-dm-friend-avatar\" src=\"{4}\" alt=\"\"/>"
            "<div class=\"group-dm-friend-text\">"
            "<span class=\"group-dm-friend-name\" title=\"{5}\">{5}</span>",
            idStr,
            isSelected ? " selected" : "",
            Platform::htmlEscape(title),
            disabled ? " disabled" : "",
            Platform::htmlEscape(f.avatarSrc.empty() ? "icon:user-plus" : f.avatarSrc),
            Platform::htmlEscape(f.displayName)
        );

        if (!f.username.empty()) {
            html += std::format(
                "<span class=\"group-dm-friend-username\" title=\"@{0}\">@{0}</span>",
                Platform::htmlEscape(f.username)
            );
        }

        html += "</div></div>"
                "<div class=\"group-dm-checkbox\"><img class=\"group-dm-check-icon\" src=\"icon:check\" alt=\"\"/></div>"
                "</button>";
    }

    if (html.empty()) {
        if (m_friends.empty())
            html = m_allowEmpty
                ? "<div class=\"group-dm-no-results\">Add friends to invite them, or create an empty group.</div>"
                : "<div class=\"group-dm-no-results\">Add more friends to create a group from this conversation.</div>";
        else
            html = "<div class=\"group-dm-no-results\">No friends match your search.</div>";
    }

    friendsHost->SetInnerRML(html);

    ::Rml::ElementList friendBtns;
    friendsHost->QuerySelectorAll(friendBtns, ".group-dm-friend");
    for (auto *btn : friendBtns) {
        const bool btnDisabled = btn->HasAttribute("disabled");
        btn->SetPseudoClass("disabled", btnDisabled);
    }

    if (focusedId.starts_with("group-dm-friend-")) {
        auto *replacement = doc->GetElementById(focusedId);
        if (replacement && !replacement->HasAttribute("disabled"))
            replacement->Focus();
        else
            focusSearch();
    }
}

void GroupDmCreateForm::populateControls()
{
    if (!doc)
        return;

    if (auto *search = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(doc->GetElementById("group-dm-search")))
        search->SetValue(m_query);

    setError(m_error);
    rebuildFriendRows();
    updateContextAndCount();
    updateAllDisabledStates();

    if (m_repaint)
        m_repaint();
}

} // namespace Styx::UI::Rml
