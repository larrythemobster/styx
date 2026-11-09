#include "Keybinds.hpp"

#include "Core/SettingsKeys.hpp"

#include <format>
#include <optional>
#include <string>
#include <vector>

namespace Styx::UI::Rml {

namespace {

KeyBinding defaultShortcutBinding(ShortcutAction action)
{
    for (const auto &shortcut : shortcutTable()) {
        if (shortcut.action == action)
            return {shortcut.key, shortcut.modifiers};
    }
    return {};
}

bool isMaskSubset(int subset, int superset)
{
    return (subset & superset) == subset;
}

} // namespace

const KeyBinding &KeybindSet::get(KeybindAction action) const
{
    switch (action) {
    case KeybindAction::PushToTalk:
        return pushToTalk;
    case KeybindAction::ToggleMute:
        return toggleMute;
    case KeybindAction::ToggleDeafen:
        return toggleDeafen;
    }
    return pushToTalk;
}

const char *keybindName(KeybindAction action)
{
    switch (action) {
    case KeybindAction::PushToTalk:
        return "Push to Talk";
    case KeybindAction::ToggleMute:
        return "Toggle Mute";
    case KeybindAction::ToggleDeafen:
        return "Toggle Deafen";
    }
    return "";
}

KeybindSet parseKeybinds(const nlohmann::json &ptt, const nlohmann::json &mute,
                         const nlohmann::json &deafen)
{
    KeybindSet set;
    set.pushToTalk = keyBindingFromJson(ptt);
    set.toggleMute = mute.is_null() ? defaultShortcutBinding(ShortcutAction::ToggleMute) : keyBindingFromJson(mute);
    set.toggleDeafen = deafen.is_null() ? defaultShortcutBinding(ShortcutAction::ToggleDeafen) : keyBindingFromJson(deafen);
    return set;
}

KeybindSet loadKeybinds()
{
    namespace Settings = Core::Settings;
    return parseKeybinds(
        Settings::get(Settings::Keys::VoicePushToTalkKey),
        Settings::get(Settings::Keys::VoiceToggleMuteKey),
        Settings::get(Settings::Keys::VoiceToggleDeafenKey));
}

void storeKeybind(KeybindAction action, const KeyBinding &binding)
{
    namespace Settings = Core::Settings;
    const char *key = nullptr;
    switch (action) {
    case KeybindAction::PushToTalk:
        key = Settings::Keys::VoicePushToTalkKey;
        break;
    case KeybindAction::ToggleMute:
        key = Settings::Keys::VoiceToggleMuteKey;
        break;
    case KeybindAction::ToggleDeafen:
        key = Settings::Keys::VoiceToggleDeafenKey;
        break;
    }
    if (key)
        Settings::set(key, keyBindingToJson(binding.valid() ? binding : KeyBinding{}));
}

std::optional<KeybindAction> conflictingKeybind(KeybindAction target, const KeyBinding &binding,
                                                const KeybindSet &configured)
{
    if (!binding.valid())
        return std::nullopt;

    constexpr KeybindAction actions[] = {
        KeybindAction::PushToTalk,
        KeybindAction::ToggleMute,
        KeybindAction::ToggleDeafen
    };

    for (const auto action : actions) {
        if (action == target)
            continue;

        const auto &other = configured.get(action);
        if (!other.valid() || other.key != binding.key)
            continue;

        if (target == KeybindAction::PushToTalk) {
            if (isMaskSubset(binding.modifiers, other.modifiers))
                return action;
        } else if (action == KeybindAction::PushToTalk) {
            if (isMaskSubset(other.modifiers, binding.modifiers))
                return action;
        } else {
            if (binding.modifiers == other.modifiers)
                return action;
        }
    }
    return std::nullopt;
}

std::optional<ShortcutAction> conflictingFixedShortcut(KeybindAction target, const KeyBinding &binding)
{
    if (!binding.valid())
        return std::nullopt;

    for (const auto &shortcut : shortcutTable()) {
        if (shortcut.action == ShortcutAction::ToggleMute || shortcut.action == ShortcutAction::ToggleDeafen)
            continue;
        if (shortcut.key != binding.key)
            continue;

        if (target == KeybindAction::PushToTalk) {
            if (isMaskSubset(binding.modifiers, shortcut.modifiers))
                return shortcut.action;
        } else {
            if (binding.modifiers == shortcut.modifiers)
                return shortcut.action;
        }
    }
    return std::nullopt;
}

std::string keybindConflictText(KeybindAction target, const KeyBinding &binding,
                                const KeybindSet &configured)
{
    if (!binding.valid())
        return {};

    if (const auto conflict = conflictingKeybind(target, binding, configured)) {
        return std::format("{} overlaps {}. Choose another key.",
                           keyBindingLabel(binding), keybindName(*conflict));
    }

    if (const auto fixed = conflictingFixedShortcut(target, binding)) {
        for (const auto &shortcut : shortcutTable()) {
            if (shortcut.action == *fixed) {
                return std::format("{} overlaps {}. Choose another key.",
                                   keyBindingLabel(binding), shortcut.description);
            }
        }
    }

    return {};
}

std::optional<ShortcutAction> matchKeybindShortcut(int key, int rmlModifiers,
                                                  const KeybindSet &configured)
{
    if (keyBindingPressedExactly(configured.toggleMute, key, rmlModifiers))
        return ShortcutAction::ToggleMute;

    if (keyBindingPressedExactly(configured.toggleDeafen, key, rmlModifiers))
        return ShortcutAction::ToggleDeafen;

    const auto action = matchShortcut(key, rmlModifiers);
    if (action && *action != ShortcutAction::ToggleMute && *action != ShortcutAction::ToggleDeafen)
        return action;

    return std::nullopt;
}

std::vector<Shortcut> effectiveShortcuts(const KeybindSet &configured)
{
    std::vector<Shortcut> shortcuts;
    const auto &table = shortcutTable();
    shortcuts.reserve(table.size());
    for (const auto &shortcut : table) {
        const auto binding = shortcut.action == ShortcutAction::ToggleMute ? configured.toggleMute
            : shortcut.action == ShortcutAction::ToggleDeafen ? configured.toggleDeafen
            : KeyBinding{shortcut.key, shortcut.modifiers};
        if (binding.valid())
            shortcuts.push_back({shortcut.action, binding.key, binding.modifiers, shortcut.description});
    }
    return shortcuts;
}

} // namespace Styx::UI::Rml
