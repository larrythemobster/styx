#include "Core/PollVotes.hpp"

#include <limits>
#include <nlohmann/json.hpp>

#include "Discord/Entities.hpp"
#include "Discord/Events.hpp"

namespace Styx::Core {

bool applyPollVote(Discord::Message &message, const Discord::MessagePollVote &event,
                   bool added, Snowflake selfId)
{
    if (event.messageId.get() != message.id.get() || event.channelId.get() != message.channelId.get()
        || !message.poll.hasValue() || !message.poll->answers.hasValue() || !message.poll->results.hasValue()
        || message.poll->results->isFinalized.valueOr(false) || message.pollJson.empty())
        return false;

    auto doc = nlohmann::json::parse(message.pollJson, nullptr, false);
    if (!doc.is_object())
        return false;

    auto resultsIt = doc.find("results");
    if (resultsIt == doc.end() || !resultsIt->is_object())
        return false;

    auto countsIt = resultsIt->find("answer_counts");
    if (countsIt == resultsIt->end() || !countsIt->is_array())
        return false;

    const int targetAnswerId = event.answerId.get();
    bool declaredAnswer = false;
    for (const auto &answer : message.poll->answers.get()) {
        if (answer.answerId.get() == targetAnswerId) {
            declaredAnswer = true;
            break;
        }
    }
    if (!declaredAnswer)
        return false;

    auto &answerCounts = *countsIt;
    nlohmann::json *matchingEntry = nullptr;
    for (auto &entry : answerCounts) {
        const auto id = entry.find("id");
        if (id != entry.end() && id->is_number_integer() && id->get<int>() == targetAnswerId) {
            matchingEntry = &entry;
            break;
        }
    }

    int currentCount = 0;
    bool currentMeVoted = false;
    if (matchingEntry) {
        const auto count = matchingEntry->find("count");
        const auto meVoted = matchingEntry->find("me_voted");
        if (count != matchingEntry->end() && count->is_number_integer())
            currentCount = count->get<int>();
        if (meVoted != matchingEntry->end() && meVoted->is_boolean())
            currentMeVoted = meVoted->get<bool>();
    }
    if (currentCount < 0)
        currentCount = 0;

    const bool isSelf = selfId.isValid() && event.userId.get() == selfId;
    if (isSelf && added == currentMeVoted)
        return false;

    const int nextCount = added
        ? (currentCount < std::numeric_limits<int>::max() ? currentCount + 1 : currentCount)
        : (currentCount > 0 ? currentCount - 1 : 0);
    const bool nextMeVoted = isSelf ? added : currentMeVoted;

    if (nextCount == currentCount && nextMeVoted == currentMeVoted)
        return false;

    if (matchingEntry) {
        (*matchingEntry)["count"] = nextCount;
        (*matchingEntry)["me_voted"] = nextMeVoted;
    } else {
        answerCounts.push_back(nlohmann::json{
            { "id", targetAnswerId },
            { "count", nextCount },
            { "me_voted", nextMeVoted }
        });
    }

    message.poll = Discord::Poll::fromJson(doc);
    message.pollJson = doc.dump();
    return true;
}

} // namespace Styx::Core
