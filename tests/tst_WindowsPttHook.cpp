#include <catch2/catch_test_macros.hpp>

#include "UI/PttKeyState.hpp"
#include "UI/WindowsPttHook.hpp"
#include "UI/Rml/Shortcuts.hpp"

#include <RmlUi/Core/Input.h>

#include <optional>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace Styx::UI;
using namespace Styx::UI::Rml;
namespace KI = ::Rml::Input;

TEST_CASE("Key state ignores press with missing modifiers", "[ptt]")
{
    PttKeyState state('T', ShortcutModifier::Primary);

    REQUIRE_FALSE(state.update('T', 0, true).has_value());
    REQUIRE_FALSE(state.update('T', ShortcutModifier::Shift, true).has_value());
    REQUIRE_FALSE(state.update('T', ShortcutModifier::Alt, true).has_value());
    REQUIRE_FALSE(state.update('T', ShortcutModifier::Shift | ShortcutModifier::Alt, true).has_value());
}

TEST_CASE("Key state matches press with required and extra modifiers", "[ptt]")
{
    PttKeyState state('T', ShortcutModifier::Primary);
    REQUIRE(state.update('T', ShortcutModifier::Primary, true) == std::optional<bool>(true));

    PttKeyState extraState('T', ShortcutModifier::Primary);
    REQUIRE(extraState.update('T', ShortcutModifier::Primary | ShortcutModifier::Shift, true) == std::optional<bool>(true));

    PttKeyState allState('T', ShortcutModifier::Primary);
    REQUIRE(allState.update('T', ShortcutModifier::Primary | ShortcutModifier::Shift | ShortcutModifier::Alt, true) ==
            std::optional<bool>(true));
}

TEST_CASE("Key state ignores repeated down and repeated up events", "[ptt]")
{
    PttKeyState state('T', ShortcutModifier::Primary);

    REQUIRE(state.update('T', ShortcutModifier::Primary, true) == std::optional<bool>(true));
    REQUIRE_FALSE(state.update('T', ShortcutModifier::Primary, true).has_value());
    REQUIRE_FALSE(state.update('T', ShortcutModifier::Primary, true).has_value());

    REQUIRE(state.update('T', ShortcutModifier::Primary, false) == std::optional<bool>(false));
    REQUIRE_FALSE(state.update('T', ShortcutModifier::Primary, false).has_value());
    REQUIRE_FALSE(state.update('T', ShortcutModifier::Primary, false).has_value());
}

TEST_CASE("Key state ignores unrelated keys while held", "[ptt]")
{
    PttKeyState state('T', ShortcutModifier::Primary);

    REQUIRE(state.update('T', ShortcutModifier::Primary, true) == std::optional<bool>(true));

    REQUIRE_FALSE(state.update('A', 0, true).has_value());
    REQUIRE_FALSE(state.update('A', 0, false).has_value());
    REQUIRE_FALSE(state.update('A', ShortcutModifier::Primary, true).has_value());
    REQUIRE_FALSE(state.update('A', ShortcutModifier::Primary, false).has_value());

    REQUIRE(state.update('T', 0, false) == std::optional<bool>(false));
}

TEST_CASE("Key state transitions to released even when modifiers released first", "[ptt]")
{
    PttKeyState state('T', ShortcutModifier::Primary);

    REQUIRE(state.update('T', ShortcutModifier::Primary, true) == std::optional<bool>(true));
    REQUIRE(state.update('T', 0, false) == std::optional<bool>(false));
}

TEST_CASE("Key state works for a lone modifier target", "[ptt]")
{
    constexpr int targetVk = 0xA2; // VK_LCONTROL
    PttKeyState ctrlState(targetVk, 0);

    REQUIRE(ctrlState.update(targetVk, 0, true) == std::optional<bool>(true));
    REQUIRE_FALSE(ctrlState.update(targetVk, 0, true).has_value());
    REQUIRE(ctrlState.update(targetVk, 0, false) == std::optional<bool>(false));
    REQUIRE_FALSE(ctrlState.update(targetVk, 0, false).has_value());

    constexpr int targetAlt = 0xA4; // VK_LMENU
    PttKeyState altState(targetAlt, 0);

    REQUIRE(altState.update(targetAlt, ShortcutModifier::Alt, true) == std::optional<bool>(true));
    REQUIRE_FALSE(altState.update(targetAlt, ShortcutModifier::Alt, true).has_value());
    REQUIRE(altState.update(targetAlt, 0, false) == std::optional<bool>(false));
    REQUIRE_FALSE(altState.update(targetAlt, 0, false).has_value());
}

