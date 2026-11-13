#include "Shortcuts.hpp"

#include <RmlUi/Core/Input.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Styx {
namespace UI {
namespace Rml {

namespace {

int toRmlModifiers(int modifiers)
{
    int rml = 0;
#ifdef __APPLE__
    if (modifiers & Primary)
        rml |= ::Rml::Input::KM_META;
#else
    if (modifiers & Primary)
        rml |= ::Rml::Input::KM_CTRL;
#endif
    if (modifiers & Shift)
        rml |= ::Rml::Input::KM_SHIFT;
    if (modifiers & Alt)
        rml |= ::Rml::Input::KM_ALT;
    return rml;
}

std::string keyName(int key)
{
    if (key >= ::Rml::Input::KI_A && key <= ::Rml::Input::KI_Z)
        return std::string(1, static_cast<char>('A' + (key - ::Rml::Input::KI_A)));
    if (key >= ::Rml::Input::KI_F1 && key <= ::Rml::Input::KI_F24)
        return "F" + std::to_string(key - ::Rml::Input::KI_F1 + 1);
    if (key >= ::Rml::Input::KI_0 && key <= ::Rml::Input::KI_9)
        return std::string(1, static_cast<char>('0' + (key - ::Rml::Input::KI_0)));

    switch (key) {
    case ::Rml::Input::KI_UP:
        return "Up";
    case ::Rml::Input::KI_DOWN:
        return "Down";
    case ::Rml::Input::KI_PRIOR:
        return "Page Up";
    case ::Rml::Input::KI_NEXT:
        return "Page Down";
    case ::Rml::Input::KI_ESCAPE:
        return "Escape";
    case ::Rml::Input::KI_OEM_2:
        return "/";
    case ::Rml::Input::KI_OEM_PLUS:
        return "=";
    case ::Rml::Input::KI_OEM_MINUS:
        return "-";
    case ::Rml::Input::KI_OEM_3:
        return "`";
    case ::Rml::Input::KI_LSHIFT:
        return "Left Shift";
    case ::Rml::Input::KI_RSHIFT:
        return "Right Shift";
    case ::Rml::Input::KI_LCONTROL:
        return "Left Ctrl";
    case ::Rml::Input::KI_RCONTROL:
        return "Right Ctrl";
#ifdef __APPLE__
    case ::Rml::Input::KI_LMETA:
        return "Left Cmd";
    case ::Rml::Input::KI_RMETA:
        return "Right Cmd";
    case ::Rml::Input::KI_LMENU:
        return "Left Option";
    case ::Rml::Input::KI_RMENU:
        return "Right Option";
#else
    case ::Rml::Input::KI_LMENU:
        return "Left Alt";
    case ::Rml::Input::KI_RMENU:
        return "Right Alt";
#endif
    default:
        return "Key " + std::to_string(key);
    }
}

int ownRmlModifier(int key)
{
    if (key == ::Rml::Input::KI_LCONTROL || key == ::Rml::Input::KI_RCONTROL)
        return ::Rml::Input::KM_CTRL;
    if (key == ::Rml::Input::KI_LSHIFT || key == ::Rml::Input::KI_RSHIFT)
        return ::Rml::Input::KM_SHIFT;
    if (key == ::Rml::Input::KI_LMENU || key == ::Rml::Input::KI_RMENU)
        return ::Rml::Input::KM_ALT;
    if (key == ::Rml::Input::KI_LMETA || key == ::Rml::Input::KI_RMETA)
        return ::Rml::Input::KM_META;
    return 0;
}

int ownModifier(int key)
{
    return shortcutModifiers(ownRmlModifier(key));
}

std::string comboLabel(int key, int modifiers)
{
    std::string label;
#ifdef __APPLE__
    if (modifiers & Primary)
        label += "Cmd+";
    if (modifiers & Alt)
        label += "Option+";
#else
    if (modifiers & Primary)
        label += "Ctrl+";
    if (modifiers & Alt)
        label += "Alt+";
#endif
    if (modifiers & Shift)
        label += "Shift+";

    label += keyName(key);
    return label;
}

} // namespace

const std::vector<Shortcut> &shortcutTable()
{
    static const std::vector<Shortcut> shortcuts = {
        {ShortcutAction::QuickSwitcher, ::Rml::Input::KI_K, Primary, "Find or start a conversation"},
        {ShortcutAction::Search, ::Rml::Input::KI_F, Primary, "Search messages"},
        {ShortcutAction::PreviousChannel, ::Rml::Input::KI_UP, Alt, "Previous channel"},
        {ShortcutAction::NextChannel, ::Rml::Input::KI_DOWN, Alt, "Next channel"},
        {ShortcutAction::PreviousUnreadChannel, ::Rml::Input::KI_UP, Alt | Shift, "Previous unread channel"},
        {ShortcutAction::NextUnreadChannel, ::Rml::Input::KI_DOWN, Alt | Shift, "Next unread channel"},
        {ShortcutAction::PreviousServer, ::Rml::Input::KI_UP, Primary | Alt, "Previous server"},
        {ShortcutAction::NextServer, ::Rml::Input::KI_DOWN, Primary | Alt, "Next server"},
        {ShortcutAction::ScrollUp, ::Rml::Input::KI_PRIOR, 0, "Scroll messages up"},
        {ShortcutAction::ScrollDown, ::Rml::Input::KI_NEXT, 0, "Scroll messages down"},
        {ShortcutAction::MarkChannelRead, ::Rml::Input::KI_ESCAPE, 0, "Mark channel as read"},
        {ShortcutAction::ToggleMute, ::Rml::Input::KI_M, Primary | Shift, "Toggle mute"},
        {ShortcutAction::ToggleDeafen, ::Rml::Input::KI_D, Primary | Shift, "Toggle deafen"},
        {ShortcutAction::ShortcutsHelp, ::Rml::Input::KI_OEM_2, Primary, "Show keyboard shortcuts"},
        {ShortcutAction::ZoomIn, ::Rml::Input::KI_OEM_PLUS, Primary, "Zoom in"},
        {ShortcutAction::ZoomOut, ::Rml::Input::KI_OEM_MINUS, Primary, "Zoom out"},
        {ShortcutAction::ZoomReset, ::Rml::Input::KI_0, Primary, "Reset zoom"},
    };
    return shortcuts;
}

std::optional<ShortcutAction> matchShortcut(int key, int rmlModifiers)
{
    const int effectiveModifiers =
        rmlModifiers & ~(::Rml::Input::KM_CAPSLOCK | ::Rml::Input::KM_NUMLOCK | ::Rml::Input::KM_SCROLLLOCK);
    for (const auto &shortcut : shortcutTable()) {
        if (shortcut.key == key && toRmlModifiers(shortcut.modifiers) == effectiveModifiers)
            return shortcut.action;
    }
    return std::nullopt;
}

std::string shortcutLabel(const Shortcut &shortcut)
{
    return comboLabel(shortcut.key, shortcut.modifiers);
}

int shortcutModifiers(int rmlModifiers)
{
    int mods = 0;
#ifdef __APPLE__
    if (rmlModifiers & ::Rml::Input::KM_META)
        mods |= Primary;
#else
    if (rmlModifiers & ::Rml::Input::KM_CTRL)
        mods |= Primary;
#endif
    if (rmlModifiers & ::Rml::Input::KM_SHIFT)
        mods |= Shift;
    if (rmlModifiers & ::Rml::Input::KM_ALT)
        mods |= Alt;
    return mods;
}

KeyBinding recordKeyBinding(int key, int rmlModifiers)
{
    return {key, shortcutModifiers(rmlModifiers) & ~ownModifier(key)};
}

bool isModifierKey(int key)
{
    return ownModifier(key) != 0;
}

bool keyBindingPressed(const KeyBinding &binding, int key, int rmlModifiers)
{
    return binding.valid() && key == binding.key && (shortcutModifiers(rmlModifiers) & binding.modifiers) == binding.modifiers;
}

bool keyBindingPressedExactly(const KeyBinding &binding, int key, int rmlModifiers)
{
    if (!binding.valid() || key != binding.key)
        return false;
    int effectiveModifiers =
        rmlModifiers & ~(::Rml::Input::KM_CAPSLOCK | ::Rml::Input::KM_NUMLOCK | ::Rml::Input::KM_SCROLLLOCK);
    effectiveModifiers &= ~ownRmlModifier(key);
    return effectiveModifiers == toRmlModifiers(binding.modifiers);
}

std::string keyBindingLabel(const KeyBinding &binding)
{
    if (!binding.valid())
        return {};
    return comboLabel(binding.key, binding.modifiers);
}

nlohmann::json keyBindingToJson(const KeyBinding &binding)
{
    return nlohmann::json{
        {"key", binding.key},
        {"modifiers", binding.modifiers}
    };
}

KeyBinding keyBindingFromJson(const nlohmann::json &json)
{
    if (!json.is_object())
        return {};
    const auto keyIt = json.find("key");
    if (keyIt == json.end() || !keyIt->is_number_integer())
        return {};
    const int key = keyIt->get<int>();
    if (key <= 0)
        return {};

    int modifiers = 0;
    const auto modIt = json.find("modifiers");
    if (modIt != json.end()) {
        if (!modIt->is_number_integer())
            return {};
        modifiers = modIt->get<int>();
    }

    return {key, modifiers};
}

} // namespace Rml
} // namespace UI
} // namespace Styx
