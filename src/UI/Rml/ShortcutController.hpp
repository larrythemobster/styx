#pragma once

#include "Controller.hpp"
#include "Keybinds.hpp"
#include "Shortcuts.hpp"

#include <array>
#include <functional>
#include <optional>
#include <unordered_map>

namespace Rml {
class Event;
}

namespace Styx {
namespace UI {
namespace Rml {

// Runs the handler registered for a shortcut when its keys are pressed anywhere in the main
// window. It listens in the capture phase so a focused text box can't swallow the keys first.
class ShortcutController final : public Controller
{
public:
    explicit ShortcutController(Shell &shell);

    static void reloadKeybinds();
    [[nodiscard]] static const KeybindSet &keybinds();

    // One handler per action; setting it again replaces the old one. A handler returns whether
    // it acted: when it didn't, or there's no handler, the key reaches the focused element as
    // usual. Bare keys such as Page Up rely on this to leave text boxes alone when idle.
    void setHandler(ShortcutAction action, std::function<bool()> handler);
    [[nodiscard]] bool hasHandler(ShortcutAction action) const;

    // The action a keydown event asks for, for popovers in their own documents that the main
    // listener doesn't see.
    [[nodiscard]] static std::optional<ShortcutAction> actionFor(::Rml::Event &event);
    // The Rml::Input::KeyModifier state a key event reports.
    [[nodiscard]] static int eventModifiers(::Rml::Event &event);

private:
    static KeybindSet cachedKeybinds;
    std::unordered_map<ShortcutAction, std::function<bool()>> handlers;
    std::array<bool, 256> downKeys{};
};

} // namespace Rml
} // namespace UI
} // namespace Styx
