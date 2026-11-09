#include <catch2/catch_test_macros.hpp>

#include "UI/ChannelList/Navigation.hpp"

#include <optional>
#include <vector>

using namespace Styx;
using namespace Styx::Core;
using namespace Styx::UI;

static std::vector<NavigationEntry> sampleEntries()
{
    return {
        {Snowflake(1), false},
        {Snowflake(2), false},
        {Snowflake(3), true},
        {Snowflake(4), false},
        {Snowflake(5), true},
    };
}

static std::optional<Snowflake> step(Snowflake current, int direction, bool unreadOnly = false,
                                     const std::vector<NavigationEntry> &entries = sampleEntries())
{
    return stepNavigation(entries, current, direction, unreadOnly);
}

TEST_CASE("Step forward and back from adjacent entry", "[navigation]")
{
    REQUIRE(step(Snowflake(2), 1) == std::optional<Snowflake>(3));
    REQUIRE(step(Snowflake(2), -1) == std::optional<Snowflake>(1));
}

TEST_CASE("Navigation wraps around list ends", "[navigation]")
{
    REQUIRE(step(Snowflake(5), 1) == std::optional<Snowflake>(1));
    REQUIRE(step(Snowflake(1), -1) == std::optional<Snowflake>(5));
}

TEST_CASE("Unread-only navigation steps between unread entries", "[navigation]")
{
    REQUIRE(step(Snowflake(1), 1, true) == std::optional<Snowflake>(3));
    REQUIRE(step(Snowflake(3), 1, true) == std::optional<Snowflake>(5));
    REQUIRE(step(Snowflake(5), 1, true) == std::optional<Snowflake>(3));
    REQUIRE(step(Snowflake(4), -1, true) == std::optional<Snowflake>(3));
}

TEST_CASE("Current unread entry is skipped when stepping", "[navigation]")
{
    REQUIRE(step(Snowflake(3), 1, true) == std::optional<Snowflake>(5));
    REQUIRE(step(Snowflake(3), -1, true) == std::optional<Snowflake>(5));
}

TEST_CASE("Missing current entry starts from list ends", "[navigation]")
{
    REQUIRE(step(Snowflake(99), 1) == std::optional<Snowflake>(1));
    REQUIRE(step(Snowflake(99), -1) == std::optional<Snowflake>(5));
    REQUIRE(step(Snowflake(99), 1, true) == std::optional<Snowflake>(3));
    REQUIRE(step(Snowflake(99), -1, true) == std::optional<Snowflake>(5));
}

TEST_CASE("Navigation yields nullopt when no other entry qualifies", "[navigation]")
{
    REQUIRE(step(Snowflake(1), 1, false, {{Snowflake(1), false}}) == std::optional<Snowflake>());
    REQUIRE(step(Snowflake(1), 1, false, {}) == std::optional<Snowflake>());
    REQUIRE(step(Snowflake(1), 1, true, {{Snowflake(1), false}, {Snowflake(2), false}}) == std::optional<Snowflake>());
}

TEST_CASE("Invalid current id not in the list acts as missing", "[navigation]")
{
    REQUIRE(step(Snowflake::Invalid, 1) == std::optional<Snowflake>(1));
    REQUIRE(step(Snowflake::Invalid, -1) == std::optional<Snowflake>(5));
    REQUIRE(step(Snowflake::Invalid, 1, true) == std::optional<Snowflake>(3));
    REQUIRE(step(Snowflake::Invalid, -1, true) == std::optional<Snowflake>(5));
}

TEST_CASE("An invalid id in the list is found like any other", "[navigation]")
{
    // Home sits first in the server list under an invalid id.
    const std::vector<NavigationEntry> servers = {{Snowflake::Invalid, false}, {Snowflake(10), false}, {Snowflake(20), false}};
    REQUIRE(step(Snowflake::Invalid, 1, false, servers) == std::optional<Snowflake>(10));
    REQUIRE(step(Snowflake::Invalid, -1, false, servers) == std::optional<Snowflake>(20));
    REQUIRE(step(Snowflake(20), 1, false, servers) == std::optional<Snowflake>(Snowflake::Invalid));
}
