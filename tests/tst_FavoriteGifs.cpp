#include "Core/Gifs/FavoriteGifRules.hpp"
#include "UI/Gifs/GifMasonryLayout.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace Styx;
using namespace Styx::Core;
using Styx::Proto::FavoriteGif;
using Styx::Proto::GifType;

TEST_CASE("Favorite GIFs: testKeyDropsTheSignatureOfDiscordAttachmentsOnly")
{
    REQUIRE(FavoriteGifRules::keyFor("https://cdn.discordapp.com/attachments/1/2/cat.gif?ex=aa&is=bb&hm=cc&") == std::string("https://cdn.discordapp.com/attachments/1/2/cat.gif"));
    REQUIRE(FavoriteGifRules::keyFor("https://media.discordapp.net/attachments/1/2/cat.gif?ex=aa&width=10&is=bb&hm=cc") == std::string("https://media.discordapp.net/attachments/1/2/cat.gif?width=10"));
    REQUIRE(FavoriteGifRules::keyFor("https://tenor.com/view/cat-123?ex=aa") == std::string("https://tenor.com/view/cat-123?ex=aa"));
    REQUIRE(FavoriteGifRules::keyFor("https://images-ext-1.discordapp.net/external/sig/https/x.test/a.gif?ex=aa") == std::string("https://images-ext-1.discordapp.net/external/sig/https/x.test/a.gif?ex=aa"));
    REQUIRE(FavoriteGifRules::keyFor("not a url") == std::string("not a url"));
}

TEST_CASE("Favorite GIFs: testGifvEmbedIsStoredAsItsProxiedVideo")
{
    const FavoriteGifCandidate candidate{ "https://tenor.com/view/cat-123", "https://images-ext-1.discordapp.net/external/sig/https/media.tenor.com/abc/cat.mp4", {}, GifType::Video, Platform::Size{498, 280} };
    const FavoriteGif gif = FavoriteGifRules::stored(candidate, 7);

    REQUIRE(gif.url == std::string("https://tenor.com/view/cat-123"));
    REQUIRE(gif.src == candidate.src);
    REQUIRE(gif.format == GifType::Video);
    REQUIRE(gif.width == 498u);
    REQUIRE(gif.height == 280u);
    REQUIRE(gif.order == 7u);
    REQUIRE(gif.messageAsReceived.empty());
}

TEST_CASE("Favorite GIFs: testGifAttachmentIsStoredAsAnimatedWebp")
{
    const FavoriteGifCandidate candidate{ "https://cdn.discordapp.com/attachments/1/2/cat.gif?ex=aa&is=bb&hm=cc&", "https://media.discordapp.net/attachments/1/2/cat.gif?ex=aa&is=bb&hm=cc&", {}, GifType::Image, Platform::Size{320, 240} };
    const FavoriteGif gif = FavoriteGifRules::stored(candidate, 1);

    REQUIRE(gif.url == std::string("https://cdn.discordapp.com/attachments/1/2/cat.gif"));
    REQUIRE(gif.src == std::string("https://media.discordapp.net/attachments/1/2/cat.gif?ex=aa&is=bb&hm=cc&format=webp&animated=true"));
    REQUIRE(gif.format == GifType::Image);
}

TEST_CASE("Favorite GIFs: testWebpAttachmentOnlyGainsTheAnimatedFlag")
{
    const FavoriteGifCandidate candidate{ "https://cdn.discordapp.com/attachments/1/2/cat.webp", "https://media.discordapp.net/attachments/1/2/cat.webp?ex=aa", {}, GifType::Image, Platform::Size{320, 240} };
    REQUIRE(FavoriteGifRules::stored(candidate, 1).src == std::string("https://media.discordapp.net/attachments/1/2/cat.webp?ex=aa&animated=true"));
}

TEST_CASE("Favorite GIFs: testKlipyEmbedIsStoredAsItsThumbnail")
{
    const FavoriteGifCandidate candidate{ "https://klipy.com/gifs/cat", "https://images-ext-1.discordapp.net/external/sig/https/static.klipy.com/cat.mp4",
                                          "https://images-ext-1.discordapp.net/external/sig2/https/static.klipy.com/cat.webp", GifType::Video, Platform::Size{400, 300} };
    const FavoriteGif gif = FavoriteGifRules::stored(candidate, 1);

    REQUIRE(gif.src == std::string("https://images-ext-1.discordapp.net/external/sig2/https/static.klipy.com/cat.webp?animated=true"));
    REQUIRE(gif.format == GifType::Image);
}

TEST_CASE("Favorite GIFs: testProviderResultPrefersItsGifSource")
{
    const FavoriteGifCandidate candidate{ "https://tenor.com/view/cat-123", "//media.tenor.com/abc/cat.webm", "//media.tenor.com/abc/cat.gif", GifType::Video, Platform::Size{400, 300} };
    const FavoriteGif gif = FavoriteGifRules::stored(candidate, 1);

    REQUIRE(gif.src == std::string("https://media.tenor.com/abc/cat.gif"));
    REQUIRE(gif.format == GifType::Image);
}

