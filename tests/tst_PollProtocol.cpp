#include "Discord/PollProtocol.hpp"
#include "Discord/HttpClient.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

using namespace Styx::Discord;
using Styx::Core::Snowflake;

namespace {

HttpResponse makeResponse(bool success, int statusCode, std::string body = {})
{
    HttpResponse response;
    response.success = success;
    response.statusCode = statusCode;
    response.body = std::move(body);
    return response;
}

nlohmann::json makeValidPollMessageJson(const std::string &channelId = "100",
                                        const std::string &messageId = "200")
{
    return nlohmann::json::parse(R"({
        "id": ")" + messageId + R"(",
        "channel_id": ")" + channelId + R"(",
        "poll": {
            "question": {"text": "Favorite language?"},
            "answers": [
                {"answer_id": 1, "poll_media": {"text": "C++"}},
                {"answer_id": 2, "poll_media": {"text": "Rust"}}
            ],
            "expiry": "2026-11-10T12:00:00+00:00",
            "allow_multiselect": false,
            "layout_type": 1,
            "results": {
                "is_finalized": true,
                "answer_counts": [
                    {"id": 1, "count": 10, "me_voted": true},
                    {"id": 2, "count": 5, "me_voted": false}
                ]
            }
        }
    })");
}

nlohmann::json makeValidCreatedPollMessageJson(const std::string &channelId = "100",
                                               const std::string &messageId = "200")
{
    return nlohmann::json::parse(R"({
        "id": ")" + messageId + R"(",
        "channel_id": ")" + channelId + R"(",
        "author": {
            "id": "300"
        },
        "content": "",
        "timestamp": "2026-11-10T12:00:00+00:00",
        "type": 0,
        "flags": 0,
        "poll": {
            "question": {"text": "Favorite language?"},
            "answers": [
                {"answer_id": 1, "poll_media": {"text": "C++"}},
                {"answer_id": 2, "poll_media": {"text": "Rust"}}
            ],
            "expiry": "2026-11-10T13:00:00+00:00",
            "allow_multiselect": false,
            "layout_type": 1
        }
    })");
}

} // namespace

TEST_CASE("pollVoteBody encodes exact integer array and empty list", "[polls]")
{
    const auto body = pollVoteBody({ 7, 42 });
    REQUIRE(body.dump() == R"({"answer_ids":[7,42]})");

    const auto emptyBody = pollVoteBody({});
    REQUIRE(emptyBody.dump() == R"({"answer_ids":[]})");
}

TEST_CASE("pollVoteResult requires HTTP 204 and preserves API error", "[polls]")
{
    const auto okResult = pollVoteResult(makeResponse(true, 204));
    REQUIRE(okResult.success());

    const auto rejected200 = pollVoteResult(makeResponse(true, 200));
    REQUIRE_FALSE(rejected200.success());
    const auto forbidden = pollVoteResult(makeResponse(false, 403, R"({"code":50013,"message":"Missing Permissions"})"));
    REQUIRE_FALSE(forbidden.success());
    REQUIRE(forbidden.code == 50013);
    REQUIRE(forbidden.error == "Missing Permissions");
}

TEST_CASE("endPollResult parses valid expired poll message", "[polls]")
{
    const auto channelId = Snowflake(100);
    const auto messageId = Snowflake(200);
    const auto msgJson = makeValidPollMessageJson("100", "200");

    const auto result = endPollResult(makeResponse(true, 200, msgJson.dump()), channelId, messageId);
    REQUIRE(result.success());

    const auto &message = *result.value;
    REQUIRE(message.id.get() == messageId);
    REQUIRE(message.channelId.get() == channelId);
    REQUIRE(message.poll.hasValue());
    REQUIRE(message.poll->results.hasValue());
    REQUIRE(message.poll->results->isFinalized.hasValue());
    REQUIRE(message.poll->results->isFinalized.get() == true);
    REQUIRE(message.pollJson == msgJson["poll"].dump());
}

