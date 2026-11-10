#include "Core/PollVotes.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"
#include "Discord/Events.hpp"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>

using namespace Styx::Discord;
using Styx::Core::Snowflake;
using Styx::Core::applyPollVote;

namespace {

static const Snowflake ChannelId(200);
static const Snowflake MessageId(100);
static const Snowflake SelfUserId(500);
static const Snowflake OtherUserId(600);

static nlohmann::json makeSamplePollJson()
{
    return nlohmann::json::parse(R"({
        "question": {"text": "Favorite number?"},
        "answers": [
            {"answer_id": 7, "poll_media": {"text": "Seven"}},
            {"answer_id": 42, "poll_media": {"text": "Forty-two"}},
            {"answer_id": 99, "poll_media": {"text": "Ninety-nine"}}
        ],
        "expiry": "2024-05-02T10:00:02.039342+00:00",
        "allow_multiselect": true,
        "layout_type": 1,
        "poll_opaque_field": "opaque_poll_value",
        "results": {
            "is_finalized": false,
            "results_opaque_field": 4242,
            "answer_counts": [
                {"id": 7, "count": 2, "me_voted": false, "count_opaque_field": "tag7"},
                {"id": 42, "count": 1, "me_voted": false, "count_opaque_field": "tag42"}
            ]
        }
    })");
}

static Message makeSampleMessage(const nlohmann::json &pollJson = makeSamplePollJson())
{
    nlohmann::json obj = {
        { "id", MessageId.toString() },
        { "channel_id", ChannelId.toString() },
        { "poll", pollJson }
    };
    return Message::fromJson(obj);
}

static MessagePollVote makeVoteEvent(Snowflake userId, int answerId,
                                    Snowflake channelId = ChannelId,
                                    Snowflake messageId = MessageId)
{
    nlohmann::json obj = {
        { "user_id", userId.toString() },
        { "channel_id", channelId.toString() },
        { "message_id", messageId.toString() },
        { "answer_id", answerId }
    };
    return MessagePollVote::fromJson(obj);
}

} // namespace

TEST_CASE("applyPollVote adjusts count for other voter and preserves opaque fields", "[poll-votes]")
{
    Message msg = makeSampleMessage();

    REQUIRE(applyPollVote(msg, makeVoteEvent(OtherUserId, 7), true, SelfUserId));
    REQUIRE(msg.poll->results->answerCounts->at(0).count.get() == 3);
    REQUIRE_FALSE(msg.poll->results->answerCounts->at(0).meVoted.get());

    nlohmann::json doc = nlohmann::json::parse(msg.pollJson);
    REQUIRE(doc["poll_opaque_field"] == "opaque_poll_value");
    REQUIRE(doc["results"]["results_opaque_field"] == 4242);
    REQUIRE(doc["results"]["answer_counts"][0]["count_opaque_field"] == "tag7");
    REQUIRE(doc["results"]["answer_counts"][0]["count"] == 3);
    REQUIRE(doc["results"]["answer_counts"][0]["me_voted"] == false);

    REQUIRE(applyPollVote(msg, makeVoteEvent(OtherUserId, 7), false, SelfUserId));
    REQUIRE(msg.poll->results->answerCounts->at(0).count.get() == 2);
    REQUIRE_FALSE(msg.poll->results->answerCounts->at(0).meVoted.get());

    doc = nlohmann::json::parse(msg.pollJson);
    REQUIRE(doc["results"]["answer_counts"][0]["count"] == 2);
    REQUIRE(doc["poll_opaque_field"] == "opaque_poll_value");
}

TEST_CASE("applyPollVote handles self-vote transitions and enforces idempotence", "[poll-votes]")
{
    Message msg = makeSampleMessage();

    REQUIRE(applyPollVote(msg, makeVoteEvent(SelfUserId, 7), true, SelfUserId));
    REQUIRE(msg.poll->results->answerCounts->at(0).count.get() == 3);
    REQUIRE(msg.poll->results->answerCounts->at(0).meVoted.get());

    nlohmann::json doc = nlohmann::json::parse(msg.pollJson);
    REQUIRE(doc["results"]["answer_counts"][0]["count"] == 3);
    REQUIRE(doc["results"]["answer_counts"][0]["me_voted"] == true);

    REQUIRE_FALSE(applyPollVote(msg, makeVoteEvent(SelfUserId, 7), true, SelfUserId));

    REQUIRE(applyPollVote(msg, makeVoteEvent(SelfUserId, 7), false, SelfUserId));
    REQUIRE(msg.poll->results->answerCounts->at(0).count.get() == 2);
    REQUIRE_FALSE(msg.poll->results->answerCounts->at(0).meVoted.get());

    REQUIRE_FALSE(applyPollVote(msg, makeVoteEvent(SelfUserId, 7), false, SelfUserId));
}

