#include "GroupDmMembersForm.hpp"
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

enum class RowActionType
{
    None,
    Add,
    Remove,
};

void setControlDisabled(::Rml::Element *element, bool disabled)
{
    if (disabled)
        element->SetAttribute("disabled", "");
    else
        element->RemoveAttribute("disabled");
    element->SetPseudoClass("disabled", disabled);
}

void syncButtonDisabledStates(::Rml::Element *container)
{
    ::Rml::ElementList btns;
    container->QuerySelectorAll(btns, "button");
    for (auto *btn : btns)
        btn->SetPseudoClass("disabled", btn->HasAttribute("disabled"));
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

std::string renderPersonRow(const GroupDmMemberPerson &person, RowActionType actionType, bool disabled)
{
    const std::string idStr = person.userId.toString();
    const std::string_view avatarSrc = person.avatarSrc.empty() ? std::string_view("icon:user-plus") : std::string_view(person.avatarSrc);

    std::string title = person.displayName;
    if (!person.username.empty() && person.username != person.displayName)
        title += " (@" + person.username + ")";

    std::string html;
    html.reserve(384);
    html += "<div class=\"gdm-members-row\">"
            "<div class=\"gdm-members-row-left\">"
            "<img class=\"gdm-members-avatar\" src=\"" + Platform::htmlEscape(avatarSrc) + "\" alt=\"\"/>"
            "<div class=\"gdm-members-user-info\">"
            "<div class=\"gdm-members-name-line\">"
            "<span class=\"gdm-members-name\" title=\"" + Platform::htmlEscape(title) + "\">"
            + Platform::htmlEscape(person.displayName) + "</span>";

    if (person.isOwner)
        html += "<span class=\"gdm-members-badge gdm-members-badge-owner\" title=\"Owner\"><img class=\"gdm-members-crown\" src=\"icon:crown\" alt=\"\"/><span>Owner</span></span>";
    if (person.isSelf)
        html += "<span class=\"gdm-members-badge gdm-members-badge-self\" title=\"You\"><span>You</span></span>";

    html += "</div>";

    if (!person.username.empty())
        html += "<span class=\"gdm-members-username\" title=\"@" + Platform::htmlEscape(person.username) + "\">@" + Platform::htmlEscape(person.username) + "</span>";

    html += "</div></div>"
            "<div class=\"gdm-members-row-right\">";

    if (actionType != RowActionType::None) {
        const std::string_view action = actionType == RowActionType::Add ? "add" : "remove";
        const std::string_view label = actionType == RowActionType::Add ? "Add" : "Remove";
        html += std::format(
            "<button id=\"gdm-members-{0}-{1}\" class=\"gdm-members-btn gdm-members-{0}-btn\" data-action=\"gdm-members-{0}\" data-user-id=\"{1}\" title=\"{2} {3}\"{4}>{2}</button>",
            action, idStr, label, Platform::htmlEscape(person.displayName), disabled ? " disabled" : "");
    }

    html += "</div></div>";
    return html;
}

} // namespace

GroupDmMembersForm::GroupDmMembersForm(std::function<void()> repaint)
    : m_repaint(std::move(repaint))
{
}

GroupDmMembersForm::~GroupDmMembersForm()
{
    setDocument(nullptr);
}

void GroupDmMembersForm::setDocument(::Rml::ElementDocument *document)
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
    } else {
        m_query.clear();
        m_error.clear();
        m_pending = false;
    }
}

void GroupDmMembersForm::reset(GroupDmMembersSnapshot snapshot)
{
    m_snapshot = std::move(snapshot);
    m_query.clear();
    m_error.clear();
    m_pending = false;

    populateControls();
}

void GroupDmMembersForm::update(GroupDmMembersSnapshot snapshot)
{
    m_snapshot = std::move(snapshot);
    if (!doc)
        return;

    updateHeaderAndHint();
    rebuildMemberRows();
    rebuildFriendRows();
    updateAllDisabledStates();

    if (m_repaint)
        m_repaint();
}

