#include <catch2/catch_test_macros.hpp>

#include "UI/Rml/NavigationHistory.hpp"

#include <optional>

using Styx::UI::NavigationHistory;

namespace {
struct Location {
    int account, guild, channel;
    bool operator==(const Location &) const = default;
};
constexpr Location friends{1, 0, 0};
constexpr Location dm{1, 0, 10};
constexpr Location forum{1, 20, 21};
constexpr Location otherAccount{2, 30, 31};

template<typename Predicate>
std::optional<Location> step(NavigationHistory<Location> &history, int direction, Predicate valid)
{
    auto index = history.find(direction, valid);
    if (!index) return std::nullopt;
    auto result = history.at(*index);
    history.select(*index);
    return result;
}
const auto all = [](const Location &) { return true; };
}

TEST_CASE("History revisits contexts across accounts without recording replay", "[navigation-history]")
{
    NavigationHistory<Location> history;
    REQUIRE(history.record(friends));
    REQUIRE_FALSE(history.record(friends));
    REQUIRE(history.record(dm));
    REQUIRE(history.record(forum));
    REQUIRE(history.record(otherAccount));
    REQUIRE_FALSE(history.find(1, all));
    REQUIRE(step(history, -1, all) == forum);
    REQUIRE(step(history, -1, all) == dm);
    REQUIRE(step(history, -1, all) == friends);
    REQUIRE_FALSE(history.find(-1, all));
    REQUIRE(step(history, 1, all) == dm);
    REQUIRE(step(history, 1, all) == forum);
    REQUIRE(step(history, 1, all) == otherAccount);
    REQUIRE_FALSE(history.find(1, all));
}

TEST_CASE("A new location after going back replaces the forward branch", "[navigation-history]")
{
    NavigationHistory<Location> history;
    history.record(friends);
    history.record(dm);
    history.record(forum);
    REQUIRE(step(history, -1, all) == dm);
    REQUIRE_FALSE(history.record(dm));
    REQUIRE(history.find(1, all));
    REQUIRE(history.record(otherAccount));
    REQUIRE_FALSE(history.find(1, all));
    REQUIRE(step(history, -1, all) == dm);
    REQUIRE(step(history, 1, all) == otherAccount);
}

TEST_CASE("History skips unavailable locations in both directions", "[navigation-history]")
{
    NavigationHistory<Location> history;
    history.record(friends);
    history.record(dm);
    history.record(forum);
    history.record(otherAccount);
    auto available = [](const Location &loc) { return loc != dm && loc != forum; };
    REQUIRE(step(history, -1, available) == friends);
    REQUIRE_FALSE(history.find(-1, available));
    REQUIRE(step(history, 1, available) == otherAccount);
    REQUIRE_FALSE(history.find(1, available));
    REQUIRE(step(history, -1, all) == forum);
}

TEST_CASE("History evicts its oldest locations at capacity", "[navigation-history]")
{
    NavigationHistory<Location> history(2);
    history.record(friends);
    history.record(dm);
    history.record(forum);
    REQUIRE(step(history, -1, all) == dm);
    REQUIRE_FALSE(history.find(-1, all));
}
