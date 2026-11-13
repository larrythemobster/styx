#include "Core/Theme/Appearance.hpp"

#include <catch2/catch_test_macros.hpp>

namespace Theme = Styx::Core::Theme;
using Theme::DeviceTheme;
using Theme::ThemeChoice;

TEST_CASE("Default themes map to Discord's settings-proto theme values", "[theme][appearance]")
{
    REQUIRE(Theme::choiceFromProto(0) == ThemeChoice::Ash);
    REQUIRE(Theme::choiceFromProto(1) == ThemeChoice::Ash);
    REQUIRE(Theme::choiceFromProto(2) == ThemeChoice::Light);
    REQUIRE(Theme::choiceFromProto(3) == ThemeChoice::Dark);
    REQUIRE(Theme::choiceFromProto(4) == ThemeChoice::Onyx);
    REQUIRE_FALSE(Theme::choiceFromProto(5));
    REQUIRE(Theme::protoFromChoice(ThemeChoice::Ash) == 1u);
    REQUIRE(Theme::protoFromChoice(ThemeChoice::Light) == 2u);
    REQUIRE(Theme::protoFromChoice(ThemeChoice::Dark) == 3u);
    REQUIRE(Theme::protoFromChoice(ThemeChoice::Onyx) == 4u);
    REQUIRE_FALSE(Theme::protoFromChoice(ThemeChoice::Custom));
}

TEST_CASE("Following the device decides light or dark without losing the chosen theme", "[theme][appearance]")
{
    Theme::Appearance appearance;
    appearance.choice = ThemeChoice::Onyx;
    appearance.followDevice = true;
    REQUIRE(Theme::effectiveChoice(appearance, DeviceTheme::Light) == ThemeChoice::Light);
    REQUIRE(Theme::effectiveChoice(appearance, DeviceTheme::Dark) == ThemeChoice::Onyx);
    REQUIRE(Theme::effectiveChoice(appearance, DeviceTheme::Unknown) == ThemeChoice::Onyx);
    appearance.choice = ThemeChoice::Light;
    REQUIRE(Theme::effectiveChoice(appearance, DeviceTheme::Dark) == ThemeChoice::Dark);
    appearance.followDevice = false;
    REQUIRE(Theme::effectiveChoice(appearance, DeviceTheme::Dark) == ThemeChoice::Light);
}

TEST_CASE("The custom theme follows the device mode and keeps its own accent", "[theme][appearance]")
{
    Theme::Appearance appearance;
    appearance.choice = ThemeChoice::Custom;
    appearance.seed = Styx::Platform::Color::rgb(0xf04d58);
    appearance.scheme = Theme::Scheme::Analogous;
    appearance.customDark = true;
    appearance.followDevice = true;
    REQUIRE(Theme::appearancePalette(appearance, DeviceTheme::Light)
            == Theme::generate(appearance.seed, appearance.scheme, false));
    appearance.followDevice = false;
    REQUIRE(Theme::appearancePalette(appearance, DeviceTheme::Light)
            == Theme::generate(appearance.seed, appearance.scheme, true));
}
