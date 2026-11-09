#include <catch2/catch_test_macros.hpp>

#include "UI/Rml/Shortcuts.hpp"

#include <RmlUi/Core/Input.h>

#include <set>
#include <string>
#include <utility>

using namespace Styx::UI::Rml;
namespace KI = ::Rml::Input;

#ifdef __APPLE__
constexpr int PRIMARY = KI::KM_META;
#else
constexpr int PRIMARY = KI::KM_CTRL;
#endif

static const Shortcut &findShortcut(ShortcutAction action)
{
    for (const auto &shortcut : shortcutTable()) {
        if (shortcut.action == action)
            return shortcut;
    }
    FAIL("action missing from the shortcut table");
    return shortcutTable().front();
}

TEST_CASE("Primary shortcuts match expected actions", "[shortcuts]")
{
    REQUIRE(matchShortcut(KI::KI_K, PRIMARY) == ShortcutAction::QuickSwitcher);
    REQUIRE(matchShortcut(KI::KI_F, PRIMARY) == ShortcutAction::Search);
    REQUIRE(matchShortcut(KI::KI_M, PRIMARY | KI::KM_SHIFT) == ShortcutAction::ToggleMute);
    REQUIRE(matchShortcut(KI::KI_OEM_2, PRIMARY) == ShortcutAction::ShortcutsHelp);
}

TEST_CASE("Modifiers must match exactly", "[shortcuts]")
{
    REQUIRE_FALSE(matchShortcut(KI::KI_K, 0).has_value());
    REQUIRE_FALSE(matchShortcut(KI::KI_K, PRIMARY | KI::KM_SHIFT).has_value());
    REQUIRE_FALSE(matchShortcut(KI::KI_K, PRIMARY | KI::KM_ALT).has_value());

    REQUIRE(matchShortcut(KI::KI_UP, KI::KM_ALT) == ShortcutAction::PreviousChannel);
    REQUIRE(matchShortcut(KI::KI_UP, KI::KM_ALT | KI::KM_SHIFT) == ShortcutAction::PreviousUnreadChannel);
    REQUIRE(matchShortcut(KI::KI_UP, PRIMARY | KI::KM_ALT) == ShortcutAction::PreviousServer);
}

TEST_CASE("Lock keys are ignored during matching", "[shortcuts]")
{
    REQUIRE(matchShortcut(KI::KI_K, PRIMARY | KI::KM_CAPSLOCK | KI::KM_NUMLOCK) == ShortcutAction::QuickSwitcher);
}

TEST_CASE("Navigation keys require exact modifier state", "[shortcuts]")
{
    REQUIRE(matchShortcut(KI::KI_NEXT, 0) == ShortcutAction::ScrollDown);
    REQUIRE_FALSE(matchShortcut(KI::KI_NEXT, KI::KM_SHIFT).has_value());
}

TEST_CASE("Every action appears once and key bindings are unique", "[shortcuts]")
{
    const auto &table = shortcutTable();
    std::set<ShortcutAction> actions;
    std::set<std::pair<int, int>> bindings;

    for (const auto &shortcut : table) {
        REQUIRE(actions.insert(shortcut.action).second);
        REQUIRE(bindings.insert({shortcut.key, shortcut.modifiers}).second);
    }
}