TEST_CASE("Favorite GIFs: testProtocolRelativeSourceGainsHttps")
{
    const FavoriteGifCandidate candidate{ "https://tenor.com/view/cat-123", "//media.tenor.com/abc/cat.mp4", {}, GifType::Video, Platform::Size{400, 300} };
    const FavoriteGif gif = FavoriteGifRules::stored(candidate, 1);

    REQUIRE(gif.src == std::string("https://media.tenor.com/abc/cat.mp4"));
    REQUIRE(gif.format == GifType::Video);
}

TEST_CASE("Favorite GIFs: testOrderFollowsTheHighestExisting")
{
    REQUIRE(FavoriteGifRules::orderAfter({}) == 1u);

    FavoriteGif low;
    low.order = 3;
    FavoriteGif high;
    high.order = 41;
    REQUIRE(FavoriteGifRules::orderAfter({ high, low }) == 42u);
}

TEST_CASE("Favorite GIFs: testNewestComesFirst")
{
    FavoriteGif first;
    first.url = "first";
    first.order = 1;
    FavoriteGif second;
    second.url = "second";
    second.order = 2;
    FavoriteGif third;
    third.url = "third";
    third.order = 3;

    const std::vector<FavoriteGif> sorted = FavoriteGifRules::newestFirst({ second, third, first });
    REQUIRE(sorted[0].url == std::string("third"));
    REQUIRE(sorted[1].url == std::string("second"));
    REQUIRE(sorted[2].url == std::string("first"));
}

TEST_CASE("Favorite GIFs: testSearchIgnoresCaseDashesUnderscoresAndSpaces")
{
    const std::string url = "https://tenor.com/view/Happy-Cat_dance-123";
    REQUIRE(FavoriteGifRules::matchesSearch(url, "happy cat"));
    REQUIRE(FavoriteGifRules::matchesSearch(url, "CAT_DANCE"));
    REQUIRE(FavoriteGifRules::matchesSearch(url, "catdance"));
    REQUIRE(!FavoriteGifRules::matchesSearch(url, "dog"));
}

TEST_CASE("Favorite GIFs: testOnlyDiscordHostedSourcesAreLoaded")
{
    FavoriteGif direct;
    direct.src = "https://media.tenor.com/abc/cat.mp4";
    direct.format = GifType::Video;
    REQUIRE(FavoriteGifRules::clipServedByDiscord(direct).empty());
    REQUIRE(FavoriteGifRules::stillServedByDiscord(direct).empty());

    FavoriteGif insecure;
    insecure.src = "http://media.discordapp.net/attachments/1/2/cat.gif";
    REQUIRE(FavoriteGifRules::clipServedByDiscord(insecure).empty());
}

TEST_CASE("Favorite GIFs: testTileUrlsForAGifSource")
{
    FavoriteGif gif;
    gif.format = GifType::Image;

    gif.src = "https://media.discordapp.net/attachments/1/2/cat.gif?ex=aa&format=webp&animated=true";
    REQUIRE(FavoriteGifRules::clipServedByDiscord(gif) == std::string("https://media.discordapp.net/attachments/1/2/cat.gif?ex=aa&format=webp&animated=true"));
    REQUIRE(FavoriteGifRules::stillServedByDiscord(gif) == std::string("https://media.discordapp.net/attachments/1/2/cat.gif?ex=aa"));

    gif.src = "https://images-ext-1.discordapp.net/external/sig/https/x.test/cat.gif";
    REQUIRE(FavoriteGifRules::clipServedByDiscord(gif) == std::string("https://images-ext-1.discordapp.net/external/sig/https/x.test/cat.gif?format=webp&animated=true"));
    REQUIRE(FavoriteGifRules::stillServedByDiscord(gif) == std::string("https://images-ext-1.discordapp.net/external/sig/https/x.test/cat.gif"));

    gif.src = "https://media.discordapp.net/attachments/1/2/cat.avif?ex=aa";
    REQUIRE(FavoriteGifRules::clipServedByDiscord(gif) == std::string("https://media.discordapp.net/attachments/1/2/cat.avif?ex=aa&format=webp&animated=true"));

    gif.src = "https://media.discordapp.net/attachments/1/2/cat.webp?ex=aa";
    REQUIRE(FavoriteGifRules::clipServedByDiscord(gif) == std::string("https://media.discordapp.net/attachments/1/2/cat.webp?ex=aa&animated=true"));
}

