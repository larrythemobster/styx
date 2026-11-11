#include "GroupDmDetailsForm.hpp"

#include "Platform/Native.hpp"
#include "Platform/Utf8.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <utility>
#include <utf8proc.h>

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

::Rml::Element *findActionElement(::Rml::Element *element)
{
    for (auto *cur = element; cur; cur = cur->GetParentNode()) {
        if (cur->HasAttribute("data-action"))
            return cur;
    }
    return nullptr;
}

} // namespace

GroupDmDetailsForm::GroupDmDetailsForm(std::function<void()> repaint)
    : m_repaint(std::move(repaint))
{
}

GroupDmDetailsForm::~GroupDmDetailsForm()
{
    setDocument(nullptr);
}

void GroupDmDetailsForm::setDocument(::Rml::ElementDocument *document)
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
        m_nameDraft.clear();
        m_iconDraft = {};
        m_error.clear();
        m_pending = false;
    }
}

void GroupDmDetailsForm::reset(GroupDmDetailsSnapshot snapshot)
{
    m_baseline = std::move(snapshot);
    m_nameDraft = m_baseline.name;
    m_iconDraft = {};
    m_error.clear();
    m_pending = false;

    populateControls();
    if (doc)
        if (auto *input = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(doc->GetElementById("gdm-details-name")))
            input->SetSelectionRange(0, 0);
}

void GroupDmDetailsForm::updateBaseline(GroupDmDetailsSnapshot snapshot)
{
    const bool nameWasClean = (m_nameDraft == m_baseline.name);
    const bool iconWasClean = !isIconDirty();

    m_baseline = std::move(snapshot);

    if (nameWasClean) {
        m_nameDraft = m_baseline.name;
        if (doc) {
            if (auto *nameInput = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(doc->GetElementById("gdm-details-name"))) {
                if (nameInput->GetValue() != m_nameDraft)
                    nameInput->SetValue(m_nameDraft);
            }
        }
    }

    if (iconWasClean)
        m_iconDraft = {};

    if (doc) {
        renderIcon();
        updateDisabledStates();
    }

    if (m_repaint)
        m_repaint();
}

void GroupDmDetailsForm::setIcon(std::string dataUri, std::string previewSource, int previewWidth, int previewHeight)
{
    if (m_pending)
        return;

    m_iconDraft.dataUri = std::move(dataUri);
    m_iconDraft.previewSource = std::move(previewSource);
    m_iconDraft.previewWidth = previewWidth;
    m_iconDraft.previewHeight = previewHeight;
    m_iconDraft.removed = false;

    if (doc) {
        renderIcon();
        updateDisabledStates();
    }

    if (m_repaint)
        m_repaint();
}

void GroupDmDetailsForm::removeIcon()
{
    if (m_pending)
        return;

    m_iconDraft.dataUri.clear();
    m_iconDraft.previewSource.clear();
    m_iconDraft.previewWidth = 0;
    m_iconDraft.previewHeight = 0;
    m_iconDraft.removed = true;

    if (doc) {
        renderIcon();
        updateDisabledStates();
    }

    if (m_repaint)
        m_repaint();
}

void GroupDmDetailsForm::setPending(bool pending)
{
    if (m_pending == pending)
        return;

    m_pending = pending;
    if (!doc)
        return;

    if (m_pending)
        doc->Focus();

    updateDisabledStates();
    if (m_repaint)
        m_repaint();
}

void GroupDmDetailsForm::setError(std::string_view error)
{
    m_error = std::string(error);
    if (!doc)
        return;

    if (auto *errorEl = doc->GetElementById("gdm-details-error")) {
        errorEl->SetInnerRML(Platform::htmlEscape(m_error));
        errorEl->SetProperty("display", m_error.empty() ? "none" : "block");
    }

    if (m_repaint)
        m_repaint();
}

void GroupDmDetailsForm::focusName()
{
    if (!doc)
        return;
    if (auto *context = doc->GetContext())
        context->Update();

    if (m_pending)
        doc->Focus();
    else if (auto *nameInput = doc->GetElementById("gdm-details-name"))
        nameInput->Focus();
}

