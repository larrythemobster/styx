#include "Client.hpp"
#include "PollProtocol.hpp"

#include <utility>

namespace Styx::Discord {
namespace {

std::string pollPath(Snowflake channelId, Snowflake messageId)
{
    return "/channels/" + channelId.toString() + "/polls/" + messageId.toString();
}

} // namespace

void Client::submitPollVote(Snowflake channelId, Snowflake messageId, const std::vector<int> &answerIds, ActionCallback callback)
{
    const auto prefix = pollPath(channelId, messageId);
    httpClient->put(prefix + "/answers/@me", pollVoteBody(answerIds),
                    [callback = std::move(callback)](const HttpResponse &response) {
        if (!callback)
            return;
        callback(pollVoteResult(response));
    });
}

void Client::endPoll(Snowflake channelId, Snowflake messageId, ResultCallback<Message> callback)
{
    const auto prefix = pollPath(channelId, messageId);
    httpClient->post(prefix + "/expire", nlohmann::json::object(),
                     [channelId, messageId, callback = std::move(callback)](const HttpResponse &response) {
        if (!callback)
            return;
        callback(endPollResult(response, channelId, messageId));
    });
}

} // namespace Styx::Discord
