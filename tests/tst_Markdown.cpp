#include "Core/EmojiSegmenter.hpp"
#include "Core/Markdown/Parser.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

using namespace Styx::Core::Markdown;

static std::string renderInline(const std::string &source)
{
    Parser parser;
    ParseState state;
    state.isInline = true;
    return parser.toHtml(parser.parse(source, state));
}

static bool isSingleLink(const std::string &source)
{
    Parser parser;
    ParseState state;
    state.isInline = true;
    return Parser::isSingleLink(parser.parse(source, state));
}

TEST_CASE("Markdown newlines", "[markdown]")
{
    CHECK(renderInline("i\nlove\n\ncats") == "i<br>love<br><br>cats");
}

TEST_CASE("Markdown autolinks and URL normalization", "[markdown]")
{
    CHECK(renderInline("<https://example.com>") == "<a href=\"https://example.com/\">https://example.com/</a>");
    CHECK(renderInline("a <https://example.com> b") == "a <a href=\"https://example.com/\">https://example.com/</a> b");
    CHECK(renderInline("https://example.com") == "<a href=\"https://example.com/\">https://example.com/</a>");
    CHECK(renderInline("<https://example.com/a/b>") == "<a href=\"https://example.com/a/b\">https://example.com/a/b</a>");
    CHECK(renderInline("<discord://foo>") == "<a href=\"discord://foo\">discord://foo</a>");
    CHECK(renderInline("<ftp://example.com>") == "ftp://example.com");
    CHECK(renderInline("<https://>") == "https://");
    CHECK(renderInline("https://en.wikipedia.org/wiki/Django_(web_framework)") == "<a href=\"https://en.wikipedia.org/wiki/Django_(web_framework)\">https://en.wikipedia.org/wiki/Django_(web_framework)</a>");
    CHECK(renderInline("(https://example.com)") == "(<a href=\"https://example.com/\">https://example.com/</a>)");
    CHECK(renderInline("https://example.com))") == "<a href=\"https://example.com)/\">https://example.com)/</a>)");
    CHECK(renderInline("https://localhost:3000") == "<a href=\"https://localhost:3000/\">https://localhost:3000/</a>");
}

TEST_CASE("Markdown single-link detection", "[markdown]")
{
    CHECK(isSingleLink("https://klipy.com/gifs/cat-jump"));
    CHECK(isSingleLink("https://tenor.com/view/cat-gif-12345"));
    CHECK(isSingleLink("[a cat](https://klipy.com/gifs/cat-jump)"));
    CHECK(!isSingleLink("look https://klipy.com/gifs/cat-jump"));
    CHECK(!isSingleLink("https://klipy.com/gifs/cat-jump lol"));
    CHECK(!isSingleLink("https://klipy.com/gifs/a https://klipy.com/gifs/b"));
    CHECK(!isSingleLink("**https://klipy.com/gifs/cat-jump**"));
    CHECK(!isSingleLink("||https://klipy.com/gifs/cat-jump||"));
    CHECK(!isSingleLink("just text"));
    CHECK(!isSingleLink(""));
}

TEST_CASE("Emoji segmentation preserves sequences", "[markdown][emoji]")
{
    using Styx::Core::countUnicodeEmojisSegmented;
    using Styx::Core::extractEmojiSequences;
    const std::string thumbsUp = "\xF0\x9F\x91\x8D";
    const std::string thumbsUpTone2 = "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBC";
    const std::string family = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
    const std::string keycapOne = "1\xEF\xB8\x8F\xE2\x83\xA3";
    CHECK((extractEmojiSequences("hi " + thumbsUpTone2 + " and " + family + keycapOne + "!") == std::vector<std::string>{thumbsUpTone2, family, keycapOne}));
    CHECK(extractEmojiSequences("plain text <:pepe:123> 1 + 1").empty());
    CHECK(countUnicodeEmojisSegmented(thumbsUp + " " + family) == 2);
    CHECK(countUnicodeEmojisSegmented("x " + thumbsUp) == -1);
    CHECK(countUnicodeEmojisSegmented("   ") == 0);
}
