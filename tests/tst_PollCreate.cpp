#include "Discord/PollCreate.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Styx::Discord;
using Styx::Core::Snowflake;

namespace {

std::string repeatAstral(std::string_view glyph, size_t count)
{
    std::string result;
    result.reserve(glyph.size() * count);
    for (size_t i = 0; i < count; ++i)
        result += glyph;
    return result;
}

PollCreateRequest makeValidBaseRequest()
{
    PollCreateRequest request;
    request.question = "Base question?";
    request.answers = {
        { "Option A", std::nullopt },
        { "Option B", std::nullopt }
    };
    request.durationHours = 24;
    request.allowMultiselect = false;
    return request;
}

} // namespace

TEST_CASE("pollCreateBody serializes exact wire format with mixed answers and multiselect", "[polls]")
{
    const Snowflake customEmojiId(998877665544332211ULL);

    PollCreateRequest request;
    request.question = "  Favorite programming language?  ";
    request.answers = {
        { " C++ ", std::nullopt },
        { "Rust", Emoji::custom(customEmojiId, "ferris", true) },
        { "Python", Emoji::unicode("🐍") }
    };
    request.durationHours = 48;
    request.allowMultiselect = true;

    const auto result = pollCreateBody(request);
    REQUIRE(result.success());

    const auto expected = nlohmann::json::parse(R"({
        "question": {
            "text": "  Favorite programming language?  "
        },
        "answers": [
            {
                "poll_media": {
                    "text": " C++ "
                }
            },
            {
                "poll_media": {
                    "text": "Rust",
                    "emoji": {
                        "id": "998877665544332211"
                    }
                }
            },
            {
                "poll_media": {
                    "text": "Python",
                    "emoji": {
                        "name": "🐍"
                    }
                }
            }
        ],
        "duration": 48,
        "allow_multiselect": true,
        "layout_type": 1
    })");

    REQUIRE(*result.value == expected);
}

TEST_CASE("pollCreateBody enforces question and answer codepoint limits on multibyte astral text", "[polls]")
{
    auto req300 = makeValidBaseRequest();
    req300.question = repeatAstral("🚀", 300);
    const auto res300 = pollCreateBody(req300);
    REQUIRE(res300.success());
    REQUIRE(res300.value->at("question").at("text") == req300.question);

    auto req301 = makeValidBaseRequest();
    req301.question = repeatAstral("🚀", 301);
    REQUIRE_FALSE(pollCreateBody(req301).success());

    auto req55 = makeValidBaseRequest();
    req55.answers[0].text = repeatAstral("🚀", 55);
    const auto res55 = pollCreateBody(req55);
    REQUIRE(res55.success());
    REQUIRE(res55.value->at("answers").at(0).at("poll_media").at("text") == req55.answers[0].text);

    auto req56 = makeValidBaseRequest();
    req56.answers[0].text = repeatAstral("🚀", 56);
    REQUIRE_FALSE(pollCreateBody(req56).success());
}

TEST_CASE("pollCreateBody accepts between 2 and 10 answers and preserves terminal answer content", "[polls]")
{
    auto req1 = makeValidBaseRequest();
    req1.answers = { { "Only one", std::nullopt } };
    REQUIRE_FALSE(pollCreateBody(req1).success());

    auto req2 = makeValidBaseRequest();
    req2.answers = { { "First", std::nullopt }, { "Terminal 2", std::nullopt } };
    const auto res2 = pollCreateBody(req2);
    REQUIRE(res2.success());
    REQUIRE(res2.value->at("answers").at(1).at("poll_media").at("text") == "Terminal 2");

    auto req10 = makeValidBaseRequest();
    req10.answers.clear();
    for (int i = 1; i <= 9; ++i)
        req10.answers.push_back({ "Option " + std::to_string(i), std::nullopt });
    req10.answers.push_back({ "Terminal 10", std::nullopt });
    const auto res10 = pollCreateBody(req10);
    REQUIRE(res10.success());
    REQUIRE(res10.value->at("answers").at(9).at("poll_media").at("text") == "Terminal 10");

    auto req11 = makeValidBaseRequest();
    req11.answers.clear();
    for (int i = 1; i <= 11; ++i)
        req11.answers.push_back({ "Option " + std::to_string(i), std::nullopt });
    REQUIRE_FALSE(pollCreateBody(req11).success());
}

TEST_CASE("pollCreateBody enforces duration bounds", "[polls]")
{
    auto req = makeValidBaseRequest();

    req.durationHours = 1;
    const auto shortest = pollCreateBody(req);
    REQUIRE(shortest.success());
    CHECK(shortest.value->at("duration") == 1);

    req.durationHours = 768;
    const auto longest = pollCreateBody(req);
    REQUIRE(longest.success());
    CHECK(longest.value->at("duration") == 768);

    req.durationHours = 0;
    REQUIRE_FALSE(pollCreateBody(req).success());

    req.durationHours = 769;
    REQUIRE_FALSE(pollCreateBody(req).success());
}

TEST_CASE("pollCreateBody rejects empty text, malformed UTF-8, and invalid emoji specifications", "[polls]")
{
    auto reqEmptyQ = makeValidBaseRequest();
    reqEmptyQ.question = "";
    REQUIRE_FALSE(pollCreateBody(reqEmptyQ).success());

    auto reqBlankQ = makeValidBaseRequest();
    reqBlankQ.question = "  \t \n ";
    REQUIRE_FALSE(pollCreateBody(reqBlankQ).success());

    auto reqEmptyA = makeValidBaseRequest();
    reqEmptyA.answers[0].text = "";
    REQUIRE_FALSE(pollCreateBody(reqEmptyA).success());

    auto reqBlankA = makeValidBaseRequest();
    reqBlankA.answers[0].text = "   \n";
    REQUIRE_FALSE(pollCreateBody(reqBlankA).success());

    auto reqInvalidUtf8Q = makeValidBaseRequest();
    reqInvalidUtf8Q.question = "Invalid \xFF\xFE UTF8";
    REQUIRE_FALSE(pollCreateBody(reqInvalidUtf8Q).success());

    auto reqInvalidUtf8A = makeValidBaseRequest();
    reqInvalidUtf8A.answers[0].text = "Bad \xC0\xAF bytes";
    REQUIRE_FALSE(pollCreateBody(reqInvalidUtf8A).success());

    auto reqEmptyUni = makeValidBaseRequest();
    reqEmptyUni.answers[0].emoji = Emoji::unicode("");
    REQUIRE_FALSE(pollCreateBody(reqEmptyUni).success());

    auto reqBlankUni = makeValidBaseRequest();
    reqBlankUni.answers[0].emoji = Emoji::unicode("   ");
    REQUIRE_FALSE(pollCreateBody(reqBlankUni).success());

    auto reqInvalidUtf8Uni = makeValidBaseRequest();
    reqInvalidUtf8Uni.answers[0].emoji = Emoji::unicode("\xED\xA0\x80");
    REQUIRE_FALSE(pollCreateBody(reqInvalidUtf8Uni).success());

    auto reqInvalidCustom = makeValidBaseRequest();
    reqInvalidCustom.answers[0].emoji = Emoji::custom(Snowflake::Invalid, "custom", false);
    REQUIRE_FALSE(pollCreateBody(reqInvalidCustom).success());

    auto reqZeroCustom = makeValidBaseRequest();
    reqZeroCustom.answers[0].emoji = Emoji::custom(Snowflake(0), "custom", false);
    REQUIRE_FALSE(pollCreateBody(reqZeroCustom).success());
}
