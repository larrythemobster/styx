#include "Discord/Entities.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using namespace Styx::Discord;

static Embed embed(const char *json)
{
    return Embed::fromJson(nlohmann::json::parse(json));
}

static const char *KlipyGifv = R"({"type":"gifv","url":"https://klipy.com/gifs/cat","provider":{"name":"Klipy"},
    "thumbnail":{"url":"https://static.klipy.com/cat.webp","proxy_url":"https://images-ext-1.discordapp.net/external/s/https/static.klipy.com/cat.webp","width":498,"height":280},
    "video":{"url":"https://static.klipy.com/cat.mp4","proxy_url":"https://images-ext-1.discordapp.net/external/s/https/static.klipy.com/cat.mp4","width":498,"height":280}})";

static const char *PlainImage = R"({"type":"image","url":"https://example.com/cat.png",
    "thumbnail":{"url":"https://example.com/cat.png","proxy_url":"https://images-ext-1.discordapp.net/external/s/https/example.com/cat.png","width":640,"height":480}})";

TEST_CASE("Embeds: testGifvIsBareMedia", "[embeds]")
{
    REQUIRE(embed(KlipyGifv).isBareMedia());
    REQUIRE(embed(R"({"type":"gifv","title":"Cat","author":{"name":"someone"},"thumbnail":{"url":"https://a/b.png","width":10,"height":10},"video":{"url":"https://a/b.mp4","width":10,"height":10}})").isBareMedia());
    REQUIRE(embed(R"({"type":"gifv","video":{"url":"https://a/b.mp4","proxy_url":"https://p/b.mp4","width":10,"height":10}})").isBareMedia());

    REQUIRE(!embed(R"({"type":"gifv","video":{"url":"https://a/b.mp4","width":10,"height":10}})").isBareMedia());
    REQUIRE(!embed(R"({"type":"gifv","thumbnail":{"url":"https://a/b.png","width":10,"height":10},"video":{"url":"http://a/b.mp4","width":10,"height":10}})").isBareMedia());
    REQUIRE(!embed(R"({"type":"gifv","thumbnail":{"url":"https://a/b.png","width":10,"height":10}})").isBareMedia());
}

TEST_CASE("Embeds: testImageIsBareMediaWithoutTitleOrAuthor", "[embeds]")
{
    REQUIRE(embed(PlainImage).isBareMedia());
    REQUIRE(embed(R"({"type":"image","image":{"url":"https://a/b.png","width":10,"height":10}})").isBareMedia());

    REQUIRE(!embed(R"({"type":"image","title":"Cat","thumbnail":{"url":"https://a/b.png","width":10,"height":10}})").isBareMedia());
    REQUIRE(!embed(R"({"type":"image","author":{"name":"someone"},"thumbnail":{"url":"https://a/b.png","width":10,"height":10}})").isBareMedia());
    REQUIRE(!embed(R"({"type":"image","url":"https://a/b.png"})").isBareMedia());
}

TEST_CASE("Embeds: testOtherTypesAreNotBareMedia", "[embeds]")
{
    for (const char *type : { "rich", "article", "video", "link" }) {
        const std::string json = std::string(R"({"type":")") + type + R"(","image":{"url":"https://a/b.png","width":10,"height":10},"thumbnail":{"url":"https://a/b.png","width":10,"height":10}})";
        INFO(type);
        REQUIRE(!embed(json.c_str()).isBareMedia());
    }
    REQUIRE(!embed(R"({"image":{"url":"https://a/b.png","width":10,"height":10}})").isBareMedia());
}

TEST_CASE("Embeds: testMediaWithoutSizeDoesNotCount", "[embeds]")
{
    REQUIRE(!embed(R"({"type":"image","thumbnail":{"url":"https://a/b.png"}})").isBareMedia());
    REQUIRE(!embed(R"({"type":"image","thumbnail":{"url":"https://a/b.png","width":0,"height":0}})").isBareMedia());
    REQUIRE(!embed(R"({"type":"gifv","thumbnail":{"url":"https://a/b.png","width":10,"height":10},"video":{"url":"https://a/b.mp4","proxy_url":"https://p/b.mp4"}})").isBareMedia());
}

TEST_CASE("Embeds: testOnlyALoneLinkIsHiddenBehindItsEmbed", "[embeds]")
{
    Message message;
    message.embeds = std::vector<Embed>{ embed(KlipyGifv) };
    REQUIRE(!message.hidesLinkBehindItsEmbed());

    message.contentIsSingleLink = true;
    REQUIRE(message.hidesLinkBehindItsEmbed());

    message.embeds = std::vector<Embed>{ embed(KlipyGifv), embed(PlainImage) };
    REQUIRE(!message.hidesLinkBehindItsEmbed());

    message.embeds = std::vector<Embed>{ embed(R"({"type":"article","title":"News","thumbnail":{"url":"https://a/b.png","width":10,"height":10}})") };
    REQUIRE(!message.hidesLinkBehindItsEmbed());

    Message withoutEmbeds;
    withoutEmbeds.contentIsSingleLink = true;
    REQUIRE(!withoutEmbeds.hidesLinkBehindItsEmbed());
}