bool GroupDmDetailsForm::isNameDirty() const
{
    return m_nameDraft != m_baseline.name;
}

bool GroupDmDetailsForm::isIconDirty() const
{
    if (m_iconDraft.removed)
        return !m_baseline.iconHash.empty();
    return !m_iconDraft.dataUri.empty();
}

bool GroupDmDetailsForm::hasActiveIcon() const
{
    if (m_iconDraft.removed)
        return false;
    if (!m_iconDraft.dataUri.empty())
        return true;
    return !m_baseline.iconHash.empty();
}

bool GroupDmDetailsForm::isDirty() const
{
    return isNameDirty() || isIconDirty();
}

std::string_view GroupDmDetailsForm::nameError() const
{
    if (Platform::trim(m_nameDraft).empty())
        return "Group name is required";

    std::size_t codePoints = 0;
    std::size_t offset = 0;
    while (offset < m_nameDraft.size()) {
        utf8proc_int32_t cp = 0;
        const auto length = utf8proc_iterate(
            reinterpret_cast<const utf8proc_uint8_t *>(m_nameDraft.data() + offset),
            static_cast<utf8proc_ssize_t>(m_nameDraft.size() - offset), &cp);
        if (length <= 0)
            return "Group name contains invalid UTF-8";
        if (++codePoints > 100)
            return "Group name must be at most 100 characters";
        offset += static_cast<std::size_t>(length);
    }
    return {};
}

bool GroupDmDetailsForm::isNameValid() const
{
    if (!isNameDirty())
        return true;
    return nameError().empty();
}

Core::Result<Discord::GroupDmEdit> GroupDmDetailsForm::edit() const
{
    if (!isDirty())
        return Core::Result<Discord::GroupDmEdit>::makeError("No changes to save");

    Discord::GroupDmEdit result;

    if (isNameDirty()) {
        const auto error = nameError();
        if (!error.empty())
            return Core::Result<Discord::GroupDmEdit>::makeError(std::string(error));
        result.name = m_nameDraft;
    }

    if (m_iconDraft.removed) {
        if (!m_baseline.iconHash.empty())
            result.icon = nullptr;
    } else if (!m_iconDraft.dataUri.empty()) {
        result.icon = m_iconDraft.dataUri;
    }

    return Core::Result<Discord::GroupDmEdit>::makeOk(std::move(result));
}

void GroupDmDetailsForm::renderIcon()
{
    if (!doc)
        return;

    auto *previewEl = doc->GetElementById("gdm-details-icon-preview");
    auto *stateEl = doc->GetElementById("gdm-details-icon-state");
    if (!previewEl || !stateEl)
        return;

    if (!m_iconDraft.dataUri.empty() && !m_iconDraft.removed) {
        if (m_iconDraft.previewSource.empty() || m_iconDraft.previewWidth <= 0 || m_iconDraft.previewHeight <= 0) {
            previewEl->SetProperty("display", "none");
            previewEl->RemoveAttribute("src");
            stateEl->SetProperty("display", "flex");
            stateEl->SetInnerRML("<span>Preview unavailable</span>");
            return;
        }

        const double scale = std::min(96.0 / m_iconDraft.previewWidth, 96.0 / m_iconDraft.previewHeight);
        const int w = std::clamp(static_cast<int>(std::round(m_iconDraft.previewWidth * scale)), 1, 96);
        const int h = std::clamp(static_cast<int>(std::round(m_iconDraft.previewHeight * scale)), 1, 96);

        stateEl->SetProperty("display", "none");
        stateEl->SetInnerRML("");
        previewEl->SetProperty("display", "block");
        previewEl->SetProperty("width", std::format("{}dp", w));
        previewEl->SetProperty("height", std::format("{}dp", h));
        previewEl->SetAttribute("src", m_iconDraft.previewSource);
        return;
    }

    stateEl->SetProperty("display", "none");
    stateEl->SetInnerRML("");
    previewEl->SetProperty("display", "block");
    const bool savedIcon = !m_iconDraft.removed && !m_baseline.iconHash.empty() && !m_baseline.iconSrc.empty();
    previewEl->SetProperty("width", savedIcon ? "96dp" : "32dp");
    previewEl->SetProperty("height", savedIcon ? "96dp" : "32dp");
    if (savedIcon)
        previewEl->SetAttribute("src", m_baseline.iconSrc);
    else
        previewEl->SetAttribute("src", "icon:camera");
}

