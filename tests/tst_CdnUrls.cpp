#include "Discord/CdnUrls.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>

using namespace Styx;
using namespace Styx::Discord;

TEST_CASE("CdnUrls: testGifIsAnimatedWhateverItsFlags", "[cdn]")
{
    REQUIRE(Cdn::isAnimatedImage("https://cdn.discordapp.com/attachments/1/2/cat.gif?ex=a&is=b&hm=c&", false));
    REQUIRE(Cdn::isAnimatedImage("https://cdn.discordapp.com/attachments/1/2/CAT.GIF", false));
    REQUIRE(Cdn::isAnimatedImage("https://example.com/a.gif#frag", false));
    REQUIRE(!Cdn::isAnimatedImage("https://cdn.discordapp.com/attachments/1/2/cat.png?ex=a", false));
    REQUIRE(!Cdn::isAnimatedImage("https://cdn.discordapp.com/attachments/1/2/cat.png?ex=a", true));
}

TEST_CASE("CdnUrls: testWebpAndAvifNeedTheAnimatedFlag", "[cdn]")
{
    const std::string webp = "https://cdn.discordapp.com/attachments/1/2/cat.webp?ex=a&is=b&hm=c&";
    const std::string avif = "https://cdn.discordapp.com/attachments/1/2/cat.avif?ex=a&is=b&hm=c&";
    REQUIRE(!Cdn::isAnimatedImage(webp, false));
    REQUIRE(Cdn::isAnimatedImage(webp, true));
    REQUIRE(!Cdn::isAnimatedImage(avif, false));
    REQUIRE(Cdn::isAnimatedImage(avif, true));
}

TEST_CASE("CdnUrls: testExtensionMustBeFollowedByEndQueryOrFragment", "[cdn]")
{
    REQUIRE(!Cdn::isAnimatedImage("https://example.com/a.gifv", false));
    REQUIRE(!Cdn::isAnimatedImage("https://example.com/a.gif.png", false));
    REQUIRE(Cdn::isAnimatedImage("https://example.com/a.gif.png?source=b.gif", false));
}

TEST_CASE("CdnUrls: testAnimatedGifUrlAsksOnlyForItsSize", "[cdn]")
{
    const std::string proxy = "https://media.discordapp.net/attachments/1/2/cat.gif?ex=a&is=b&hm=c";
    const std::string animated = Cdn::animatedImageUrl(proxy, false, Platform::Size{800, 600}, Platform::Size{400, 300});
    REQUIRE(animated == "https://media.discordapp.net/attachments/1/2/cat.gif?ex=a&is=b&hm=c&width=400&height=300");
}

TEST_CASE("CdnUrls: testAnimatedGifUrlAtItsOwnSizeIsTheProxyUrl", "[cdn]")
{
    const std::string proxy = "https://media.discordapp.net/attachments/1/2/cat.gif?ex=a&is=b&hm=c";
    REQUIRE(Cdn::animatedImageUrl(proxy, false, Platform::Size{320, 240}, Platform::Size{320, 240}) == proxy);
}

TEST_CASE("CdnUrls: testAnimatedWebpUrlAsksForAnimation", "[cdn]")
{
    const std::string proxy = "https://media.discordapp.net/attachments/1/2/cat.webp?ex=a&is=b&hm=c";
    REQUIRE(Cdn::animatedImageUrl(proxy, true, Platform::Size{800, 600}, Platform::Size{400, 300}) == "https://media.discordapp.net/attachments/1/2/cat.webp?ex=a&is=b&hm=c&animated=true&width=400&height=300");
    REQUIRE(Cdn::animatedImageUrl(proxy, false, Platform::Size{800, 600}, Platform::Size{400, 300}) == "https://media.discordapp.net/attachments/1/2/cat.webp?ex=a&is=b&hm=c&width=400&height=300");
}

TEST_CASE("CdnUrls: testAnimatedAvifUrlAsksForAnimatedWebp", "[cdn]")
{
    const std::string proxy = "https://media.discordapp.net/attachments/1/2/cat.avif?ex=a&is=b&hm=c";
    REQUIRE(Cdn::animatedImageUrl(proxy, true, Platform::Size{800, 600}, Platform::Size{400, 300}) == "https://media.discordapp.net/attachments/1/2/cat.avif?ex=a&is=b&hm=c&animated=true&format=webp&width=400&height=300");
}

TEST_CASE("CdnUrls: testRequestedSizeIsCappedAtTheProxyLimit", "[cdn]")
{
    const std::string proxy = "https://media.discordapp.net/attachments/1/2/wide.gif";
    REQUIRE(Cdn::animatedImageUrl(proxy, false, Platform::Size{9000, 100}, Platform::Size{8192, 1000}) == "https://media.discordapp.net/attachments/1/2/wide.gif?width=4096&height=500");
    REQUIRE(Cdn::animatedImageUrl(proxy, false, Platform::Size{100, 9000}, Platform::Size{1000, 8192}) == "https://media.discordapp.net/attachments/1/2/wide.gif?width=500&height=4096");
}

TEST_CASE("CdnUrls: testCdnHostUrlIsLeftAlone", "[cdn]")
{
    const std::string cdn = "https://cdn.discordapp.com/attachments/1/2/cat.gif?ex=a&is=b&hm=c";
    REQUIRE(Cdn::animatedImageUrl(cdn, false, Platform::Size{800, 600}, Platform::Size{400, 300}) == cdn);
}

TEST_CASE("CdnUrls: testDiscordAssetHosts", "[cdn]")
{
    REQUIRE(Cdn::isDiscordAssetUrl("https://cdn.discordapp.com/attachments/1/2/cat.gif"));
    REQUIRE(Cdn::isDiscordAssetUrl("https://media.discordapp.net/attachments/1/2/cat.gif"));
    REQUIRE(Cdn::isDiscordAssetUrl("https://images-ext-1.discordapp.net/external/abc/https/media.tenor.com/x/y.mp4"));
    REQUIRE(!Cdn::isDiscordAssetUrl("http://media.discordapp.net/attachments/1/2/cat.gif"));
    REQUIRE(!Cdn::isDiscordAssetUrl("https://media.tenor.com/x/y.mp4"));
    REQUIRE(!Cdn::isDiscordAssetUrl("https://discordapp.net.evil.example/x.mp4"));
    REQUIRE(!Cdn::isDiscordAssetUrl("https://evildiscordapp.net/x.mp4"));
    REQUIRE(!Cdn::isDiscordAssetUrl("file:///C:/x.gif"));
}

TEST_CASE("CdnUrls: testLoopingVideoLimitAllowsEitherOrientation", "[cdn]")
{
    REQUIRE(Cdn::fitsLoopingVideoLimit(Platform::Size{498, 280}));
    REQUIRE(Cdn::fitsLoopingVideoLimit(Platform::Size{6016, 3384}));
    REQUIRE(Cdn::fitsLoopingVideoLimit(Platform::Size{3384, 6016}));
    REQUIRE(!Cdn::fitsLoopingVideoLimit(Platform::Size{6017, 3384}));
    REQUIRE(!Cdn::fitsLoopingVideoLimit(Platform::Size{4000, 4000}));
}