TEST_CASE("applyPollVote maintains independent self flags across multiple answers", "[poll-votes]")
{
    Message msg = makeSampleMessage();

    REQUIRE(applyPollVote(msg, makeVoteEvent(SelfUserId, 7), true, SelfUserId));
    REQUIRE(applyPollVote(msg, makeVoteEvent(SelfUserId, 42), true, SelfUserId));

    REQUIRE(msg.poll->results->answerCounts->at(0).id.get() == 7);
    REQUIRE(msg.poll->results->answerCounts->at(0).meVoted.get());
    REQUIRE(msg.poll->results->answerCounts->at(1).id.get() == 42);
    REQUIRE(msg.poll->results->answerCounts->at(1).meVoted.get());

    REQUIRE(applyPollVote(msg, makeVoteEvent(SelfUserId, 7), false, SelfUserId));
    REQUIRE_FALSE(msg.poll->results->answerCounts->at(0).meVoted.get());
    REQUIRE(msg.poll->results->answerCounts->at(1).meVoted.get());
}

TEST_CASE("applyPollVote appends entry for sparse declared answer without prior counts", "[poll-votes]")
{
    Message msg = makeSampleMessage();

    REQUIRE(applyPollVote(msg, makeVoteEvent(OtherUserId, 99), true, SelfUserId));
    REQUIRE(msg.poll->results->answerCounts->size() == 3);

    const auto &addedEntry = msg.poll->results->answerCounts->back();
    REQUIRE(addedEntry.id.get() == 99);
    REQUIRE(addedEntry.count.get() == 1);
    REQUIRE_FALSE(addedEntry.meVoted.get());

    const auto doc = nlohmann::json::parse(msg.pollJson);
    REQUIRE(doc["results"]["answer_counts"].size() == 3);
    REQUIRE(doc["results"]["answer_counts"][2]["id"] == 99);
    REQUIRE(doc["results"]["answer_counts"][2]["count"] == 1);
    REQUIRE(doc["results"]["answer_counts"][2]["me_voted"] == false);
}

TEST_CASE("applyPollVote rejects decrement at zero count", "[poll-votes]")
{
    auto pollJson = makeSamplePollJson();
    pollJson["results"]["answer_counts"][0]["count"] = 0;
    Message msg = makeSampleMessage(pollJson);

    REQUIRE_FALSE(applyPollVote(msg, makeVoteEvent(OtherUserId, 7), false, SelfUserId));
    REQUIRE_FALSE(applyPollVote(msg, makeVoteEvent(OtherUserId, 99), false, SelfUserId));
}

TEST_CASE("applyPollVote clamps count at int max without overflow", "[poll-votes]")
{
    auto pollJson = makeSamplePollJson();
    pollJson["results"]["answer_counts"][0]["count"] = std::numeric_limits<int>::max();
    Message msg = makeSampleMessage(pollJson);

    REQUIRE_FALSE(applyPollVote(msg, makeVoteEvent(OtherUserId, 7), true, SelfUserId));

    REQUIRE(applyPollVote(msg, makeVoteEvent(SelfUserId, 7), true, SelfUserId));
    REQUIRE(msg.poll->results->answerCounts->at(0).count.get() == std::numeric_limits<int>::max());
    REQUIRE(msg.poll->results->answerCounts->at(0).meVoted.get());
}

TEST_CASE("applyPollVote rejects mismatched channel or message id", "[poll-votes]")
{
    Message msg = makeSampleMessage();

    REQUIRE_FALSE(applyPollVote(msg, makeVoteEvent(OtherUserId, 7, Snowflake(999), MessageId), true, SelfUserId));
    REQUIRE_FALSE(applyPollVote(msg, makeVoteEvent(OtherUserId, 7, ChannelId, Snowflake(999)), true, SelfUserId));
}

TEST_CASE("applyPollVote rejects undeclared answer id", "[poll-votes]")
{
    Message msg = makeSampleMessage();
    REQUIRE_FALSE(applyPollVote(msg, makeVoteEvent(OtherUserId, 123), true, SelfUserId));
}

TEST_CASE("applyPollVote leaves unknown results unchanged", "[poll-votes]")
{
    auto pollJson = makeSamplePollJson();
    pollJson.erase("results");
    Message msg = makeSampleMessage(pollJson);

    REQUIRE_FALSE(applyPollVote(msg, makeVoteEvent(OtherUserId, 7), true, SelfUserId));
    REQUIRE_FALSE(msg.poll->results.hasValue());
}

TEST_CASE("applyPollVote ignores vote events for finalized polls", "[poll-votes]")
{
    auto pollJson = makeSamplePollJson();
    pollJson["results"]["is_finalized"] = true;
    Message msg = makeSampleMessage(pollJson);

    REQUIRE_FALSE(applyPollVote(msg, makeVoteEvent(OtherUserId, 7), true, SelfUserId));
    REQUIRE(msg.poll->results->isFinalized.get());
    REQUIRE(msg.poll->results->answerCounts->at(0).count.get() == 2);
}
