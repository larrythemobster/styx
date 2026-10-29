#include "Core/Emoji/UnicodeEmojiIndex.hpp"

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <string>
#include <vector>

using namespace Styx::Core;

static const std::string thumbsUpTone2 = "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBC";

TEST_CASE("Unicode emoji names and aliases", "[emoji]")
{
    const auto &index = UnicodeEmojiIndex::instance();
    const auto *emoji = index.byName("thumbsup::skin-tone-2");
    REQUIRE(emoji);
    CHECK(emoji->surrogates == thumbsUpTone2);
    CHECK(emoji->isDiversityChild);
    const auto *parent = index.byName("thumbsup");
    REQUIRE(parent);
    CHECK(index.byName("+1") == parent);
    CHECK(parent->hasDiversity);
    CHECK(UnicodeEmojiIndex::primaryName(*parent) == "thumbsup");
    CHECK(index.byName("definitely_not_an_emoji") == nullptr);
    REQUIRE(index.byName("laughing"));
    CHECK(index.byName("satisfied") == index.byName("laughing"));
    const auto *grinning = index.byName("grinning");
    REQUIRE(grinning);
    CHECK(std::find(grinning->keywords.begin(), grinning->keywords.end(), "happy") != grinning->keywords.end());
    CHECK(index.byName("happy") == nullptr);
    CHECK(index.convertSurrogateToBase(thumbsUpTone2) == parent);
    CHECK(index.bySurrogate(thumbsUpTone2) == emoji);
}

TEST_CASE("Unicode emoji name translation preserves custom tags", "[emoji]")
{
    const auto &index = UnicodeEmojiIndex::instance();
    CHECK(index.translateNamesToSurrogates(":thumbsup: hi <:pepe:123> :nope:") == "\xF0\x9F\x91\x8D hi <:pepe:123> :nope:");
    CHECK(index.translateNamesToSurrogates(":thumbsup::skin-tone-2:") == thumbsUpTone2);
    CHECK(index.translateNamesToSurrogates("<a:pepe:123>") == "<a:pepe:123>");
    CHECK(index.translateNamesToSurrogates("no emoji here") == "no emoji here");
    CHECK(index.translateNamesToSurrogates("<:100:123456789>") == "<:100:123456789>");
    CHECK(index.translateNamesToSurrogates(":100: <:100:123456789>") == "\xF0\x9F\x92\xAF <:100:123456789>");
}

TEST_CASE("Unicode emoji categories contain top-level emoji", "[emoji]")
{
    const auto &index = UnicodeEmojiIndex::instance();
    REQUIRE(!index.topLevel().empty());
    for (const auto *emoji : index.topLevel())
        CHECK(!emoji->isDiversityChild);
    std::vector<std::string> ids;
    std::size_t categorized = 0;
    for (const auto &category : index.categories()) {
        ids.push_back(category.id);
        categorized += category.emojis.size();
        for (const auto *emoji : category.emojis)
            CHECK(emoji->category == category.id);
    }
    CHECK((ids == std::vector<std::string>{"people", "nature", "food", "activity", "travel", "objects", "symbols", "flags"}));
    CHECK(categorized == index.topLevel().size());
    REQUIRE(!index.categories().empty());
    REQUIRE(!index.categories().front().emojis.empty());
    CHECK(index.categories().front().emojis.front() == index.byName("grinning"));
    REQUIRE(index.byName("thumbsup::skin-tone-2"));
    CHECK(index.byName("thumbsup::skin-tone-2")->category == "people");
}