TEST_CASE("endPollResult rejects invalid responses and preserves API error", "[polls]")
{
    const auto channelId = Snowflake(100);
    const auto messageId = Snowflake(200);

    // Malformed JSON
    const auto malformed = endPollResult(makeResponse(true, 200, "{malformed"), channelId, messageId);
    REQUIRE_FALSE(malformed.success());

    // Non-object JSON
    const auto nonObject = endPollResult(makeResponse(true, 200, R"(["not_an_object"])"), channelId, messageId);
    REQUIRE_FALSE(nonObject.success());

    // Missing poll
    auto missingPollJson = makeValidPollMessageJson("100", "200");
    missingPollJson.erase("poll");
    const auto missingPoll = endPollResult(makeResponse(true, 200, missingPollJson.dump()), channelId, messageId);
    REQUIRE_FALSE(missingPoll.success());

    // Wrong message id
    const auto validJsonStr = makeValidPollMessageJson("100", "200").dump();
    const auto wrongMessageId = endPollResult(makeResponse(true, 200, validJsonStr), channelId, Snowflake(999));
    REQUIRE_FALSE(wrongMessageId.success());

    // Wrong channel id
    const auto wrongChannelId = endPollResult(makeResponse(true, 200, validJsonStr), Snowflake(999), messageId);
    REQUIRE_FALSE(wrongChannelId.success());

    // Invalid typed poll content
    auto invalidPollJson = makeValidPollMessageJson("100", "200");
    invalidPollJson["poll"]["answers"] = "invalid_string_instead_of_array";
    const auto invalidTyped = endPollResult(makeResponse(true, 200, invalidPollJson.dump()), channelId, messageId);
    REQUIRE_FALSE(invalidTyped.success());

    auto invalidScalarJson = makeValidPollMessageJson("100", "200");
    invalidScalarJson["poll"]["allow_multiselect"] = "not a boolean";
    const auto invalidScalar = endPollResult(makeResponse(true, 200, invalidScalarJson.dump()), channelId, messageId);
    REQUIRE_FALSE(invalidScalar.success());

    // Successful 204
    const auto status204 = endPollResult(makeResponse(true, 204, validJsonStr), channelId, messageId);
    REQUIRE_FALSE(status204.success());

    // HTTP 403 API error preservation
    const auto forbidden = endPollResult(makeResponse(false, 403, R"({"code":50013,"message":"Missing Permissions"})"), channelId, messageId);
    REQUIRE_FALSE(forbidden.success());
    REQUIRE(forbidden.code == 50013);
    REQUIRE(forbidden.error == "Missing Permissions");
}

TEST_CASE("createPollResult parses valid created poll message and preserves fields", "[polls]")
{
    const auto channelId = Snowflake(100);
    auto baseJson = makeValidCreatedPollMessageJson("100", "200");
    baseJson["poll"]["unknown_key"] = "preserved_value";

    SECTION("Omitted results preserves unknown poll keys and empty content") {
        const auto result = createPollResult(makeResponse(true, 200, baseJson.dump()), channelId);
        REQUIRE(result.success());

        const auto &message = *result.value;
        REQUIRE(message.id.get() == Snowflake(200));
        REQUIRE(message.channelId.get() == channelId);
        REQUIRE(message.author->id.get() == Snowflake(300));
        REQUIRE(message.content.get().empty());
        REQUIRE(message.poll.hasValue());
        REQUIRE_FALSE(message.poll->results.hasValue());
        REQUIRE(nlohmann::json::parse(message.pollJson).at("unknown_key") == "preserved_value");
    }

    SECTION("Preserved string nonce") {
        baseJson["nonce"] = "string-nonce-xyz";
        const auto result = createPollResult(makeResponse(true, 200, baseJson.dump()), channelId);
        REQUIRE(result.success());
        REQUIRE(result.value->nonce.hasValue());
        REQUIRE(result.value->nonce.get() == "string-nonce-xyz");
    }

    SECTION("Normalized integer nonce") {
        baseJson["nonce"] = 987654321;
        const auto result = createPollResult(makeResponse(true, 200, baseJson.dump()), channelId);
        REQUIRE(result.success());
        REQUIRE(result.value->nonce.hasValue());
        REQUIRE(result.value->nonce.get() == "987654321");
    }

    SECTION("Arbitrary positive message ID on same channel is accepted") {
        baseJson["id"] = "987654321";
        const auto result = createPollResult(makeResponse(true, 200, baseJson.dump()), channelId);
        REQUIRE(result.success());
        REQUIRE(result.value->id.get() == Snowflake(987654321));
    }

    SECTION("Explicit results with valid answer counts are parsed") {
        baseJson["poll"]["results"] = nlohmann::json::parse(R"({
            "is_finalized": false,
            "answer_counts": [
                {"id": 1, "count": 0, "me_voted": false},
                {"id": 2, "count": 3, "me_voted": true}
            ]
        })");
        const auto result = createPollResult(makeResponse(true, 200, baseJson.dump()), channelId);
        REQUIRE(result.success());
        REQUIRE(result.value->poll->results.hasValue());
        REQUIRE_FALSE(result.value->poll->results->isFinalized.get());
        REQUIRE(result.value->poll->results->answerCounts->at(1).count.get() == 3);
        REQUIRE(result.value->poll->results->answerCounts->at(1).meVoted.get());
    }

    SECTION("Null expiry and optional media emoji are accepted") {
        baseJson["poll"]["expiry"] = nullptr;
        baseJson["poll"]["answers"][0]["poll_media"]["emoji"] = nlohmann::json::parse(R"({
            "id": null,
            "name": "👍"
        })");
        const auto result = createPollResult(makeResponse(true, 200, baseJson.dump()), channelId);
        REQUIRE(result.success());
        REQUIRE(result.value->poll->expiry.isNull());
    }
}