void GroupDmMembersForm::setPending(bool pending)
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

void GroupDmMembersForm::setError(std::string_view error)
{
    m_error = std::string(error);
    if (!doc)
        return;

    if (auto *errorEl = doc->GetElementById("gdm-members-error")) {
        errorEl->SetInnerRML(Platform::htmlEscape(m_error));
        errorEl->SetProperty("display", m_error.empty() ? "none" : "block");
    }
    if (m_repaint)
        m_repaint();
}

void GroupDmMembersForm::focusSearch()
{
    if (!doc)
        return;
    if (auto *context = doc->GetContext())
        context->Update();

    if (m_pending)
        doc->Focus();
    else if (auto *search = doc->GetElementById("gdm-members-search"))
        search->Focus();
}

void GroupDmMembersForm::ProcessEvent(::Rml::Event &event)
{
    const auto &type = event.GetType();

    if (type == "keydown") {
        const int key = event.GetParameter<int>("key_identifier", 0);
        if (key == ::Rml::Input::KI_ESCAPE) {
            event.StopPropagation();
            closeRequested.publish();
            return;
        }
        return;
    }

    if (type == "change") {
        if (m_pending)
            return;

        auto *target = event.GetTargetElement();
        if (!target || target->GetId() != "gdm-members-search")
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
        if (action == "gdm-members-close") {
            closeRequested.publish();
            return;
        }

        if (m_pending || actionEl->HasAttribute("disabled"))
            return;

        if (action == "gdm-members-add") {
            if (m_snapshot.members.size() >= 10)
                return;

            const auto userIdOpt = resolveUserId(actionEl);
            if (!userIdOpt)
                return;

            const auto &userId = *userIdOpt;

            auto itFriend = std::find_if(m_snapshot.friends.begin(), m_snapshot.friends.end(), [&](const GroupDmMemberPerson &p) {
                return p.userId == userId;
            });
            if (itFriend == m_snapshot.friends.end())
                return;

            auto itMember = std::find_if(m_snapshot.members.begin(), m_snapshot.members.end(), [&](const GroupDmMemberPerson &p) {
                return p.userId == userId;
            });
            if (itMember != m_snapshot.members.end())
                return;

            addRequested.publish(userId);
            return;
        }

        if (action == "gdm-members-remove") {
            if (!m_snapshot.canRemove)
                return;

            const auto userIdOpt = resolveUserId(actionEl);
            if (!userIdOpt)
                return;

            const auto &userId = *userIdOpt;

            auto itMember = std::find_if(m_snapshot.members.begin(), m_snapshot.members.end(), [&](const GroupDmMemberPerson &p) {
                return p.userId == userId;
            });
            if (itMember == m_snapshot.members.end() || itMember->isSelf)
                return;

            removeRequested.publish(userId);
            return;
        }
    }
}

void GroupDmMembersForm::updateHeaderAndHint()
{
    if (!doc)
        return;

    const std::size_t count = m_snapshot.members.size();

    if (auto *countEl = doc->GetElementById("gdm-members-count"))
        countEl->SetInnerRML(std::format("{} / 10", count));

    if (auto *hintEl = doc->GetElementById("gdm-members-hint")) {
        std::string hint;
        if (count >= 10)
            hint = "This group has reached the 10 member limit.";
        else {
            const auto remaining = 10 - count;
            hint = std::format("You can add {} more {}.", remaining, remaining == 1 ? "friend" : "friends");
        }
        if (!m_snapshot.canRemove)
            hint += " Only the group owner can remove members.";
        hintEl->SetInnerRML(hint);
    }
}

