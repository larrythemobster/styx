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
    REQUIRE(rejected200.error == "Unexpected status code 200");

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
    REQUIRE(malformed.error == "Invalid poll message response");

    // Non-object JSON
    const auto nonObject = endPollResult(makeResponse(true, 200, R"(["not_an_object"])"), channelId, messageId);
    REQUIRE_FALSE(nonObject.success());
    REQUIRE(nonObject.error == "Invalid poll message response");

    // Missing poll
    auto missingPollJson = makeValidPollMessageJson("100", "200");
    missingPollJson.erase("poll");
    const auto missingPoll = endPollResult(makeResponse(true, 200, missingPollJson.dump()), channelId, messageId);
    REQUIRE_FALSE(missingPoll.success());
    REQUIRE(missingPoll.error == "Invalid poll message response");

    // Wrong message id
    const auto validJsonStr = makeValidPollMessageJson("100", "200").dump();
    const auto wrongMessageId = endPollResult(makeResponse(true, 200, validJsonStr), channelId, Snowflake(999));
    REQUIRE_FALSE(wrongMessageId.success());
    REQUIRE(wrongMessageId.error == "Poll message response does not match the requested message");

    // Wrong channel id
    const auto wrongChannelId = endPollResult(makeResponse(true, 200, validJsonStr), Snowflake(999), messageId);
    REQUIRE_FALSE(wrongChannelId.success());
    REQUIRE(wrongChannelId.error == "Poll message response does not match the requested message");

    // Invalid typed poll content
    auto invalidPollJson = makeValidPollMessageJson("100", "200");
    invalidPollJson["poll"]["answers"] = "invalid_string_instead_of_array";
    const auto invalidTyped = endPollResult(makeResponse(true, 200, invalidPollJson.dump()), channelId, messageId);
    REQUIRE_FALSE(invalidTyped.success());
    REQUIRE(invalidTyped.error == "Invalid poll message response");

    auto invalidScalarJson = makeValidPollMessageJson("100", "200");
    invalidScalarJson["poll"]["allow_multiselect"] = "not a boolean";
    const auto invalidScalar = endPollResult(makeResponse(true, 200, invalidScalarJson.dump()), channelId, messageId);
    REQUIRE_FALSE(invalidScalar.success());
    REQUIRE(invalidScalar.error == "Invalid poll message response");

    // Successful 204
    const auto status204 = endPollResult(makeResponse(true, 204, validJsonStr), channelId, messageId);
    REQUIRE_FALSE(status204.success());
    REQUIRE(status204.error == "Unexpected status code 204");

    // HTTP 403 API error preservation
    const auto forbidden = endPollResult(makeResponse(false, 403, R"({"code":50013,"message":"Missing Permissions"})"), channelId, messageId);
    REQUIRE_FALSE(forbidden.success());
    REQUIRE(forbidden.code == 50013);
    REQUIRE(forbidden.error == "Missing Permissions");
}
