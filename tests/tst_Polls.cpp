#include "Discord/Entities.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <string>

using namespace Styx::Discord;
using Styx::Core::Snowflake;

namespace {

static nlohmann::json makeSamplePollJson()
{
    return nlohmann::json::parse(R"({
        "question": {"text": "Aliens?"},
        "answers": [
            {"answer_id": 1, "poll_media": {"text": "Alien"}},
            {"answer_id": 2, "poll_media": {"text": "Alien 2", "emoji": {"id": null, "name": "👽"}}},
            {"answer_id": 3, "poll_media": {"text": "Alien 3", "emoji": {"id": "1120790948302033046", "name": "meowlien"}}}
        ],
        "expiry": "2024-05-02T10:00:02.039342+00:00",
        "allow_multiselect": true,
        "layout_type": 1,
        "results": {"answer_counts": [{"id": 1, "count": 1, "me_voted": false}], "is_finalized": false}
    })");
}

// A MESSAGE_UPDATE for message 100 carrying this poll.
static nlohmann::json makeUpdateJson(const nlohmann::json &poll)
{
    return { { "id", "100" }, { "channel_id", "200" }, { "poll", poll } };
}

} // namespace

TEST_CASE("Poll parses wire format and preserves raw json", "[polls]")
{
    const nlohmann::json pollObj = makeSamplePollJson();
    const nlohmann::json messageObj = {
        { "id", "100" },
        { "channel_id", "200" },
        { "poll", pollObj }
    };

    const Message message = Message::fromJson(messageObj);
    REQUIRE(message.poll.hasValue());
    REQUIRE(message.poll->question.hasValue());
    REQUIRE(message.poll->question->text.valueOr("") == "Aliens?");

    REQUIRE(message.poll->answers.hasValue());
    const auto &answers = message.poll->answers.get();
    REQUIRE(answers.size() == 3);

    REQUIRE(answers[0].answerId.get() == 1);
    REQUIRE(answers[0].pollMedia.hasValue());
    REQUIRE(answers[0].pollMedia->text.valueOr("") == "Alien");
    REQUIRE_FALSE(answers[0].pollMedia->emoji.hasValue());

    REQUIRE(answers[1].answerId.get() == 2);
    REQUIRE(answers[1].pollMedia.hasValue());
    REQUIRE(answers[1].pollMedia->text.valueOr("") == "Alien 2");
    REQUIRE(answers[1].pollMedia->emoji.hasValue());
    REQUIRE(answers[1].pollMedia->emoji->id.isNull());
    REQUIRE(answers[1].pollMedia->emoji->name.valueOr("") == "👽");

    REQUIRE(answers[2].answerId.get() == 3);
    REQUIRE(answers[2].pollMedia.hasValue());
    REQUIRE(answers[2].pollMedia->text.valueOr("") == "Alien 3");
    REQUIRE(answers[2].pollMedia->emoji.hasValue());
    REQUIRE(answers[2].pollMedia->emoji->id.get() == Snowflake(1120790948302033046ULL));
    REQUIRE(answers[2].pollMedia->emoji->name.valueOr("") == "meowlien");

    REQUIRE(message.poll->expiry.hasValue());
    REQUIRE(message.poll->allowMultiselect.hasValue());
    REQUIRE(message.poll->allowMultiselect.get());
    REQUIRE(message.poll->layoutType.hasValue());
    REQUIRE(message.poll->layoutType.get() == 1);

    REQUIRE(message.poll->results.hasValue());
    REQUIRE(message.poll->results->isFinalized.hasValue());
    REQUIRE_FALSE(message.poll->results->isFinalized.get());
    REQUIRE(message.poll->results->answerCounts.hasValue());
    REQUIRE(message.poll->results->answerCounts->size() == 1);

    const auto &countEntry = message.poll->results->answerCounts->front();
    REQUIRE(countEntry.id.get() == 1);
    REQUIRE(countEntry.count.get() == 1);
    REQUIRE(countEntry.meVoted.hasValue());
    REQUIRE_FALSE(countEntry.meVoted.get());

    REQUIRE(nlohmann::json::parse(message.pollJson) == pollObj);
}

