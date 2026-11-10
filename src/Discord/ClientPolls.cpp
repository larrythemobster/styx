#include "Client.hpp"
#include "PollProtocol.hpp"
#include "PollCreate.hpp"

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

void Client::createPoll(Snowflake channelId, const PollCreateRequest &poll, const std::string &nonce,
                        ResultCallback<Message> callback)
{
    auto body = pollCreateBody(poll);
    if (!body.success()) {
        if (callback)
            callback(Core::Result<Message>::makeError(std::move(body.error), body.code));
        return;
    }

    nlohmann::json payload;
    payload["content"] = "";
    payload["flags"] = 0;
    payload["mobile_network_type"] = "unknown";
    payload["nonce"] = nonce;
    payload["tts"] = false;
    payload["poll"] = std::move(*body.value);
    httpClient->post("/channels/" + channelId.toString() + "/messages", payload,
                     ContextProperties::location("chat_input"),
                     [channelId, callback = std::move(callback)](const HttpResponse &response) {
        if (callback)
            callback(createPollResult(response, channelId));
    });
}

} // namespace Styx::Discord
