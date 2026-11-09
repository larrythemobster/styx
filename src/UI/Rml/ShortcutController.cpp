#include "ShortcutController.hpp"
#include "UI/PushToTalk.hpp"

#include "Shell.hpp"

#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

#include <utility>

namespace Styx {
namespace UI {
namespace Rml {
KeybindSet ShortcutController::cachedKeybinds = parseKeybinds(nullptr, nullptr, nullptr);

ShortcutController::ShortcutController(Shell &shell)
    : Controller(shell, nullptr)
{
    reloadKeybinds();

    track(this->shell.focusLost.connect([this] {
        downKeys.fill(false);
    }));

    if (auto *doc = shell.document()) {
        listen("keydown", [this](::Rml::Event &event) {
            const int key = event.GetParameter<int>("key_identifier", 0);
            const bool wasDown = (key >= 0 && key < static_cast<int>(downKeys.size())) ? downKeys[key] : false;
            if (key >= 0 && key < static_cast<int>(downKeys.size()))
                downKeys[key] = true;

            if (auto *ptt = this->shell.pushToTalk(); ptt && ptt->isSuspended())
                return;

            const auto action = actionFor(event);
            if (!action)
                return;

            if (wasDown && (*action == ShortcutAction::ToggleMute || *action == ShortcutAction::ToggleDeafen)) {
                event.StopPropagation();
                return;
            }

            const auto it = handlers.find(*action);
            if (it != handlers.end() && it->second())
                event.StopPropagation();
        }, doc, true);

        listen("keyup", [this](::Rml::Event &event) {
            const int key = event.GetParameter<int>("key_identifier", 0);
            if (key >= 0 && key < static_cast<int>(downKeys.size()))
                downKeys[key] = false;
        }, doc, true);
    }
}

void ShortcutController::reloadKeybinds()
{
    cachedKeybinds = loadKeybinds();
}

const KeybindSet &ShortcutController::keybinds()
{
    return cachedKeybinds;
}

void ShortcutController::setHandler(ShortcutAction action, std::function<bool()> handler)
{
    handlers.insert_or_assign(action, std::move(handler));
}

bool ShortcutController::hasHandler(ShortcutAction action) const
{
    return handlers.contains(action);
}

std::optional<ShortcutAction> ShortcutController::actionFor(::Rml::Event &event)
{
    return matchKeybindShortcut(event.GetParameter<int>("key_identifier", 0), eventModifiers(event), cachedKeybinds);
}

int ShortcutController::eventModifiers(::Rml::Event &event)
{
    int modifiers = 0;
    if (event.GetParameter<int>("ctrl_key", 0) != 0)
        modifiers |= ::Rml::Input::KM_CTRL;
    if (event.GetParameter<int>("shift_key", 0) != 0)
        modifiers |= ::Rml::Input::KM_SHIFT;
    if (event.GetParameter<int>("alt_key", 0) != 0)
        modifiers |= ::Rml::Input::KM_ALT;
    if (event.GetParameter<int>("meta_key", 0) != 0)
        modifiers |= ::Rml::Input::KM_META;
    return modifiers;
}

} // namespace Rml
} // namespace UI
} // namespace Styx
