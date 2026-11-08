#include <catch2/catch_test_macros.hpp>

#include "Core/Audio/Sounds.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

using namespace Styx::Core::Sounds;

static constexpr std::array ALL_SOUNDS = {
    Sound::Message,
    Sound::Mention,
    Sound::Ring,
    Sound::Join,
    Sound::Leave,
};

static float peak(const std::vector<float> &samples)
{
    float maxVal = 0.0f;
    for (float s : samples)
        maxVal = std::max(maxVal, std::fabs(s));
    return maxVal;
}

TEST_CASE("Every sound starts and ends silently and stays within range", "[sounds]")
{
    for (Sound sound : ALL_SOUNDS) {
        INFO(static_cast<int>(sound));
        const auto samples = synthesize(sound);
        REQUIRE_FALSE(samples.empty());
        REQUIRE(std::fabs(samples.front()) < 0.01f);
        REQUIRE(std::fabs(samples.back()) < 0.01f);
        REQUIRE(peak(samples) > 0.1f);
        REQUIRE(peak(samples) <= 0.5f);
    }
}

TEST_CASE("Sound lengths", "[sounds]")
{
    // Message: 4320 + 7680 = 12000
    REQUIRE(synthesize(Sound::Message).size() == 12000);
    // Mention: 3360 + 3360 + 8640 = 15360
    REQUIRE(synthesize(Sound::Mention).size() == 15360);
    // Join: 3840 + 6720 = 10560
    REQUIRE(synthesize(Sound::Join).size() == 10560);
    // Leave: same as Join, 10560
    REQUIRE(synthesize(Sound::Leave).size() == 10560);
    // Ring: 2 * (8640 + 8640 + 3840) + 28800 = 71040
    REQUIRE(synthesize(Sound::Ring).size() == 71040);
}

TEST_CASE("Join and leave are different sounds", "[sounds]")
{
    REQUIRE(synthesize(Sound::Join) != synthesize(Sound::Leave));
    REQUIRE(synthesize(Sound::Message) != synthesize(Sound::Mention));
}

TEST_CASE("The ring ends in a pause so it can repeat", "[sounds]")
{
    const auto samples = synthesize(Sound::Ring);
    REQUIRE(std::all_of(samples.end() - 28800, samples.end(), [](float s) { return s == 0.0f; }));
}

TEST_CASE("Each sound has its own setting", "[sounds]")
{
    std::set<std::string> keys;
    for (Sound sound : ALL_SOUNDS)
        keys.insert(std::string(settingKey(sound)));

    REQUIRE(keys.size() == 5);
    for (const auto &key : keys)
        REQUIRE(key.rfind("sounds/", 0) == 0);
}