TEST_CASE("Favorite GIFs: testTileUrlsForAVideoSource")
{
    FavoriteGif gif;
    gif.format = GifType::Video;
    gif.src = "https://images-ext-1.discordapp.net/external/sig/https/media.tenor.com/abc/cat.mp4";

    REQUIRE(FavoriteGifRules::clipServedByDiscord(gif) == gif.src);
    REQUIRE(FavoriteGifRules::stillServedByDiscord(gif) == gif.src);
}

TEST_CASE("Favorite GIFs: testGutterTightensAsColumnsAreAdded")
{
    using namespace UI::GifMasonry;
    REQUIRE(gutterFor(1) == RoomiestGutter);
    REQUIRE(gutterFor(2) == 12);
    REQUIRE(gutterFor(3) == 8);
    REQUIRE(gutterFor(4) == 6);
    REQUIRE(gutterFor(5) == TightestGutter);
    REQUIRE(gutterFor(MostColumns) == TightestGutter);
}

TEST_CASE("Favorite GIFs: testDenseGridFitsMoreColumnsInTheSameWidth")
{
    using namespace UI::GifMasonry;
    std::vector<Platform::Size> sevenSquares;
    for (int i = 0; i < 7; ++i)
        sevenSquares.push_back(Platform::Size{100, 100});
    const Arrangement arrangement = arrange(sevenSquares, 498, MostColumns);

    const int columnWidth = 78;
    REQUIRE(arrangement.gutter == TightestGutter);
    for (int column = 0; column < MostColumns; ++column)
        REQUIRE((arrangement.tiles[column] == Platform::Rect{static_cast<float>(TightestGutter + column * (columnWidth + TightestGutter)), TightestGutter, columnWidth, columnWidth}));
    REQUIRE((arrangement.tiles[6] == Platform::Rect{TightestGutter, TightestGutter + columnWidth + TightestGutter, columnWidth, columnWidth}));
    REQUIRE(arrangement.tiles[5].x + arrangement.tiles[5].width <= 498);
}

TEST_CASE("Favorite GIFs: testTilesFillTheShortestColumn")
{
    using namespace UI::GifMasonry;
    const Arrangement arrangement = arrange({ Platform::Size{100, 200}, Platform::Size{100, 50}, Platform::Size{100, 50}, Platform::Size{100, 100} }, 436, 2);

    const int Gutter = RoomiestGutter;
    const int columnWidth = 200;
    REQUIRE(arrangement.tiles.size() == 4);
    REQUIRE((arrangement.tiles[0] == Platform::Rect{Gutter, Gutter, columnWidth, 400}));
    REQUIRE((arrangement.tiles[1] == Platform::Rect{Gutter + columnWidth + Gutter, Gutter, columnWidth, 100}));
    REQUIRE((arrangement.tiles[2] == Platform::Rect{Gutter + columnWidth + Gutter, Gutter + 100 + Gutter, columnWidth, 100}));
    REQUIRE((arrangement.tiles[3] == Platform::Rect{Gutter + columnWidth + Gutter, Gutter + 2 * (100 + Gutter), columnWidth, 200}));
    REQUIRE(arrangement.contentHeight == Gutter + 2 * (100 + Gutter) + 200 + Gutter);
}

TEST_CASE("Favorite GIFs: testTileHeightKeepsTheAspectRatio")
{
    using namespace UI::GifMasonry;
    const Arrangement arrangement = arrange({ Platform::Size{498, 280}, Platform::Size{0, 0} }, 236, 1);

    const int Gutter = RoomiestGutter;
    REQUIRE((arrangement.tiles[0] == Platform::Rect{Gutter, Gutter, 212, 119}));
    REQUIRE((Platform::Size{static_cast<int>(arrangement.tiles[1].width), static_cast<int>(arrangement.tiles[1].height)} == Platform::Size{212, 212}));
    REQUIRE(arrange({}, 236, 1).contentHeight == Gutter);
}

TEST_CASE("Favorite GIFs: testStillsComeInTwoSizesWhateverTheDensity")
{
    using namespace UI::GifMasonry;
    const Platform::Size natural(498, 280);
    const int viewportWidth = 498;

    std::vector<Platform::Size> sizesAskedFor;
    for (int columns = FewestColumns; columns <= MostColumns; ++columns) {
        const int tileWidth = arrange({ natural }, viewportWidth, columns).tiles[0].width;
        const Platform::Size still = stillSizeFor(natural, tileWidth);
        REQUIRE(still.width >= tileWidth);
        if (std::find(sizesAskedFor.begin(), sizesAskedFor.end(), still) == sizesAskedFor.end())
            sizesAskedFor.push_back(still);
    }

    REQUIRE((sizesAskedFor == std::vector<Platform::Size>{Platform::Size{RoomyStillWidth, 144}, Platform::Size{DenseStillWidth, 72}}));
    REQUIRE((stillSizeFor(Platform::Size{0, 0}, 100) == Platform::Size{DenseStillWidth, DenseStillWidth}));
}


