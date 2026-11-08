#include <catch2/catch_test_macros.hpp>

#include "Core/SearchCompletion.hpp"

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
            {Snowflake(1), {"Ann Lee", "annie"}},
            {Snowflake(2), {"Bob"}},
        },
        {
            {Snowflake(10), {"general"}},
            {Snowflake(11), {"gaming"}},
        },
    };
}

TEST_CASE("Empty input lists filters in order", "[search]")
{
    const auto [people, channels] = sampleData();
    const auto withChannels = searchSuggestions("", people, channels);
    REQUIRE(withChannels.size() == 8);
    REQUIRE(withChannels[0].label == "from:");
    REQUIRE(withChannels[0].detail == "user");
    REQUIRE(withChannels[0].replacement == "from:");
    REQUIRE(withChannels[1].label == "mentions:");
    REQUIRE(withChannels[1].detail == "user");
    REQUIRE(withChannels[1].replacement == "mentions:");
    REQUIRE(withChannels[2].label == "in:");
    REQUIRE(withChannels[2].detail == "channel");
    REQUIRE(withChannels[2].replacement == "in:");
    REQUIRE(withChannels[3].label == "has:");
    REQUIRE(withChannels[3].detail == "link, embed, file, image…");
    REQUIRE(withChannels[3].replacement == "has:");
    REQUIRE(withChannels[4].label == "pinned:");
    REQUIRE(withChannels[4].detail == "true or false");
    REQUIRE(withChannels[4].replacement == "pinned:");
    REQUIRE(withChannels[5].label == "before:");
    REQUIRE(withChannels[5].detail == "date");
    REQUIRE(withChannels[5].replacement == "before:");
    REQUIRE(withChannels[6].label == "after:");
    REQUIRE(withChannels[6].detail == "date");
    REQUIRE(withChannels[6].replacement == "after:");
    REQUIRE(withChannels[7].label == "during:");
    REQUIRE(withChannels[7].detail == "date");
    REQUIRE(withChannels[7].replacement == "during:");

    const auto withoutChannels = searchSuggestions("", people, {});
    REQUIRE(withoutChannels.size() == 7);
    REQUIRE(withoutChannels[0].label == "from:");
    REQUIRE(withoutChannels[1].label == "mentions:");
    REQUIRE(withoutChannels[2].label == "has:");
    REQUIRE(withoutChannels[3].label == "pinned:");
    REQUIRE(withoutChannels[4].label == "before:");
    REQUIRE(withoutChannels[5].label == "after:");
    REQUIRE(withoutChannels[6].label == "during:");
}

TEST_CASE("Filter names match case-insensitively with prefix", "[search]")
{
    const auto [people, channels] = sampleData();
    const auto fResult = searchSuggestions("hello f", people, channels);
    REQUIRE(fResult.size() == 1);
    REQUIRE(fResult[0].label == "from:");
    REQUIRE(fResult[0].detail == "user");
    REQUIRE(fResult[0].replacement == "hello from:");

    const auto menResult = searchSuggestions("MEN", people, channels);
    REQUIRE(menResult.size() == 1);
    REQUIRE(menResult[0].label == "mentions:");
    REQUIRE(menResult[0].detail == "user");
    REQUIRE(menResult[0].replacement == "mentions:");
}

TEST_CASE("from suggestions match people names", "[search]")
{
    const auto [people, channels] = sampleData();
    const auto allPeople = searchSuggestions("from:", people, channels);
    REQUIRE(allPeople.size() == 2);
    REQUIRE(allPeople[0].label == "Ann Lee");
    REQUIRE(allPeople[0].detail == "annie");
    REQUIRE(allPeople[0].replacement == "from:\"Ann Lee\" ");
    REQUIRE(allPeople[1].label == "Bob");
    REQUIRE(allPeople[1].detail.empty());
    REQUIRE(allPeople[1].replacement == "from:Bob ");

    const auto annResult = searchSuggestions("from:ann", people, channels);
    REQUIRE(annResult.size() == 1);
    REQUIRE(annResult[0].label == "Ann Lee");
    REQUIRE(annResult[0].detail == "annie");
    REQUIRE(annResult[0].replacement == "from:\"Ann Lee\" ");

    const auto anniResult = searchSuggestions("from:anni", people, channels);
    REQUIRE(anniResult.size() == 1);
    REQUIRE(anniResult[0].label == "Ann Lee");
    REQUIRE(anniResult[0].detail == "annie");
    REQUIRE(anniResult[0].replacement == "from:annie ");
}

TEST_CASE("in suggestions match channel names", "[search]")
{
    const auto [people, channels] = sampleData();
    const auto result = searchSuggestions("cats in:ga", people, channels);
    REQUIRE(result.size() == 1);
    REQUIRE(result[0].label == "#gaming");
    REQUIRE(result[0].detail.empty());
    REQUIRE(result[0].replacement == "cats in:gaming ");
}

TEST_CASE("has and pinned filter values", "[search]")
{
    const auto [people, channels] = sampleData();
    const auto hasResult = searchSuggestions("has:im", people, channels);
    REQUIRE(hasResult.size() == 1);
    REQUIRE(hasResult[0].label == "image");
    REQUIRE(hasResult[0].detail.empty());
    REQUIRE(hasResult[0].replacement == "has:image ");

    const auto pinnedResult = searchSuggestions("pinned:", people, channels);
    REQUIRE(pinnedResult.size() == 2);
    REQUIRE(pinnedResult[0].label == "true");
    REQUIRE(pinnedResult[0].detail.empty());
    REQUIRE(pinnedResult[0].replacement == "pinned:true ");
    REQUIRE(pinnedResult[1].label == "false");
    REQUIRE(pinnedResult[1].detail.empty());
    REQUIRE(pinnedResult[1].replacement == "pinned:false ");
}

TEST_CASE("Date and unknown filters have no suggestions", "[search]")
{
    const auto [people, channels] = sampleData();
    REQUIRE(searchSuggestions("before:2024", people, channels).empty());
    REQUIRE(searchSuggestions("note:x", people, channels).empty());
}

TEST_CASE("Open quoted value completes across spaces", "[search]")
{
    const auto [people, channels] = sampleData();
    const auto result = searchSuggestions("from:\"Ann L", people, channels);
    REQUIRE(result.size() == 1);
    REQUIRE(result[0].label == "Ann Lee");
    REQUIRE(result[0].detail == "annie");
    REQUIRE(result[0].replacement == "from:\"Ann Lee\" ");
}

TEST_CASE("Limit restricts suggestion count", "[search]")
{
    const auto [people, channels] = sampleData();
    const auto result = searchSuggestions("", people, channels, 1);
    REQUIRE(result.size() == 1);
    REQUIRE(result[0].label == "from:");
    REQUIRE(result[0].replacement == "from:");
}
