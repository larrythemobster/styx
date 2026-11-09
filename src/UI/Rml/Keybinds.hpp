#pragma once

#include "Shortcuts.hpp"

#include <optional>
#include <string>
#include <vector>

namespace Styx::UI::Rml {

enum class KeybindAction {
    PushToTalk = 0,
    ToggleMute = 1,
    ToggleDeafen = 2
};

struct KeybindSet
{
    KeyBinding pushToTalk;
    KeyBinding toggleMute;
    KeyBinding toggleDeafen;
    [[nodiscard]] const KeyBinding &get(KeybindAction action) const;
};

[[nodiscard]] const char *keybindName(KeybindAction action);
// Missing mute/deafen preferences retain their built-in defaults. An explicitly stored unset
// binding disables the action, so removing a binding survives restarting the application.
[[nodiscard]] KeybindSet parseKeybinds(const nlohmann::json &ptt, const nlohmann::json &mute,
                                      const nlohmann::json &deafen);
[[nodiscard]] KeybindSet loadKeybinds();
void storeKeybind(KeybindAction action, const KeyBinding &binding);

// PTT accepts additional modifiers; toggle shortcuts require exact modifiers. Conflict checks
// use those rules so a bare PTT key cannot overlap a toggle using the same key plus modifiers.
[[nodiscard]] std::optional<KeybindAction> conflictingKeybind(KeybindAction target, const KeyBinding &binding,
                                                            const KeybindSet &configured);
// Fixed shortcuts require exact modifiers. Mute/deafen defaults are excluded here because
// their effective bindings are checked by conflictingKeybind.
[[nodiscard]] std::optional<ShortcutAction> conflictingFixedShortcut(KeybindAction target, const KeyBinding &binding);
[[nodiscard]] std::string keybindConflictText(KeybindAction target, const KeyBinding &binding,
                                            const KeybindSet &configured);

// No settings reads or allocations on the key-event path. Mute/deafen overrides replace the
// defaults; a removed override must not fall through to its old built-in shortcut.
[[nodiscard]] std::optional<ShortcutAction> matchKeybindShortcut(int key, int rmlModifiers,
                                                              const KeybindSet &configured);
[[nodiscard]] std::vector<Shortcut> effectiveShortcuts(const KeybindSet &configured);

} // namespace Styx::UI::Rml
