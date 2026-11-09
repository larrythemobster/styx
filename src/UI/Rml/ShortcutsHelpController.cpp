#include "ShortcutsHelpController.hpp"

#include "Platform/Native.hpp"
#include "RmlView.hpp"
#include "Shell.hpp"
#include "ShortcutController.hpp"
#include "Shortcuts.hpp"

#include <format>
#include <string>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

namespace Styx {
namespace UI {
namespace Rml {

ShortcutsHelpController::ShortcutsHelpController(Shell &shell)
    : Controller(shell, nullptr)
{
}

ShortcutsHelpController::~ShortcutsHelpController()
{
    if (helpDocument) {
        helpDocument->Close();
        helpDocument = nullptr;
    }
}

void ShortcutsHelpController::open()
{
    ensureDocument();
    if (!helpDocument)
        return;

    visible = true;
    helpDocument->Show(::Rml::ModalFlag::None);
    requestRender();
}

void ShortcutsHelpController::close()
{
    if (!visible)
        return;

    visible = false;
    if (helpDocument)
        helpDocument->Hide();
    requestRender();
}

void ShortcutsHelpController::ensureDocument()
{
    if (helpDocument)
        return;

    auto *view = shell.view();
    if (!view || !view->context())
        return;

    helpDocument = view->context()->LoadDocument("ui/shortcuts_help.rml");
    if (!helpDocument)
        return;

    regionRoot = helpDocument;

    if (auto *listEl = helpDocument->GetElementById("shortcuts-help-list")) {
        std::string html;
        for (const auto &shortcut : shortcutTable()) {
            html += std::format(
                "<div class=\"shortcuts-help-row\">"
                "<span class=\"shortcuts-help-description\">{}</span>"
                "<span class=\"shortcuts-help-keys\">{}</span>"
                "</div>",
                Platform::htmlEscape(shortcut.description),
                Platform::htmlEscape(shortcutLabel(shortcut)));
        }
        listEl->SetInnerRML(html);
    }

    setupEventListeners();
}

void ShortcutsHelpController::setupEventListeners()
{
    if (!helpDocument)
        return;

    listen("click", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        if (!target)
            return;

        auto *actionEl = closest(target, "data-action");
        if (!actionEl)
            return;

        if (attribute(actionEl, "data-action") == "close")
            close();
    });

    listen("keydown", [this](::Rml::Event &event) {
        if (ShortcutController::actionFor(event) == ShortcutAction::ShortcutsHelp) {
            event.StopPropagation();
            close();
            return;
        }

        const int key = event.GetParameter<int>("key_identifier", 0);
        if (key == ::Rml::Input::KI_ESCAPE) {
            event.StopPropagation();
            close();
        }
    });
}

} // namespace Rml
} // namespace UI
} // namespace Styx
