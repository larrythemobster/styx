#include <catch2/catch_test_macros.hpp>

#include "UI/Rml/Keybinds.hpp"
#include "UI/Rml/Shortcuts.hpp"

#include <RmlUi/Core/Input.h>

#include <optional>
#include <vector>

using namespace Styx::UI::Rml;
namespace KI = ::Rml::Input;

#ifdef __APPLE__
constexpr int PRIMARY = KI::KM_META;
#else
constexpr int PRIMARY = KI::KM_CTRL;
#endif

namespace {

const Shortcut &findShortcutInList(const std::vector<Shortcut> &shortcuts, ShortcutAction action)
{
    for (const auto &shortcut : shortcuts) {
        if (shortcut.action == action)
            return shortcut;
    }
    FAIL("Action missing from shortcut list");
    return shortcuts.front();
}

bool listContainsAction(const std::vector<Shortcut> &shortcuts, ShortcutAction action)
{
    for (const auto &shortcut : shortcuts) {
        if (shortcut.action == action)
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("Missing settings preserve existing voice shortcuts", "[keybinds]")
{
    const auto set = parseKeybinds(nullptr, nullptr, nullptr);
    REQUIRE_FALSE(set.pushToTalk.valid());
    REQUIRE(matchKeybindShortcut(KI::KI_M, PRIMARY | KI::KM_SHIFT, set) == ShortcutAction::ToggleMute);
    REQUIRE(matchKeybindShortcut(KI::KI_D, PRIMARY | KI::KM_SHIFT, set) == ShortcutAction::ToggleDeafen);
}

TEST_CASE("Explicit unset survives parse", "[keybinds]")
{
    const nlohmann::json unsetObj = {{"key", 0}, {"modifiers", 0}};
    const auto set = parseKeybinds(unsetObj, unsetObj, unsetObj);
    REQUIRE_FALSE(set.pushToTalk.valid());
    REQUIRE_FALSE(set.toggleMute.valid());
    REQUIRE_FALSE(set.toggleDeafen.valid());
}

TEST_CASE("Custom override disables old default key and removal does not fall through", "[keybinds]")
{
    KeybindSet configured;
    configured.toggleMute = {KI::KI_X, Primary};
    configured.toggleDeafen = {};

    REQUIRE(matchKeybindShortcut(KI::KI_X, PRIMARY, configured) == ShortcutAction::ToggleMute);
    REQUIRE_FALSE(matchKeybindShortcut(KI::KI_M, PRIMARY | KI::KM_SHIFT, configured).has_value());
    REQUIRE_FALSE(matchKeybindShortcut(KI::KI_D, PRIMARY | KI::KM_SHIFT, configured).has_value());
    REQUIRE(matchKeybindShortcut(KI::KI_K, PRIMARY, configured) == ShortcutAction::QuickSwitcher);
}

TEST_CASE("Exact toggle matching rejects extra modifiers and ignores lock keys", "[keybinds]")
{
    const KeyBinding binding{KI::KI_M, Primary | Shift};
    REQUIRE(keyBindingPressedExactly(binding, KI::KI_M, PRIMARY | KI::KM_SHIFT));
    REQUIRE(keyBindingPressedExactly(binding, KI::KI_M, PRIMARY | KI::KM_SHIFT | KI::KM_CAPSLOCK | KI::KM_NUMLOCK | KI::KM_SCROLLLOCK));
    REQUIRE_FALSE(keyBindingPressedExactly(binding, KI::KI_M, PRIMARY | KI::KM_SHIFT | KI::KM_ALT));
    REQUIRE_FALSE(keyBindingPressedExactly(binding, KI::KI_M, PRIMARY));
    REQUIRE_FALSE(keyBindingPressedExactly(binding, KI::KI_N, PRIMARY | KI::KM_SHIFT));
}

TEST_CASE("Lone modifier key matches with its own raw modifier bit", "[keybinds]")
{
    const KeyBinding loneCtrl{KI::KI_LCONTROL, 0};
    REQUIRE(keyBindingPressedExactly(loneCtrl, KI::KI_LCONTROL, KI::KM_CTRL));
    REQUIRE(keyBindingPressedExactly(loneCtrl, KI::KI_LCONTROL, KI::KM_CTRL | KI::KM_CAPSLOCK));
    REQUIRE_FALSE(keyBindingPressedExactly(loneCtrl, KI::KI_LCONTROL, KI::KM_CTRL | KI::KM_SHIFT));

    const KeyBinding loneShift{KI::KI_LSHIFT, 0};
    REQUIRE(keyBindingPressedExactly(loneShift, KI::KI_LSHIFT, KI::KM_SHIFT));
    REQUIRE_FALSE(keyBindingPressedExactly(loneShift, KI::KI_LSHIFT, KI::KM_SHIFT | KI::KM_ALT));
}

TEST_CASE("PTT subset conflicts with toggle on same key", "[keybinds]")
{
    KeybindSet configuredWithToggle;
    configuredWithToggle.toggleMute = {KI::KI_M, Primary};

    const KeyBinding barePtt{KI::KI_M, 0};
    REQUIRE(conflictingKeybind(KeybindAction::PushToTalk, barePtt, configuredWithToggle) == KeybindAction::ToggleMute);

    KeybindSet configuredWithPtt;
    configuredWithPtt.pushToTalk = {KI::KI_M, 0};
    const KeyBinding ctrlToggle{KI::KI_M, Primary};
    REQUIRE(conflictingKeybind(KeybindAction::ToggleMute, ctrlToggle, configuredWithPtt) == KeybindAction::PushToTalk);

    const KeyBinding specificPtt{KI::KI_M, Primary | Shift};
    REQUIRE_FALSE(conflictingKeybind(KeybindAction::PushToTalk, specificPtt, configuredWithToggle).has_value());
}

TEST_CASE("Toggles with different exact modifiers on same key are allowed", "[keybinds]")
{
    KeybindSet configured;
    configured.toggleDeafen = {KI::KI_M, Primary | Shift};

    const KeyBinding muteBinding{KI::KI_M, Primary};
    REQUIRE_FALSE(conflictingKeybind(KeybindAction::ToggleMute, muteBinding, configured).has_value());
}

TEST_CASE("Same combo actions conflict and target self is ignored", "[keybinds]")
{
    KeybindSet configured;
    configured.toggleMute = {KI::KI_M, Primary};
    configured.toggleDeafen = {KI::KI_M, Primary};

    const KeyBinding muteBinding{KI::KI_M, Primary};
    REQUIRE(conflictingKeybind(KeybindAction::ToggleMute, muteBinding, configured) == KeybindAction::ToggleDeafen);

    KeybindSet muteOnly;
    muteOnly.toggleMute = {KI::KI_M, Primary};
    REQUIRE_FALSE(conflictingKeybind(KeybindAction::ToggleMute, muteBinding, muteOnly).has_value());

    REQUIRE_FALSE(conflictingKeybind(KeybindAction::ToggleMute, KeyBinding{}, configured).has_value());
}

TEST_CASE("Fixed shortcut collision for PTT and toggle matching rules", "[keybinds]")
{
    const KeyBinding bareK{KI::KI_K, 0};
    REQUIRE(conflictingFixedShortcut(KeybindAction::PushToTalk, bareK) == ShortcutAction::QuickSwitcher);

    const KeyBinding ctrlShiftK{KI::KI_K, Primary | Shift};
    REQUIRE_FALSE(conflictingFixedShortcut(KeybindAction::ToggleMute, ctrlShiftK).has_value());

    const KeyBinding ctrlK{KI::KI_K, Primary};
    REQUIRE(conflictingFixedShortcut(KeybindAction::ToggleMute, ctrlK) == ShortcutAction::QuickSwitcher);

    const KeyBinding defaultMuteKey{KI::KI_M, Primary | Shift};
    REQUIRE_FALSE(conflictingFixedShortcut(KeybindAction::ToggleMute, defaultMuteKey).has_value());
}

TEST_CASE("Effective shortcuts reflect custom bindings and removals", "[keybinds]")
{
    KeybindSet configured;
    configured.toggleMute = {KI::KI_X, Primary};
    configured.toggleDeafen = {};

    const auto list = effectiveShortcuts(configured);

    const auto &mute = findShortcutInList(list, ShortcutAction::ToggleMute);
    REQUIRE(mute.key == KI::KI_X);
    REQUIRE(mute.modifiers == Primary);

    REQUIRE_FALSE(listContainsAction(list, ShortcutAction::ToggleDeafen));

    const auto &switcher = findShortcutInList(list, ShortcutAction::QuickSwitcher);
    REQUIRE(switcher.key == KI::KI_K);
    REQUIRE(switcher.modifiers == Primary);
}
