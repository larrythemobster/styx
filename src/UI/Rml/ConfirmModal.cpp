#include "ConfirmModal.hpp"

#include "Shell.hpp"
#include "RmlView.hpp"
#include "Platform/Native.hpp"
#include "Platform/Utf8.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

namespace Styx {
namespace UI {
namespace Rml {


ConfirmModalController::ConfirmModalController(Shell &shell)
    : Controller(shell, nullptr)
{
}

ConfirmModalController::~ConfirmModalController()
{
    removeListeners();
    if (modalDocument) {
        modalDocument->Close();
        modalDocument = nullptr;
    }
}

void ConfirmModalController::ensureDocument()
{
    if (modalDocument || !shell.view() || !shell.view()->context())
        return;

    modalDocument = shell.view()->context()->LoadDocument("ui/confirm_modal.rml");
    if (!modalDocument)
        return;

    regionRoot = modalDocument;

    listen("click", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        if (!target)
            return;

        if (target->GetId() == "confirm-cancel-btn" || target->GetId() == "confirm-backdrop") {
            event.StopPropagation();
            auto cb = std::move(currentOptions.onCancel);
            hide();
            if (cb)
                cb();
        } else if (target->GetId() == "confirm-action-btn") {
            event.StopPropagation();
            auto cb = std::move(currentOptions.onConfirm);
            hide();
            if (cb)
                cb();
        }
    }, modalDocument);

    listen("keydown", [this](::Rml::Event &event) {
        const auto key = static_cast<::Rml::Input::KeyIdentifier>(
            event.GetParameter<int>("key_identifier", 0));
        if (key == ::Rml::Input::KI_ESCAPE) {
            event.StopPropagation();
            auto cb = std::move(currentOptions.onCancel);
            hide();
            if (cb)
                cb();
        } else if (key == ::Rml::Input::KI_RETURN || key == ::Rml::Input::KI_NUMPADENTER) {
            event.StopPropagation();
            auto cb = std::move(currentOptions.onConfirm);
            hide();
            if (cb)
                cb();
        }
    }, modalDocument);
}

void ConfirmModalController::updateContent()
{
    if (!modalDocument)
        return;

    if (auto *titleEl = modalDocument->GetElementById("confirm-title"))
        titleEl->SetInnerRML(Platform::htmlEscape(currentOptions.title));

    if (auto *msgEl = modalDocument->GetElementById("confirm-message")) {
        auto rml = Platform::replace(currentOptions.body, "<br>", "<br/>");
        msgEl->SetInnerRML(rml);
    }

    if (auto *cancelBtn = modalDocument->GetElementById("confirm-cancel-btn")) {
        cancelBtn->SetInnerRML(Platform::htmlEscape(currentOptions.cancelText));
        cancelBtn->SetProperty("display", currentOptions.cancelText.empty() ? "none" : "block");
    }

    if (auto *actionBtn = modalDocument->GetElementById("confirm-action-btn")) {
        actionBtn->SetInnerRML(Platform::htmlEscape(currentOptions.confirmText));
        actionBtn->SetClass("danger", currentOptions.danger);
    }
}

void ConfirmModalController::show(const ConfirmOptions &options)
{
    ensureDocument();
    if (!modalDocument)
        return;

    currentOptions = options;
    updateContent();

    modalDocument->Show(::Rml::ModalFlag::Modal);
    modalDocument->PullToFront();
    visible = true;
    requestRender();
}

void ConfirmModalController::hide()
{
    if (!modalDocument || !visible)
        return;

    modalDocument->Hide();
    visible = false;
    requestRender();
}

void showConfirmModal(Shell &shell, const ConfirmOptions &options)
{
    if (auto *controller = shell.confirmModal())
        controller->show(options);
}

void showConfirmModal(const ConfirmOptions &options)
{
    if (auto *primary = Shell::primaryShell())
        showConfirmModal(*primary, options);
}

} // namespace Rml
} // namespace UI
} // namespace Styx
