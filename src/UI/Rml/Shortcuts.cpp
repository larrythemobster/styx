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
    default:
        return "?";
    }
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
    std::string label;
#ifdef __APPLE__
    if (shortcut.modifiers & Primary)
        label += "Cmd+";
    if (shortcut.modifiers & Alt)
        label += "Option+";
#else
    if (shortcut.modifiers & Primary)
        label += "Ctrl+";
    if (shortcut.modifiers & Alt)
        label += "Alt+";
#endif
    if (shortcut.modifiers & Shift)
        label += "Shift+";

    label += keyName(shortcut.key);
    return label;
}

} // namespace Rml
} // namespace UI
} // namespace Styx