TEST_CASE("Message without poll leaves poll field undefined and pollJson empty", "[polls]")
{
    const nlohmann::json messageObj = {
        { "id", "101" },
        { "channel_id", "200" },
        { "content", "Ordinary text message" }
    };

    const Message message = Message::fromJson(messageObj);
    REQUIRE_FALSE(message.poll.hasValue());
    REQUIRE(message.pollJson.empty());
}

TEST_CASE("Message applyUpdate merges poll results and updates questions", "[polls]")
{
    const nlohmann::json pollObj = makeSamplePollJson();
    const nlohmann::json messageObj = {
        { "id", "100" },
        { "channel_id", "200" },
        { "poll", pollObj }
    };
    const Message baseline = Message::fromJson(messageObj);
    // The same poll with a new question and answers, and no results.
    const nlohmann::json editedPoll = {
        { "question", { { "text", "Aliens exist?" } } },
        { "answers", nlohmann::json::array({
            { { "answer_id", 1 }, { "poll_media", { { "text", "Yes" } } } },
            { { "answer_id", 2 }, { "poll_media", { { "text", "No" } } } }
        }) },
        { "expiry", "2024-05-02T10:00:02.039342+00:00" },
        { "allow_multiselect", false },
        { "layout_type", 1 }
    };

    SECTION("Update with poll lacking results preserves previous results and updates question")
    {
        Message updated = baseline;
        const nlohmann::json updateJson = makeUpdateJson(editedPoll);

        updated.applyUpdate(Message::fromJson(updateJson));
        REQUIRE(updated.poll.hasValue());
        REQUIRE(updated.poll->question.hasValue());
        REQUIRE(updated.poll->question->text.valueOr("") == "Aliens exist?");
        REQUIRE(updated.poll->results.hasValue());
        REQUIRE_FALSE(updated.poll->results->isFinalized.get());
        REQUIRE(updated.poll->results->answerCounts.hasValue());
        REQUIRE(updated.poll->results->answerCounts->size() == 1);
        REQUIRE(updated.poll->results->answerCounts->front().id.get() == 1);
        REQUIRE(updated.poll->results->answerCounts->front().count.get() == 1);
        REQUIRE_FALSE(updated.poll->results->answerCounts->front().meVoted.get());

        const auto parsedJson = nlohmann::json::parse(updated.pollJson);
        REQUIRE(parsedJson["question"]["text"] == "Aliens exist?");
        REQUIRE(parsedJson.contains("results"));
        REQUIRE(parsedJson["results"] == pollObj["results"]);
    }

    SECTION("Update with results replaces previous results")
    {
        Message updated = baseline;
        nlohmann::json withResults = editedPoll;
        withResults["results"] = {
            { "is_finalized", true },
            { "answer_counts", nlohmann::json::array({
                { { "id", 1 }, { "count", 42 }, { "me_voted", true } },
                { { "id", 2 }, { "count", 7 }, { "me_voted", false } }
            }) }
        };
        const nlohmann::json updateJson = makeUpdateJson(withResults);

        updated.applyUpdate(Message::fromJson(updateJson));
        REQUIRE(updated.poll.hasValue());
        REQUIRE(updated.poll->results.hasValue());
        REQUIRE(updated.poll->results->isFinalized.get());
        REQUIRE(updated.poll->results->answerCounts.hasValue());
        REQUIRE(updated.poll->results->answerCounts->size() == 2);
        REQUIRE(updated.poll->results->answerCounts.get()[0].id.get() == 1);
        REQUIRE(updated.poll->results->answerCounts.get()[0].count.get() == 42);
        REQUIRE(updated.poll->results->answerCounts.get()[0].meVoted.get());
        REQUIRE(nlohmann::json::parse(updated.pollJson) == updateJson["poll"]);
    }

    SECTION("Update without poll key leaves poll unchanged")
    {
        Message updated = baseline;
        const nlohmann::json updateJson = {
            { "id", "100" },
            { "channel_id", "200" },
            { "content", "poll message edited" }
        };

        updated.applyUpdate(Message::fromJson(updateJson));
        REQUIRE(updated.content.get() == "poll message edited");
        REQUIRE(updated.poll.hasValue());
        REQUIRE(updated.poll->question.hasValue());
        REQUIRE(updated.poll->question->text.valueOr("") == "Aliens?");
        REQUIRE(nlohmann::json::parse(updated.pollJson) == pollObj);
    }
}
