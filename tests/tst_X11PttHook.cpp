#include <catch2/catch_test_macros.hpp>

#include "UI/X11PttHook.hpp"
#include "UI/Rml/Shortcuts.hpp"

#include <RmlUi/Core/Input.h>

#if defined(__linux__) && defined(STYX_HAVE_X11_PTT)
#include <X11/keysym.h>
#endif

using namespace Styx::UI;
namespace KI = ::Rml::Input;

#if defined(__linux__) && defined(STYX_HAVE_X11_PTT)

TEST_CASE("X11 keysym mappings match expected range endpoints and lowercase letters", "[ptt]")
{
    REQUIRE(X11PttHook::keySym(KI::KI_A) == XK_a);
    REQUIRE(X11PttHook::keySym(KI::KI_B) == XK_b);
    REQUIRE(X11PttHook::keySym(KI::KI_Y) == XK_y);
    REQUIRE(X11PttHook::keySym(KI::KI_Z) == XK_z);

    REQUIRE(X11PttHook::keySym(KI::KI_0) == XK_0);
    REQUIRE(X11PttHook::keySym(KI::KI_9) == XK_9);

    REQUIRE(X11PttHook::keySym(KI::KI_F1) == XK_F1);
    REQUIRE(X11PttHook::keySym(KI::KI_F12) == XK_F12);
    REQUIRE(X11PttHook::keySym(KI::KI_F13) == XK_F13);
    REQUIRE(X11PttHook::keySym(KI::KI_F24) == XK_F24);

    REQUIRE(X11PttHook::keySym(KI::KI_NUMPAD0) == XK_KP_0);
    REQUIRE(X11PttHook::keySym(KI::KI_NUMPAD9) == XK_KP_9);
}

TEST_CASE("X11 keysym mappings distinguish main and keypad Enter", "[ptt]")
{
    REQUIRE(X11PttHook::keySym(KI::KI_RETURN) == XK_Return);
    REQUIRE(X11PttHook::keySym(KI::KI_NUMPADENTER) == XK_KP_Enter);
}

TEST_CASE("X11 keysym mappings distinguish left and right modifiers", "[ptt]")
{
    REQUIRE(X11PttHook::keySym(KI::KI_LCONTROL) == XK_Control_L);
    REQUIRE(X11PttHook::keySym(KI::KI_RCONTROL) == XK_Control_R);

    REQUIRE(X11PttHook::keySym(KI::KI_LSHIFT) == XK_Shift_L);
    REQUIRE(X11PttHook::keySym(KI::KI_RSHIFT) == XK_Shift_R);

    REQUIRE(X11PttHook::keySym(KI::KI_LMENU) == XK_Alt_L);
    REQUIRE(X11PttHook::keySym(KI::KI_RMENU) == XK_Alt_R);

    REQUIRE(X11PttHook::keySym(KI::KI_LMETA) == XK_Super_L);
    REQUIRE(X11PttHook::keySym(KI::KI_RMETA) == XK_Super_R);
}

TEST_CASE("X11 keysym mappings map backquote, punctuation, navigation, arithmetic and unsupported keys", "[ptt]")
{
    REQUIRE(X11PttHook::keySym(KI::KI_OEM_3) == XK_grave);

    REQUIRE(X11PttHook::keySym(KI::KI_OEM_1) == XK_semicolon);
    REQUIRE(X11PttHook::keySym(KI::KI_OEM_PLUS) == XK_equal);
    REQUIRE(X11PttHook::keySym(KI::KI_OEM_COMMA) == XK_comma);
    REQUIRE(X11PttHook::keySym(KI::KI_OEM_MINUS) == XK_minus);
    REQUIRE(X11PttHook::keySym(KI::KI_OEM_PERIOD) == XK_period);
    REQUIRE(X11PttHook::keySym(KI::KI_OEM_2) == XK_slash);
    REQUIRE(X11PttHook::keySym(KI::KI_OEM_4) == XK_bracketleft);
    REQUIRE(X11PttHook::keySym(KI::KI_OEM_5) == XK_backslash);
    REQUIRE(X11PttHook::keySym(KI::KI_OEM_6) == XK_bracketright);
    REQUIRE(X11PttHook::keySym(KI::KI_OEM_7) == XK_apostrophe);
    REQUIRE(X11PttHook::keySym(KI::KI_OEM_102) == XK_less);

    REQUIRE(X11PttHook::keySym(KI::KI_MULTIPLY) == XK_KP_Multiply);
    REQUIRE(X11PttHook::keySym(KI::KI_ADD) == XK_KP_Add);
    REQUIRE(X11PttHook::keySym(KI::KI_SEPARATOR) == XK_KP_Separator);
    REQUIRE(X11PttHook::keySym(KI::KI_SUBTRACT) == XK_KP_Subtract);
    REQUIRE(X11PttHook::keySym(KI::KI_DECIMAL) == XK_KP_Decimal);
    REQUIRE(X11PttHook::keySym(KI::KI_DIVIDE) == XK_KP_Divide);

    REQUIRE(X11PttHook::keySym(KI::KI_BACK) == XK_BackSpace);
    REQUIRE(X11PttHook::keySym(KI::KI_TAB) == XK_Tab);
    REQUIRE(X11PttHook::keySym(KI::KI_SPACE) == XK_space);
    REQUIRE(X11PttHook::keySym(KI::KI_ESCAPE) == XK_Escape);
    REQUIRE(X11PttHook::keySym(KI::KI_DELETE) == XK_Delete);
    REQUIRE(X11PttHook::keySym(KI::KI_INSERT) == XK_Insert);
    REQUIRE(X11PttHook::keySym(KI::KI_HOME) == XK_Home);
    REQUIRE(X11PttHook::keySym(KI::KI_END) == XK_End);
    REQUIRE(X11PttHook::keySym(KI::KI_PRIOR) == XK_Page_Up);
    REQUIRE(X11PttHook::keySym(KI::KI_NEXT) == XK_Page_Down);
    REQUIRE(X11PttHook::keySym(KI::KI_UP) == XK_Up);
    REQUIRE(X11PttHook::keySym(KI::KI_DOWN) == XK_Down);
    REQUIRE(X11PttHook::keySym(KI::KI_LEFT) == XK_Left);
    REQUIRE(X11PttHook::keySym(KI::KI_RIGHT) == XK_Right);

    REQUIRE(X11PttHook::keySym(KI::KI_UNKNOWN) == 0);
    REQUIRE(X11PttHook::keySym(9999) == 0);
}

TEST_CASE("X11 PTT hook rejects Wayland driver and invalid bindings offline", "[ptt]")
{
    X11PttHook hook;
    REQUIRE_FALSE(hook.active());

    Styx::UI::Rml::KeyBinding binding{KI::KI_A, 0};
    REQUIRE_FALSE(hook.start(binding, [](bool) {}, "wayland"));
    REQUIRE_FALSE(hook.active());

    REQUIRE_FALSE(hook.start(binding, [](bool) {}, "cocoa"));
    REQUIRE_FALSE(hook.active());

    Styx::UI::Rml::KeyBinding invalidBinding{0, 0};
    REQUIRE_FALSE(hook.start(invalidBinding, [](bool) {}, "x11"));
    REQUIRE_FALSE(hook.active());

    Styx::UI::Rml::KeyBinding unsupportedBinding{9999, 0};
    REQUIRE_FALSE(hook.start(unsupportedBinding, [](bool) {}, "x11"));
    REQUIRE_FALSE(hook.active());
}

#endif
