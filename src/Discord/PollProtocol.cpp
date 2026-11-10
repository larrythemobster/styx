#include "PollProtocol.hpp"
#include "ApiError.hpp"
#include "HttpClient.hpp"

#include <string>
#include <utility>

namespace Styx::Discord {

nlohmann::json pollVoteBody(const std::vector<int> &answerIds)
{
    nlohmann::json body = nlohmann::json::object();
    body["answer_ids"] = answerIds;
    return body;
}

Core::Result<void> pollVoteResult(const HttpResponse &response)
{
    if (!response.success) {
        auto error = ApiError::fromResponse(response);
        return Core::Result<void>::makeError(std::move(error.message), error.code);
    }
    if (response.statusCode != 204)
        return Core::Result<void>::makeError("Unexpected status code " + std::to_string(response.statusCode));
    return Core::Result<void>::makeOk();
}

Core::Result<Message> endPollResult(const HttpResponse &response,
                                   Core::Snowflake channelId, Core::Snowflake messageId)
{
    if (!response.success) {
        auto error = ApiError::fromResponse(response);
        return Core::Result<Message>::makeError(std::move(error.message), error.code);
    }
    if (response.statusCode != 200)
        return Core::Result<Message>::makeError("Unexpected status code " + std::to_string(response.statusCode));

    const auto obj = nlohmann::json::parse(response.body, nullptr, false);
    if (!obj.is_object())
        return Core::Result<Message>::makeError("Invalid poll message response");

    const auto idIt = obj.find("id");
    const auto channelIdIt = obj.find("channel_id");
    const auto pollIt = obj.find("poll");
    if (idIt == obj.end() || !idIt->is_string() ||
        channelIdIt == obj.end() || !channelIdIt->is_string() ||
        pollIt == obj.end() || !pollIt->is_object())
        return Core::Result<Message>::makeError("Invalid poll message response");

    // Sequence parsing is permissive; reject malformed poll containers before it runs.
    const auto answers = pollIt->find("answers");
    const auto question = pollIt->find("question");
    if (answers == pollIt->end() || !answers->is_array() ||
        question == pollIt->end() || !question->is_object())
        return Core::Result<Message>::makeError("Invalid poll message response");

    Message message;
    try {
        message = Message::fromJson(obj);
    } catch (const nlohmann::json::exception &) {
        return Core::Result<Message>::makeError("Invalid poll message response");
    }

    if (message.channelId.get() != channelId || message.id.get() != messageId)
        return Core::Result<Message>::makeError("Poll message response does not match the requested message");

    return Core::Result<Message>::makeOk(std::move(message));
}

} // namespace Styx::Discord