TEST_CASE("Shortcut labels format keys and modifiers", "[shortcuts]")
{
#ifndef __APPLE__
    REQUIRE(shortcutLabel(findShortcut(ShortcutAction::QuickSwitcher)) == "Ctrl+K");
    REQUIRE(shortcutLabel(findShortcut(ShortcutAction::PreviousServer)) == "Ctrl+Alt+Up");
    REQUIRE(shortcutLabel(findShortcut(ShortcutAction::NextUnreadChannel)) == "Alt+Shift+Down");
    REQUIRE(shortcutLabel(findShortcut(ShortcutAction::ScrollDown)) == "Page Down");
    REQUIRE(shortcutLabel(findShortcut(ShortcutAction::ShortcutsHelp)) == "Ctrl+/");
#else
    REQUIRE(shortcutLabel(findShortcut(ShortcutAction::QuickSwitcher)) == "Cmd+K");
    REQUIRE(shortcutLabel(findShortcut(ShortcutAction::PreviousServer)) == "Cmd+Option+Up");
    REQUIRE(shortcutLabel(findShortcut(ShortcutAction::NextUnreadChannel)) == "Option+Shift+Down");
    REQUIRE(shortcutLabel(findShortcut(ShortcutAction::ScrollDown)) == "Page Down");
    REQUIRE(shortcutLabel(findShortcut(ShortcutAction::ShortcutsHelp)) == "Cmd+/");
#endif
}

TEST_CASE("Key bindings record pressed key and modifiers", "[shortcuts]")
{
    REQUIRE(recordKeyBinding(KI::KI_F13, PRIMARY) == KeyBinding{KI::KI_F13, Primary});
    REQUIRE(recordKeyBinding(KI::KI_LCONTROL, KI::KM_CTRL) == KeyBinding{KI::KI_LCONTROL, 0});
    REQUIRE(isModifierKey(KI::KI_RSHIFT));
    REQUIRE(isModifierKey(KI::KI_LMENU));
    REQUIRE_FALSE(isModifierKey(KI::KI_F13));
    REQUIRE_FALSE(isModifierKey(KI::KI_OEM_3));
}

TEST_CASE("Key bindings match matching presses", "[shortcuts]")
{
    const KeyBinding backquote{KI::KI_OEM_3, 0};
    REQUIRE(keyBindingPressed(backquote, KI::KI_OEM_3, 0));
    REQUIRE(keyBindingPressed(backquote, KI::KI_OEM_3, KI::KM_SHIFT));

    const KeyBinding f13Primary{KI::KI_F13, Primary};
    REQUIRE_FALSE(keyBindingPressed(f13Primary, KI::KI_F13, 0));
    REQUIRE(keyBindingPressed(f13Primary, KI::KI_F13, PRIMARY));

    REQUIRE_FALSE(keyBindingPressed(f13Primary, KI::KI_F12, PRIMARY));
    REQUIRE_FALSE(keyBindingPressed(backquote, KI::KI_ESCAPE, 0));

    const KeyBinding unset;
    REQUIRE_FALSE(keyBindingPressed(unset, KI::KI_F13, PRIMARY));
    REQUIRE_FALSE(keyBindingPressed(unset, KI::KI_OEM_3, 0));
}

TEST_CASE("Key binding labels format keys and modifiers", "[shortcuts]")
{
#ifndef __APPLE__
    REQUIRE(keyBindingLabel({KI::KI_F13, Primary | Shift}) == "Ctrl+Shift+F13");
#else
    REQUIRE(keyBindingLabel({KI::KI_F13, Primary | Shift}) == "Cmd+Shift+F13");
#endif
    REQUIRE(keyBindingLabel({KI::KI_LCONTROL, 0}) == "Left Ctrl");
    REQUIRE(keyBindingLabel({}) == "");
}

TEST_CASE("Key binding serialization matches expected JSON", "[shortcuts]")
{
    const std::string expected =
        "{\"key\":" + std::to_string(KI::KI_F13) + ",\"modifiers\":" + std::to_string(Primary) + "}";
    const KeyBinding binding{KI::KI_F13, Primary};
    REQUIRE(keyBindingToJson(binding).dump() == expected);
    REQUIRE(keyBindingFromJson(nlohmann::json::parse(expected)) == binding);

    REQUIRE(keyBindingFromJson(nullptr) == KeyBinding{});
    REQUIRE(keyBindingFromJson("x") == KeyBinding{});
    REQUIRE(keyBindingFromJson(nlohmann::json{{"key", 0}}) == KeyBinding{});
    REQUIRE(keyBindingFromJson(nlohmann::json{{"key", "a"}}) == KeyBinding{});
}
