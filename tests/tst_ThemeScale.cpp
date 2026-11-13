#include "Core/Theme/Scale.hpp"

#include <catch2/catch_test_macros.hpp>

namespace Theme = Styx::Core::Theme;

TEST_CASE("Sliders snap to Discord's labeled stops", "[theme][scale]")
{
    REQUIRE(Theme::nearestStop(Theme::chatTextSizes, 13) == 12);
    REQUIRE(Theme::nearestStop(Theme::chatTextSizes, 17) == 16);
    REQUIRE(Theme::nearestStop(Theme::chatTextSizes, 22) == 20);
    REQUIRE(Theme::nearestStop(Theme::chatTextSizes, 23) == 24);
    REQUIRE(Theme::nearestStop(Theme::zoomLevels, 30) == 50);
    REQUIRE(Theme::nearestStop(Theme::zoomLevels, 71) == 67);
    REQUIRE(Theme::nearestStop(Theme::zoomLevels, 138) == 150);
    REQUIRE(Theme::nearestStop(Theme::zoomLevels, 400) == 200);
}

TEST_CASE("Zoom shortcuts step between zoom levels and stop at the ends", "[theme][scale]")
{
    REQUIRE(Theme::stepStop(Theme::zoomLevels, 100, 1) == 110);
    REQUIRE(Theme::stepStop(Theme::zoomLevels, 100, -1) == 90);
    REQUIRE(Theme::stepStop(Theme::zoomLevels, 67, -1) == 50);
    REQUIRE(Theme::stepStop(Theme::zoomLevels, 50, -1) == 50);
    REQUIRE(Theme::stepStop(Theme::zoomLevels, 200, 1) == 200);
    // A zoom between levels steps from the closest one.
    REQUIRE(Theme::stepStop(Theme::zoomLevels, 112, 1) == 125);
}
