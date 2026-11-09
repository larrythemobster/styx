#include "CustomStatusController.hpp"

#include "ExpressionPickerController.hpp"
#include "RmlRuntime.hpp"
#include "Shell.hpp"

#include "Core/ClientInstance.hpp"
#include "Core/Emoji/EmojiManager.hpp"
#include "Discord/CdnUrls.hpp"
#include "Discord/Client.hpp"
#include "Platform/Native.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

#include <utility>
#include <ctime>
#include <format>
#include <optional>

namespace Styx {
namespace UI {
namespace Rml {

namespace {

void setControlDisabled(::Rml::Element *element, bool disabled)
{
    if (!element)
        return;
    if (disabled)
        element->SetAttribute("disabled", "");
    else
        element->RemoveAttribute("disabled");
    if (element->GetTagName() == "button")
        element->SetPseudoClass("disabled", disabled);
}

} // namespace

CustomStatusController::CustomStatusController(Shell &shell)
    : Controller(shell, nullptr)
{
    track(this->shell.instanceChanged.connect([this](Core::ClientInstance *) { close(); }));
}

CustomStatusController::~CustomStatusController()
{
    visible = false;
    if (picker)
        picker->close();
    if (customStatusDocument)
        customStatusDocument->Close();
}

void CustomStatusController::open()
{
    ensureDocument();
    if (!customStatusDocument)
        return;

    ++generation;
    saving = false;
    error.clear();
    draft = Proto::CustomStatus{};
    const auto nowMs = static_cast<uint64_t>(Platform::DateTime::now().unixMillis());
    auto *inst = shell.currentInstance();
    editingAccountId = inst ? inst->accountId() : Core::Snowflake::Invalid;
    if (!editingAccountId.isValid()) {
        error = "No active account.";
    } else if (auto *client = inst->discord()) {
        const auto &settings = client->getSettings();
        if (settings.status && settings.status->customStatus) {
            const auto &existing = *settings.status->customStatus;
            if (existing.expiresAtMs == 0 || existing.expiresAtMs > nowMs)
                draft = existing;
        }
    }

    auto *inputEl = textInput();
    if (inputEl)
        inputEl->SetValue(draft.text);
    if (auto *expiryEl = expirySelect())
        expiryEl->SetValue(draft.expiresAtMs != 0 ? "keep" : "never");

    visible = true;
    customStatusDocument->Show(::Rml::ModalFlag::None);
    customStatusDocument->SetProperty("display", "flex");
    customStatusDocument->PullToFront();
    render();
    if (inputEl && editingAccountId.isValid())
        inputEl->Focus();
    else
        customStatusDocument->Focus();
}

void CustomStatusController::close()
{
    if (!visible)
        return;

    visible = false;
    saving = false;
    ++generation;
    if (picker && picker->isOpen())
        picker->close();
    if (customStatusDocument)
        customStatusDocument->Hide();
    requestRender();
}

void CustomStatusController::ensureDocument()
{
    if (customStatusDocument)
        return;

    auto *view = shell.view();
    if (!view || !view->context())
        return;

    customStatusDocument = view->context()->LoadDocument("ui/custom_status.rml");
    if (!customStatusDocument)
        return;

    regionRoot = customStatusDocument;
    setupEventListeners();
}

void CustomStatusController::setupEventListeners()
{
    listen("click", [this](::Rml::Event &event) {
        auto *actionEl = closest(event.GetTargetElement(), "data-action");
        if (!actionEl)
            return;

        const std::string action = attribute(actionEl, "data-action");
        if (action == "close") {
            close();
            return;
        }

        if (saving || !editingAccountId.isValid())
            return;

        if (action == "choose-emoji") {
            chooseEmoji();
        } else if (action == "remove-emoji") {
            if (draft.emojiId != 0 || !draft.emojiName.empty()) {
                draft.emojiId = 0;
                draft.emojiName.clear();
                render();
            }
        } else if (action == "clear") {
            save(true);
        } else if (action == "save") {
            save(false);
        }
    });

    listen("keydown", [this](::Rml::Event &event) {
        if (event.GetParameter<int>("key_identifier", 0) != ::Rml::Input::KI_ESCAPE)
            return;
        event.StopPropagation();
        close();
    });

    if (auto *inputEl = textInput()) {
        listen("keydown", [this](::Rml::Event &event) {
            const int key = event.GetParameter<int>("key_identifier", 0);
            if (key != ::Rml::Input::KI_RETURN && key != ::Rml::Input::KI_NUMPADENTER)
                return;
            event.StopPropagation();
            if (!saving && editingAccountId.isValid())
                save(false);
        }, inputEl);
    }
}

void CustomStatusController::render()
{
    if (!customStatusDocument)
        return;

    if (auto *errorEl = errorElement()) {
        errorEl->SetInnerRML(Platform::htmlEscape(error));
        errorEl->SetProperty("display", error.empty() ? "none" : "block");
    }

    const bool hasEmoji = draft.emojiId != 0 || !draft.emojiName.empty();
    const bool disabled = saving || !editingAccountId.isValid();
    auto *emojiBtn = emojiButton();
    if (emojiBtn) {
        std::string src;
        if (draft.emojiId != 0)
            src = shell.imageSource(Discord::Cdn::emoji(Core::Snowflake(draft.emojiId), 32, false), {24, 24});
        else if (!draft.emojiName.empty())
            src = Runtime::emojiSource(draft.emojiName);
        if (src.empty()) {
            emojiBtn->SetInnerRML("<img class=\"custom-status-emoji-icon\" src=\"icon:smile\" alt=\"Choose emoji\"/>");
        } else {
            emojiBtn->SetInnerRML(std::format("<img class=\"custom-status-emoji-img\" src=\"{}\" alt=\"Status emoji\"/>", Rml::escape(src)));
        }
    }

    auto *saveBtn = saveButton();
    if (saveBtn)
        saveBtn->SetInnerRML(saving ? "Saving…" : "Save");
    ::Rml::Element *controls[] = {textInput(), expirySelect(), emojiBtn, clearButton(), saveBtn};
    for (auto *control : controls)
        setControlDisabled(control, disabled);
    setControlDisabled(removeEmojiButton(), !hasEmoji || disabled);
    requestRender();
}

void CustomStatusController::chooseEmoji()
{
    auto *inst = shell.currentInstance();
    if (!inst || inst->accountId() != editingAccountId)
        return;
    if (!picker) {
        picker = std::make_unique<ExpressionPickerController>(shell);
        track(picker->emojiPicked.connect([this](const Core::PickerEmoji &emoji, bool) {
            draft.emojiId = emoji.isCustom() ? static_cast<uint64_t>(emoji.customId) : 0;
            draft.emojiName = emoji.isCustom() ? std::string{} : emoji.surrogates;
            picker->close();
            render();
        }));
        track(picker->closed.connect([this]() {
            if (visible) {
                if (auto *textEl = textInput())
                    textEl->Focus();
            }
        }));
    }

    Platform::Rect anchor;
    if (auto *emojiBtn = emojiButton()) {
        anchor.x = emojiBtn->GetAbsoluteLeft();
        anchor.y = emojiBtn->GetAbsoluteTop();
        anchor.width = emojiBtn->GetClientWidth();
        anchor.height = emojiBtn->GetClientHeight();
    }
    picker->openForStatus(inst->emojis(), editingAccountId, anchor);
}

void CustomStatusController::save(bool clear)
{
    if (saving)
        return;

    auto *inst = shell.currentInstance();
    if (!inst || inst->accountId() != editingAccountId || !inst->discord()) {
        error = "Account unavailable.";
        render();
        return;
    }

    std::string text;
    if (auto *inputEl = textInput())
        text = inputEl->GetValue();

    std::optional<Proto::CustomStatus> payload;
    if (!clear && (!text.empty() || draft.emojiId != 0 || !draft.emojiName.empty())) {
        const auto nowMs = Platform::DateTime::now().unixMillis();
        auto &status = payload.emplace();
        status.text = std::move(text);
        status.emojiId = draft.emojiId;
        status.emojiName = draft.emojiName;
        status.createdAtMs = draft.createdAtMs != 0 ? draft.createdAtMs : static_cast<uint64_t>(nowMs);

        const auto *expiryEl = expirySelect();
        const std::string expiryChoice = expiryEl ? expiryEl->GetValue() : "never";
        if (expiryChoice == "keep") {
            status.expiresAtMs = draft.expiresAtMs;
        } else if (expiryChoice == "today") {
            const auto nowTime = static_cast<std::time_t>(nowMs / 1000);
            std::tm localTm{};
#ifdef _WIN32
            localtime_s(&localTm, &nowTime);
#else
            localtime_r(&nowTime, &localTm);
#endif
            ++localTm.tm_mday;
            localTm.tm_hour = 0;
            localTm.tm_min = 0;
            localTm.tm_sec = 0;
            localTm.tm_isdst = -1;
            status.expiresAtMs = static_cast<uint64_t>(std::mktime(&localTm)) * 1000;
        } else {
            const int minutes = expiryChoice == "30m" ? 30 : expiryChoice == "1h" ? 60 : expiryChoice == "4h" ? 240 : 0;
            status.expiresAtMs = minutes != 0 ? static_cast<uint64_t>(nowMs + minutes * 60000LL) : 0;
        }
    }

    saving = true;
    error.clear();
    render();

    const int gen = ++generation;
    const Core::Snowflake accountId = editingAccountId;

    inst->discord()->setCustomStatus(payload, [this, guard = lifetime(), gen, accountId](const Core::Result<void> &result) {
        if (guard.expired() || gen != generation)
            return;

        auto *currentInst = shell.currentInstance();
        if (!currentInst || currentInst->accountId() != accountId)
            return;

        if (!result.success()) {
            saving = false;
            error = result.error;
            render();
            return;
        }

        close();
    });
}

::Rml::ElementFormControlInput *CustomStatusController::textInput() const
{
    if (!customStatusDocument)
        return nullptr;
    return dynamic_cast<::Rml::ElementFormControlInput *>(customStatusDocument->GetElementById("custom-status-text"));
}

::Rml::ElementFormControlSelect *CustomStatusController::expirySelect() const
{
    if (!customStatusDocument)
        return nullptr;
    return dynamic_cast<::Rml::ElementFormControlSelect *>(customStatusDocument->GetElementById("custom-status-expiry"));
}

::Rml::Element *CustomStatusController::emojiButton() const
{
    return customStatusDocument ? customStatusDocument->GetElementById("custom-status-emoji") : nullptr;
}

::Rml::Element *CustomStatusController::removeEmojiButton() const
{
    return customStatusDocument ? customStatusDocument->GetElementById("custom-status-remove-emoji") : nullptr;
}

::Rml::Element *CustomStatusController::clearButton() const
{
    return customStatusDocument ? customStatusDocument->GetElementById("custom-status-clear") : nullptr;
}

::Rml::Element *CustomStatusController::saveButton() const
{
    return customStatusDocument ? customStatusDocument->GetElementById("custom-status-save") : nullptr;
}

::Rml::Element *CustomStatusController::errorElement() const
{
    return customStatusDocument ? customStatusDocument->GetElementById("custom-status-error") : nullptr;
}

} // namespace Rml
} // namespace UI
} // namespace Styx
