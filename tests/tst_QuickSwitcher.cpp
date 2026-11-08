#include <catch2/catch_test_macros.hpp>

#include "Core/QuickSwitcher.hpp"

#include <string_view>
#include <vector>

using namespace Styx::Core;

static std::vector<SwitcherItem> sampleItems()
{
    return {
        {SwitcherItem::Kind::Server, Snowflake(1), Snowflake(), "Gaming Hub", 0.0},
        {SwitcherItem::Kind::TextChannel, Snowflake(2), Snowflake(1), "general", 5.0},
        {SwitcherItem::Kind::TextChannel, Snowflake(3), Snowflake(1), "gen-art", 0.0},
        {SwitcherItem::Kind::VoiceChannel, Snowflake(4), Snowflake(1), "General Voice", 1.0},
        {SwitcherItem::Kind::Dm, Snowflake(5), Snowflake(), "Genevieve", 9.0},
        {SwitcherItem::Kind::Friend, Snowflake(6), Snowflake(), "bob", 0.0},
        {SwitcherItem::Kind::TextChannel, Snowflake(7), Snowflake(1), "off-topic", 0.0},
    };
}

static std::vector<std::size_t> queryIndices(std::string_view query, std::size_t limit = 20)
{
    static const auto items = sampleItems();
    return rankSwitcher(query, items, limit);
}

TEST_CASE("Exact match ranks before prefix match", "[switcher]")
{
    REQUIRE(queryIndices("general") == std::vector<std::size_t>{1, 3});
}

TEST_CASE("Prefix matches tie-break by frecency", "[switcher]")
{
    REQUIRE(queryIndices("gen") == std::vector<std::size_t>{4, 1, 3, 2});
}

TEST_CASE("Word start matches rank by tier", "[switcher]")
{
    REQUIRE(queryIndices("topic") == std::vector<std::size_t>{6});
    REQUIRE(queryIndices("hub") == std::vector<std::size_t>{0});
}

TEST_CASE("Subsequence matches characters in order", "[switcher]")
{
    REQUIRE(queryIndices("gnrl") == std::vector<std::size_t>{1, 3});
}

TEST_CASE("Query matching is case-insensitive", "[switcher]")
{
    REQUIRE(queryIndices("GENERAL") == queryIndices("general"));
}

TEST_CASE("Prefix filters by item kind", "[switcher]")
{
    REQUIRE(queryIndices("#gen") == std::vector<std::size_t>{1, 2});
    REQUIRE(queryIndices("!gen") == std::vector<std::size_t>{3});
    REQUIRE(queryIndices("@") == std::vector<std::size_t>{4});
    REQUIRE(queryIndices("* gaming") == std::vector<std::size_t>{0});
}

TEST_CASE("Empty query orders by frecency", "[switcher]")
{
    REQUIRE(queryIndices("") == std::vector<std::size_t>{4, 1, 3});
}

TEST_CASE("Limit caps result count", "[switcher]")
{
    REQUIRE(queryIndices("gen", 1) == std::vector<std::size_t>{4});
}

TEST_CASE("No match produces empty result", "[switcher]")
{
    REQUIRE(queryIndices("zzz").empty());
}