void GroupDmMembersForm::updateAllDisabledStates()
{
    if (!doc)
        return;

    if (auto *search = doc->GetElementById("gdm-members-search"))
        setControlDisabled(search, m_pending);

    const bool atCapacity = m_snapshot.members.size() >= 10;

    ::Rml::ElementList addBtns;
    doc->QuerySelectorAll(addBtns, ".gdm-members-add-btn");
    for (auto *btn : addBtns)
        setControlDisabled(btn, m_pending || atCapacity);

    ::Rml::ElementList removeBtns;
    doc->QuerySelectorAll(removeBtns, ".gdm-members-remove-btn");
    for (auto *btn : removeBtns)
        setControlDisabled(btn, m_pending);

    if (auto *statusEl = doc->GetElementById("gdm-members-status")) {
        statusEl->SetProperty("display", m_pending ? "block" : "none");
        statusEl->SetInnerRML(m_pending ? "Updating members..." : "");
    }
}

void GroupDmMembersForm::rebuildMemberRows()
{
    if (!doc)
        return;

    auto *membersHost = doc->GetElementById("gdm-members-list");
    if (!membersHost)
        return;

    std::string focusedId;
    if (auto *ctx = doc->GetContext()) {
        if (auto *focused = ctx->GetFocusElement())
            focusedId = focused->GetId();
    }

    std::string html;
    html.reserve(m_snapshot.members.size() * 384);

    for (const auto &m : m_snapshot.members) {
        RowActionType actionType = RowActionType::None;
        if (m_snapshot.canRemove && !m.isSelf && !m.isOwner)
            actionType = RowActionType::Remove;
        html += renderPersonRow(m, actionType, m_pending);
    }

    if (html.empty())
        html = "<div class=\"gdm-members-empty\">No members in this group.</div>";

    membersHost->SetInnerRML(html);
    syncButtonDisabledStates(membersHost);

    if (!focusedId.empty() && focusedId.starts_with("gdm-members-remove-")) {
        auto *replacement = doc->GetElementById(focusedId);
        if (replacement && !replacement->HasAttribute("disabled"))
            replacement->Focus();
        else
            focusSearch();
    }
}

void GroupDmMembersForm::rebuildFriendRows()
{
    if (!doc)
        return;

    auto *friendsHost = doc->GetElementById("gdm-members-friends");
    if (!friendsHost)
        return;

    std::string focusedId;
    if (auto *ctx = doc->GetContext()) {
        if (auto *focused = ctx->GetFocusElement())
            focusedId = focused->GetId();
    }

    const std::string queryFolded = Platform::caseFold(Platform::trim(m_query));
    const bool atCapacity = m_snapshot.members.size() >= 10;
    const bool actionDisabled = m_pending || atCapacity;

    std::string html;
    html.reserve(m_snapshot.friends.size() * 384);

    for (const auto &f : m_snapshot.friends) {
        if (!queryFolded.empty()) {
            const bool matches = Platform::caseFold(f.displayName).find(queryFolded) != std::string::npos
                              || Platform::caseFold(f.username).find(queryFolded) != std::string::npos;
            if (!matches)
                continue;
        }

        html += renderPersonRow(f, RowActionType::Add, actionDisabled);
    }

    if (html.empty()) {
        if (m_snapshot.friends.empty())
            html = "<div class=\"gdm-members-empty\">No friends available to add.</div>";
        else
            html = "<div class=\"gdm-members-empty\">No friends match your search.</div>";
    }

    friendsHost->SetInnerRML(html);
    syncButtonDisabledStates(friendsHost);

    if (!focusedId.empty() && focusedId.starts_with("gdm-members-add-")) {
        auto *replacement = doc->GetElementById(focusedId);
        if (replacement && !replacement->HasAttribute("disabled"))
            replacement->Focus();
        else
            focusSearch();
    }
}

void GroupDmMembersForm::populateControls()
{
    if (!doc)
        return;

    if (auto *search = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(doc->GetElementById("gdm-members-search"))) {
        if (search->GetValue() != m_query)
            search->SetValue(m_query);
    }

    setError(m_error);
    updateHeaderAndHint();
    rebuildMemberRows();
    rebuildFriendRows();
    updateAllDisabledStates();

    if (m_repaint)
        m_repaint();
}

} // namespace Styx::UI::Rml