TEST_CASE("Key state with zero target key ignores all events", "[ptt]")
{
    PttKeyState state(0, 0);

    REQUIRE_FALSE(state.update(0, 0, true).has_value());
    REQUIRE_FALSE(state.update('A', 0, true).has_value());
    REQUIRE_FALSE(state.update('A', 0, false).has_value());
}

#ifdef _WIN32

TEST_CASE("Virtual key mappings match expected Windows VK constants", "[ptt]")
{
    REQUIRE(WindowsPttHook::virtualKey(KI::KI_A) == 'A');
    REQUIRE(WindowsPttHook::virtualKey(KI::KI_Z) == 'Z');
    REQUIRE(WindowsPttHook::virtualKey(KI::KI_0) == '0');
    REQUIRE(WindowsPttHook::virtualKey(KI::KI_9) == '9');

    REQUIRE(WindowsPttHook::virtualKey(KI::KI_F1) == VK_F1);
    REQUIRE(WindowsPttHook::virtualKey(KI::KI_F12) == VK_F12);
    REQUIRE(WindowsPttHook::virtualKey(KI::KI_F13) == VK_F13);
    REQUIRE(WindowsPttHook::virtualKey(KI::KI_F24) == VK_F24);

    REQUIRE(WindowsPttHook::virtualKey(KI::KI_OEM_3) == VK_OEM_3);

    REQUIRE(WindowsPttHook::virtualKey(KI::KI_LCONTROL) == VK_LCONTROL);
    REQUIRE(WindowsPttHook::virtualKey(KI::KI_RCONTROL) == VK_RCONTROL);
    REQUIRE(WindowsPttHook::virtualKey(KI::KI_LSHIFT) == VK_LSHIFT);
    REQUIRE(WindowsPttHook::virtualKey(KI::KI_RSHIFT) == VK_RSHIFT);
    REQUIRE(WindowsPttHook::virtualKey(KI::KI_LMENU) == VK_LMENU);
    REQUIRE(WindowsPttHook::virtualKey(KI::KI_RMENU) == VK_RMENU);

    REQUIRE(WindowsPttHook::virtualKey(KI::KI_RETURN) == VK_RETURN);
    REQUIRE(WindowsPttHook::virtualKey(KI::KI_NUMPADENTER) == VK_RETURN);

    REQUIRE(WindowsPttHook::virtualKey(KI::KI_UNKNOWN) == 0);
    REQUIRE(WindowsPttHook::virtualKey(9999) == 0);
}

TEST_CASE("Virtual key normalization handles extended keys and scan codes", "[ptt]")
{
    REQUIRE(WindowsPttHook::normalizeVirtualKey(VK_CONTROL, 0, false) == VK_LCONTROL);
    REQUIRE(WindowsPttHook::normalizeVirtualKey(VK_CONTROL, 0, true) == VK_RCONTROL);

    REQUIRE(WindowsPttHook::normalizeVirtualKey(VK_MENU, 0, false) == VK_LMENU);
    REQUIRE(WindowsPttHook::normalizeVirtualKey(VK_MENU, 0, true) == VK_RMENU);

    UINT leftShiftScan = MapVirtualKeyW(VK_LSHIFT, MAPVK_VK_TO_VSC);
    UINT rightShiftScan = MapVirtualKeyW(VK_RSHIFT, MAPVK_VK_TO_VSC);
    REQUIRE(WindowsPttHook::normalizeVirtualKey(VK_SHIFT, static_cast<int>(leftShiftScan), false) == VK_LSHIFT);
    REQUIRE(WindowsPttHook::normalizeVirtualKey(VK_SHIFT, static_cast<int>(rightShiftScan), false) == VK_RSHIFT);

    REQUIRE(WindowsPttHook::normalizeVirtualKey('A', 0, false) == 'A');
    REQUIRE(WindowsPttHook::normalizeVirtualKey(VK_F1, 0, false) == VK_F1);
    REQUIRE(WindowsPttHook::normalizeVirtualKey(VK_LCONTROL, 0, false) == VK_LCONTROL);
    REQUIRE(WindowsPttHook::normalizeVirtualKey(VK_RCONTROL, 0, true) == VK_RCONTROL);
}

#else

TEST_CASE("Non-Windows virtual key returns zero and normalization is identity", "[ptt]")
{
    REQUIRE(WindowsPttHook::virtualKey(KI::KI_A) == 0);
    REQUIRE(WindowsPttHook::virtualKey(KI::KI_F13) == 0);
    REQUIRE(WindowsPttHook::normalizeVirtualKey(42, 0, false) == 42);

    WindowsPttHook hook;
    REQUIRE_FALSE(hook.start(KeyBinding{1, 0}, [](bool) {}));
    REQUIRE(hook.error() == "Global keyboard hook only available on Windows");
}

#endif
