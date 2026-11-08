#include <catch2/catch_test_macros.hpp>

#include "Core/SearchQuery.hpp"

#include <optional>
#include <string>
#include <vector>

using namespace Styx::Core;

struct SampleData
{
    std::vector<SearchName> people;
    std::vector<SearchName> channels;
};

static SampleData sampleData()
{
    return {
        {
            {Snowflake(1), {"Ann", "annie_l"}},
            {Snowflake(2), {"Bob"}},
            {Snowflake(3), {"Élodie"}},
        },
        {
            {Snowflake(10), {"general"}},
            {Snowflake(11), {"memes"}},
        },
    };
}

TEST_CASE("SearchSyntax fields copy through", "[search]")
{
    const auto [people, channels] = sampleData();
    SearchSyntax syntax;
    syntax.text = "hello world";
    syntax.has = {"image", "video"};
    syntax.pinned = true;
    syntax.before = Snowflake(100);
    syntax.after = Snowflake(50);

    const auto result = resolveSearch(syntax, people, channels);
    REQUIRE(result.query.content == "hello world");
    REQUIRE(result.query.has == std::vector<std::string>{"image", "video"});
    REQUIRE(result.query.pinned == std::optional<bool>{true});
    REQUIRE(result.query.before == Snowflake(100));
    REQUIRE(result.query.after == Snowflake(50));
    REQUIRE(result.query.authorIds.empty());
    REQUIRE(result.query.mentions.empty());
    REQUIRE(result.query.channelIds.empty());
    REQUIRE(result.unknownNames.empty());
}

TEST_CASE("Names and channels match case-insensitively", "[search]")
{
    const auto [people, channels] = sampleData();
    SearchSyntax syntax;
    syntax.from = {"ANNIE_L"};
    syntax.mentions = {"bob"};
    syntax.in = {"General"};

    const auto result = resolveSearch(syntax, people, channels);
    REQUIRE(result.query.authorIds == std::vector<Snowflake>{Snowflake(1)});
    REQUIRE(result.query.mentions == std::vector<Snowflake>{Snowflake(2)});
    REQUIRE(result.query.channelIds == std::vector<Snowflake>{Snowflake(10)});
    REQUIRE(result.unknownNames.empty());
}

TEST_CASE("Non-ASCII names match case-insensitively", "[search]")
{
    const auto [people, channels] = sampleData();
    SearchSyntax syntax;
    syntax.from = {"élodie"};

    const auto result = resolveSearch(syntax, people, channels);
    REQUIRE(result.query.authorIds == std::vector<Snowflake>{Snowflake(3)});
    REQUIRE(result.unknownNames.empty());
}

TEST_CASE("The same person twice gives one id", "[search]")
{
    const auto [people, channels] = sampleData();
    SearchSyntax syntax;
    syntax.from = {"Ann", "annie_l"};

    const auto result = resolveSearch(syntax, people, channels);
    REQUIRE(result.query.authorIds == std::vector<Snowflake>{Snowflake(1)});
    REQUIRE(result.unknownNames.empty());
}

TEST_CASE("Unmatched names end up in unknownNames in order", "[search]")
{
    const auto [people, channels] = sampleData();
    SearchSyntax syntax;
    syntax.from = {"zed"};
    syntax.in = {"nowhere"};

    const auto result = resolveSearch(syntax, people, channels);
    REQUIRE(result.query.authorIds.empty());
    REQUIRE(result.query.mentions.empty());
    REQUIRE(result.query.channelIds.empty());
    REQUIRE(result.unknownNames == std::vector<std::string>{"zed", "nowhere"});
}
