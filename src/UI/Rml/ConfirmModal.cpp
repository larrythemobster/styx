#include "ConfirmModal.hpp"

#include "Shell.hpp"
#include "RmlView.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

namespace Styx {
namespace UI {
namespace Rml {

static ConfirmModalController *s_activeConfirmController = nullptr;

ConfirmModalController::ConfirmModalController(Shell &shell, QObject *parent)
    : Controller(shell, nullptr, parent)
{
    s_activeConfirmController = this;
}

ConfirmModalController::~ConfirmModalController()
{
    if (s_activeConfirmController == this)
        s_activeConfirmController = nullptr;
    if (modalDocument) {
        modalDocument->Close();
        modalDocument = nullptr;
    }
}

void ConfirmModalController::ensureDocument()
{
    if (modalDocument || !shell.view() || !shell.view()->context())
        return;

    modalDocument = shell.view()->context()->LoadDocument(":/ui/confirm_modal.rml");
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
        titleEl->SetInnerRML(currentOptions.title.toHtmlEscaped().toStdString());

    if (auto *msgEl = modalDocument->GetElementById("confirm-message")) {
        // Convert basic Qt HTML linebreaks / paragraphs if present
        QString rml = currentOptions.body;
        rml.replace(QStringLiteral("<br>"), QStringLiteral("<br/>"));
        msgEl->SetInnerRML(rml.toStdString());
    }

    if (auto *cancelBtn = modalDocument->GetElementById("confirm-cancel-btn"))
        cancelBtn->SetInnerRML(currentOptions.cancelText.toHtmlEscaped().toStdString());

    if (auto *actionBtn = modalDocument->GetElementById("confirm-action-btn")) {
        actionBtn->SetInnerRML(currentOptions.confirmText.toHtmlEscaped().toStdString());
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
    modalDocument->SetClass("open", true);
    visible = true;
    requestRender();
}

void ConfirmModalController::hide()
{
    if (!modalDocument || !visible)
        return;

    modalDocument->SetClass("open", false);
    modalDocument->Hide();
    visible = false;
    requestRender();
}

void showConfirmModal(Shell &shell, const ConfirmOptions &options)
{
    if (s_activeConfirmController) {
        s_activeConfirmController->show(options);
        return;
    }
    // Fallback: search shell's controllers or construct
    auto controller = std::make_unique<ConfirmModalController>(shell);
    controller->show(options);
}

void showConfirmModal(const ConfirmOptions &options)
{
    if (s_activeConfirmController) {
        s_activeConfirmController->show(options);
        return;
    }
    if (auto *primary = Shell::primaryShell()) {
        showConfirmModal(*primary, options);
    }
}

} // namespace Rml
} // namespace UI
} // namespace Styx
