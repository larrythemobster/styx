#include <catch2/catch_test_macros.hpp>

#include "Core/SearchSyntax.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace Styx::Core;

static SearchSyntax parse(const char *input)
{
    return parseSearch(input, 0);
}

constexpr std::int64_t JAN_1_2024 = 1704067200000;
constexpr std::int64_t DAY = 86400000;

TEST_CASE("Plain words stay as text", "[search]")
{
    const auto result = parse("  hello   world ");
    REQUIRE(result.text == "hello world");
    REQUIRE(result.from.empty());
    REQUIRE(result.mentions.empty());
    REQUIRE(result.in.empty());
    REQUIRE(result.has.empty());
    REQUIRE_FALSE(result.pinned.has_value());
    REQUIRE_FALSE(result.before.isValid());
    REQUIRE_FALSE(result.after.isValid());
}

TEST_CASE("People and channels", "[search]")
{
    const auto result = parse("from:@ann mentions:bob in:#general cats");
    REQUIRE(result.from == std::vector<std::string>{"ann"});
    REQUIRE(result.mentions == std::vector<std::string>{"bob"});
    REQUIRE(result.in == std::vector<std::string>{"general"});
    REQUIRE(result.text == "cats");

    const auto repeated = parse("from:ann from:bob");
    REQUIRE(repeated.from == std::vector<std::string>{"ann", "bob"});
}

TEST_CASE("Quoted values keep their spaces", "[search]")
{
    const auto result = parse("from:\"Ann Lee\" hi");
    REQUIRE(result.from == std::vector<std::string>{"Ann Lee"});
    REQUIRE(result.text == "hi");

    const auto unclosed = parse("in:\"general chat");
    REQUIRE(unclosed.in == std::vector<std::string>{"general chat"});
}

TEST_CASE("Keys are case-insensitive", "[search]")
{
    const auto result = parse("FROM:ann Has:IMAGE");
    REQUIRE(result.from == std::vector<std::string>{"ann"});
    REQUIRE(result.has == std::vector<std::string>{"image"});
}

TEST_CASE("has values are checked", "[search]")
{
    const auto result = parse("has:link has:forward has:banana");
    REQUIRE(result.has == std::vector<std::string>{"link", "snapshot"});
    REQUIRE(result.text == "has:banana");
}

TEST_CASE("pinned takes true or false", "[search]")
{
    REQUIRE(parse("pinned:true").pinned == true);
    REQUIRE(parse("pinned:FALSE").pinned == false);

    const auto maybe = parse("pinned:maybe");
    REQUIRE_FALSE(maybe.pinned.has_value());
    REQUIRE(maybe.text == "pinned:maybe");
}

TEST_CASE("Dates become snowflake bounds", "[search]")
{
    REQUIRE(parse("before:2024-01-01").before == Snowflake::fromUnixMs(JAN_1_2024));
    REQUIRE(parse("after:2024-01-01").after == Snowflake::fromUnixMs(JAN_1_2024 + DAY));

    const auto during = parse("during:2024-01-01");
    REQUIRE(during.after == Snowflake::fromUnixMs(JAN_1_2024));
    REQUIRE(during.before == Snowflake::fromUnixMs(JAN_1_2024 + DAY));
}

TEST_CASE("Dates are local days", "[search]")
{
    REQUIRE(parseSearch("before:2024-01-01", 60).before == Snowflake::fromUnixMs(JAN_1_2024 - 60 * 60 * 1000));
    REQUIRE(parseSearch("before:2024-01-01", -300).before == Snowflake::fromUnixMs(JAN_1_2024 + 5 * 60 * 60 * 1000));
}

TEST_CASE("Leap days and bad dates", "[search]")
{
    REQUIRE(parse("before:2024-02-29").before == Snowflake::fromUnixMs(JAN_1_2024 + 59 * DAY));

    const auto badMonth = parse("before:2024-13-01");
    REQUIRE(badMonth.text == "before:2024-13-01");
    REQUIRE_FALSE(badMonth.before.isValid());

    const auto notADate = parse("after:yesterday");
    REQUIRE(notADate.text == "after:yesterday");
}

TEST_CASE("Unknown or empty filters stay as text", "[search]")
{
    const auto emptyFilter = parse("https://example.com from:");
    REQUIRE(emptyFilter.text == "https://example.com from:");
    REQUIRE(emptyFilter.from.empty());

    REQUIRE(parse("note:x").text == "note:x");
}
