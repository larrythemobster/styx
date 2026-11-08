#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Styx {
namespace UI {
namespace Rml {

enum class ShortcutAction {
    QuickSwitcher,
    Search,
    PreviousChannel,
    NextChannel,
    PreviousUnreadChannel,
    NextUnreadChannel,
    PreviousServer,
    NextServer,
    ScrollUp,
    ScrollDown,
    MarkChannelRead,
    ToggleMute,
    ToggleDeafen,
    ShortcutsHelp
};

// Modifier bits in Shortcut::modifiers. Primary is Ctrl, or Cmd on macOS, as in the official client.
enum ShortcutModifier : int {
    Primary = 1 << 0,
    Shift = 1 << 1,
    Alt = 1 << 2
};

struct Shortcut
{
    ShortcutAction action;
    // An Rml::Input::KeyIdentifier.
    int key;
    // ShortcutModifier bits that must be held, and no others.
    int modifiers;
    std::string_view description;
};

// Every in-app shortcut, in the order the help dialog lists them.
[[nodiscard]] const std::vector<Shortcut> &shortcutTable();

// The action for a key press, given the key and the Rml::Input::KeyModifier bits held.
// Modifiers must match exactly; Caps Lock and Num Lock are ignored.
[[nodiscard]] std::optional<ShortcutAction> matchShortcut(int key, int rmlModifiers);

// How the keys read in the help dialog: "Ctrl+Shift+M", or "Cmd+Shift+M" on macOS.
[[nodiscard]] std::string shortcutLabel(const Shortcut &shortcut);

} // namespace Rml
} // namespace UI
} // namespace Styx