TEST_CASE("createPollResult rejects invalid responses and preserves API error", "[polls]")
{
    const auto channelId = Snowflake(100);
    const auto validJson = makeValidCreatedPollMessageJson("100", "200");

    SECTION("HTTP failure and status code rejection") {
        const auto forbidden = createPollResult(
            makeResponse(false, 403, R"({"code":50013,"message":"Missing Permissions"})"), channelId);
        REQUIRE_FALSE(forbidden.success());
        REQUIRE(forbidden.code == 50013);
        REQUIRE(forbidden.error == "Missing Permissions");

        const auto status204 = createPollResult(makeResponse(true, 204, validJson.dump()), channelId);
        REQUIRE_FALSE(status204.success());
    }

    SECTION("Malformed JSON and non-object root") {
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, "{malformed"), channelId).success());
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, R"(["not_an_object"])"), channelId).success());
    }

    SECTION("Channel mismatch") {
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, validJson.dump()), Snowflake(999)).success());
    }

    SECTION("Canonical identity rejection") {
        for (const auto &badId : { "0", "0100", "-1", "abc", " 100", "100a", "" }) {
            auto msg = validJson;
            msg["id"] = badId;
            REQUIRE_FALSE(createPollResult(makeResponse(true, 200, msg.dump()), channelId).success());

            msg = validJson;
            msg["channel_id"] = badId;
            REQUIRE_FALSE(createPollResult(makeResponse(true, 200, msg.dump()), channelId).success());

            msg = validJson;
            msg["author"]["id"] = badId;
            REQUIRE_FALSE(createPollResult(makeResponse(true, 200, msg.dump()), channelId).success());
        }

        auto nonStringId = validJson;
        nonStringId["id"] = 200;
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, nonStringId.dump()), channelId).success());
    }

    SECTION("Missing or malformed required root fields") {
        for (const std::string &key : { "id", "channel_id", "author", "content", "timestamp", "type", "flags", "poll" }) {
            auto msg = validJson;
            msg.erase(key);
            REQUIRE_FALSE(createPollResult(makeResponse(true, 200, msg.dump()), channelId).success());
        }

        auto badAuthor = validJson;
        badAuthor["author"] = "not_an_object";
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, badAuthor.dump()), channelId).success());

        auto badAuthorId = validJson;
        badAuthorId["author"].erase("id");
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, badAuthorId.dump()), channelId).success());

        auto nonStringContent = validJson;
        nonStringContent["content"] = 123;
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, nonStringContent.dump()), channelId).success());

        auto nonIntegerType = validJson;
        nonIntegerType["type"] = "default";
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, nonIntegerType.dump()), channelId).success());

        auto nonIntegerFlags = validJson;
        nonIntegerFlags["flags"] = false;
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, nonIntegerFlags.dump()), channelId).success());

        auto booleanNonce = validJson;
        booleanNonce["nonce"] = true;
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, booleanNonce.dump()), channelId).success());
    }

    SECTION("Invalid timestamp and expiry") {
        auto badTimestamp = validJson;
        badTimestamp["timestamp"] = "not_an_iso_timestamp";
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, badTimestamp.dump()), channelId).success());

        auto badExpiry = validJson;
        badExpiry["poll"]["expiry"] = "not_an_iso_timestamp";
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, badExpiry.dump()), channelId).success());

        auto numberExpiry = validJson;
        numberExpiry["poll"]["expiry"] = 12345;
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, numberExpiry.dump()), channelId).success());
    }

    SECTION("Missing or malformed poll structure") {
        for (const std::string &key : { "question", "answers", "expiry", "allow_multiselect", "layout_type" }) {
            auto msg = validJson;
            msg["poll"].erase(key);
            REQUIRE_FALSE(createPollResult(makeResponse(true, 200, msg.dump()), channelId).success());
        }

        auto badQuestion = validJson;
        badQuestion["poll"]["question"] = "not_an_object";
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, badQuestion.dump()), channelId).success());

        auto missingQuestionText = validJson;
        missingQuestionText["poll"]["question"].erase("text");
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, missingQuestionText.dump()), channelId).success());

        auto badMultiselect = validJson;
        badMultiselect["poll"]["allow_multiselect"] = "false";
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, badMultiselect.dump()), channelId).success());

        auto badLayout = validJson;
        badLayout["poll"]["layout_type"] = "1";
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, badLayout.dump()), channelId).success());
    }

    SECTION("Malformed answer objects, count limits, duplicate or overflow IDs") {
        auto tooFew = validJson;
        tooFew["poll"]["answers"] = nlohmann::json::parse(R"([
            {"answer_id": 1, "poll_media": {"text": "One"}}
        ])");
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, tooFew.dump()), channelId).success());

        auto tooMany = validJson;
        tooMany["poll"]["answers"] = nlohmann::json::array();
        for (int i = 1; i <= 11; ++i) {
            tooMany["poll"]["answers"].push_back({
                { "answer_id", i },
                { "poll_media", { { "text", std::to_string(i) } } }
            });
        }
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, tooMany.dump()), channelId).success());

        auto duplicateId = validJson;
        duplicateId["poll"]["answers"][1]["answer_id"] = 1;
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, duplicateId.dump()), channelId).success());

        auto nonPositiveId = validJson;
        nonPositiveId["poll"]["answers"][0]["answer_id"] = 0;
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, nonPositiveId.dump()), channelId).success());

        auto overflowId = validJson;
        overflowId["poll"]["answers"][0]["answer_id"] = 3000000000LL;
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, overflowId.dump()), channelId).success());

        auto nonObjectAns = validJson;
        nonObjectAns["poll"]["answers"][0] = "string_answer";
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, nonObjectAns.dump()), channelId).success());

        auto missingMedia = validJson;
        missingMedia["poll"]["answers"][0].erase("poll_media");
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, missingMedia.dump()), channelId).success());

        auto nonObjectMedia = validJson;
        nonObjectMedia["poll"]["answers"][0]["poll_media"] = "media";
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, nonObjectMedia.dump()), channelId).success());

        auto missingMediaText = validJson;
        missingMediaText["poll"]["answers"][0]["poll_media"].erase("text");
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, missingMediaText.dump()), channelId).success());

        auto nonStringMediaText = validJson;
        nonStringMediaText["poll"]["answers"][0]["poll_media"]["text"] = 123;
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, nonStringMediaText.dump()), channelId).success());

        auto nonObjectEmoji = validJson;
        nonObjectEmoji["poll"]["answers"][0]["poll_media"]["emoji"] = "smile";
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, nonObjectEmoji.dump()), channelId).success());
    }

    SECTION("Malformed optional results") {
        auto nonObjectResults = validJson;
        nonObjectResults["poll"]["results"] = "invalid_string";
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, nonObjectResults.dump()), channelId).success());

        auto missingFinalized = validJson;
        missingFinalized["poll"]["results"] = nlohmann::json::parse(R"({
            "answer_counts": []
        })");
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, missingFinalized.dump()), channelId).success());

        auto nonBoolFinalized = validJson;
        nonBoolFinalized["poll"]["results"] = nlohmann::json::parse(R"({
            "is_finalized": "true",
            "answer_counts": []
        })");
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, nonBoolFinalized.dump()), channelId).success());

        auto missingCounts = validJson;
        missingCounts["poll"]["results"] = nlohmann::json::parse(R"({
            "is_finalized": false
        })");
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, missingCounts.dump()), channelId).success());

        auto nonArrayCounts = validJson;
        nonArrayCounts["poll"]["results"] = nlohmann::json::parse(R"({
            "is_finalized": false,
            "answer_counts": "not_an_array"
        })");
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, nonArrayCounts.dump()), channelId).success());

        auto nonObjectCountItem = validJson;
        nonObjectCountItem["poll"]["results"] = nlohmann::json::parse(R"({
            "is_finalized": false,
            "answer_counts": ["not_an_object"]
        })");
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, nonObjectCountItem.dump()), channelId).success());

        auto badCountId = validJson;
        badCountId["poll"]["results"] = nlohmann::json::parse(R"({
            "is_finalized": false,
            "answer_counts": [
                {"id": 0, "count": 1, "me_voted": false}
            ]
        })");
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, badCountId.dump()), channelId).success());

        auto negativeCount = validJson;
        negativeCount["poll"]["results"] = nlohmann::json::parse(R"({
            "is_finalized": false,
            "answer_counts": [
                {"id": 1, "count": -1, "me_voted": false}
            ]
        })");
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, negativeCount.dump()), channelId).success());

        auto nonBoolMeVoted = validJson;
        nonBoolMeVoted["poll"]["results"] = nlohmann::json::parse(R"({
            "is_finalized": false,
            "answer_counts": [
                {"id": 1, "count": 1, "me_voted": "false"}
            ]
        })");
        REQUIRE_FALSE(createPollResult(makeResponse(true, 200, nonBoolMeVoted.dump()), channelId).success());
    }
}
