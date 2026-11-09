#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

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

// Default shortcuts, in help-dialog order; effectiveShortcuts applies voice binding overrides.
[[nodiscard]] const std::vector<Shortcut> &shortcutTable();

// The action for a key press, given the key and the Rml::Input::KeyModifier bits held.
// Modifiers must match exactly; Caps Lock and Num Lock are ignored.
[[nodiscard]] std::optional<ShortcutAction> matchShortcut(int key, int rmlModifiers);

// How the keys read in the help dialog: "Ctrl+Shift+M", or "Cmd+Shift+M" on macOS.
[[nodiscard]] std::string shortcutLabel(const Shortcut &shortcut);

// A key the user recorded, such as the push-to-talk key: an Rml::Input::KeyIdentifier plus
// ShortcutModifier bits that must be held with it.
struct KeyBinding
{
    int key = 0;
    int modifiers = 0;

    [[nodiscard]] bool valid() const { return key != 0; }
    bool operator==(const KeyBinding &) const = default;
};

// ShortcutModifier bits for an Rml::Input::KeyModifier state (Ctrl, or Cmd on macOS, is Primary).
[[nodiscard]] int shortcutModifiers(int rmlModifiers);
// The binding a key press records. A modifier key bound on its own (Left Ctrl) doesn't also
// require its own modifier bit, which the press reports as held.
[[nodiscard]] KeyBinding recordKeyBinding(int key, int rmlModifiers);
// Ctrl, Shift or Alt (Cmd and Option on macOS). A recorder waits for a modifier's release
// before binding it alone, so it can still be held for a combination.
[[nodiscard]] bool isModifierKey(int key);
// Whether a key press triggers the binding: the same key with at least its modifiers held.
[[nodiscard]] bool keyBindingPressed(const KeyBinding &binding, int key, int rmlModifiers);
// Whether a key press matches the binding exactly: the same key with exact modifiers held.
// Ignores lock keys and strips a lone modifier key's own modifier bit.
[[nodiscard]] bool keyBindingPressedExactly(const KeyBinding &binding, int key, int rmlModifiers);
// "Ctrl+`", "Shift+F13", "Left Ctrl"; empty for an unset binding.
[[nodiscard]] std::string keyBindingLabel(const KeyBinding &binding);
// Stored in settings as {"key": n, "modifiers": n}; anything else reads as unset.
[[nodiscard]] nlohmann::json keyBindingToJson(const KeyBinding &binding);
[[nodiscard]] KeyBinding keyBindingFromJson(const nlohmann::json &json);

} // namespace Rml
} // namespace UI
} // namespace Styx