void GroupDmDetailsForm::updateDisabledStates()
{
    if (!doc)
        return;

    if (auto *nameInput = doc->GetElementById("gdm-details-name"))
        setControlDisabled(nameInput, m_pending);

    if (auto *chooseBtn = doc->GetElementById("gdm-details-choose"))
        setControlDisabled(chooseBtn, m_pending);

    if (auto *removeBtn = doc->GetElementById("gdm-details-remove"))
        setControlDisabled(removeBtn, m_pending || !hasActiveIcon());

    if (auto *resetBtn = doc->GetElementById("gdm-details-reset"))
        setControlDisabled(resetBtn, m_pending || !isDirty());

    const bool canSave = !m_pending && isDirty() && isNameValid();
    if (auto *saveBtn = doc->GetElementById("gdm-details-save")) {
        setControlDisabled(saveBtn, !canSave);
        saveBtn->SetInnerRML(m_pending ? "Saving..." : "Save");
    }

    if (auto *statusEl = doc->GetElementById("gdm-details-status")) {
        statusEl->SetProperty("display", m_pending ? "block" : "none");
        statusEl->SetInnerRML(m_pending ? "Saving changes..." : "");
    }
}

void GroupDmDetailsForm::populateControls()
{
    if (!doc)
        return;

    if (auto *nameInput = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(doc->GetElementById("gdm-details-name"))) {
        if (nameInput->GetValue() != m_nameDraft)
            nameInput->SetValue(m_nameDraft);
    }

    renderIcon();
    updateDisabledStates();

    if (auto *errorEl = doc->GetElementById("gdm-details-error")) {
        errorEl->SetInnerRML(Platform::htmlEscape(m_error));
        errorEl->SetProperty("display", m_error.empty() ? "none" : "block");
    }

    if (m_repaint)
        m_repaint();
}

void GroupDmDetailsForm::ProcessEvent(::Rml::Event &event)
{
    const auto &type = event.GetType();

    if (type == "keydown") {
        const int key = event.GetParameter<int>("key_identifier", 0);
        if (key == ::Rml::Input::KI_ESCAPE) {
            event.StopPropagation();
            closeRequested.publish();
            return;
        }

        if (key == ::Rml::Input::KI_RETURN || key == ::Rml::Input::KI_NUMPADENTER) {
            auto *target = event.GetTargetElement();
            if (target && target->GetId() == "gdm-details-name") {
                event.StopPropagation();
                if (!m_pending && isDirty() && isNameValid())
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
        if (!target || target->GetId() != "gdm-details-name")
            return;

        if (auto *input = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(target)) {
            const std::string val = event.GetParameter<::Rml::String>("value", input->GetValue());
            if (val != m_nameDraft) {
                m_nameDraft = val;
                updateDisabledStates();
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
        if (action == "gdm-details-close") {
            closeRequested.publish();
            return;
        }

        if (m_pending || actionEl->HasAttribute("disabled"))
            return;

        if (action == "gdm-details-pick-icon") {
            pickIconRequested.publish();
            return;
        }

        if (action == "gdm-details-remove-icon") {
            removeIconRequested.publish();
            return;
        }

        if (action == "gdm-details-reset") {
            resetRequested.publish();
            return;
        }

        if (action == "gdm-details-save") {
            if (isDirty() && isNameValid())
                submitRequested.publish();
            return;
        }
    }
}

} // namespace Styx::UI::Rml
