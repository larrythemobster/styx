#include "PollProtocol.hpp"
#include "ApiError.hpp"
#include "HttpClient.hpp"
#include "Core/JsonUtils.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
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

namespace {

bool isCanonicalNonzeroSnowflake(const nlohmann::json &val)
{
    if (!val.is_string())
        return false;
    const auto &str = val.get_ref<const std::string &>();
    const auto s = Core::JsonUtils::detail::snowflake(str);
    return s.isValid() && s != Core::Snowflake(0) && s.toString() == str;
}

Core::Result<Message> pollMessageResult(const HttpResponse &response,
                                       Core::Snowflake channelId,
                                       std::optional<Core::Snowflake> expectedMessageId)
{
    if (!response.success) {
        auto error = ApiError::fromResponse(response);
        return Core::Result<Message>::makeError(std::move(error.message), error.code);
    }
    if (response.statusCode != 200)
        return Core::Result<Message>::makeError("Unexpected status code " + std::to_string(response.statusCode));

    auto obj = nlohmann::json::parse(response.body, nullptr, false);
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

    if (!expectedMessageId.has_value()) {
        if (!isCanonicalNonzeroSnowflake(*idIt) || !isCanonicalNonzeroSnowflake(*channelIdIt))
            return Core::Result<Message>::makeError("Invalid poll message response");

        const auto authorIt = obj.find("author");
        if (authorIt == obj.end() || !authorIt->is_object())
            return Core::Result<Message>::makeError("Invalid poll message response");
        const auto authorIdIt = authorIt->find("id");
        if (authorIdIt == authorIt->end() || !isCanonicalNonzeroSnowflake(*authorIdIt))
            return Core::Result<Message>::makeError("Invalid poll message response");

        const auto contentIt = obj.find("content");
        if (contentIt == obj.end() || !contentIt->is_string())
            return Core::Result<Message>::makeError("Invalid poll message response");

        const auto timestampIt = obj.find("timestamp");
        if (timestampIt == obj.end() || !timestampIt->is_string())
            return Core::Result<Message>::makeError("Invalid poll message response");

        const auto typeIt = obj.find("type");
        const auto flagsIt = obj.find("flags");
        if (typeIt == obj.end() || !typeIt->is_number_integer() ||
            flagsIt == obj.end() || !flagsIt->is_number_integer())
            return Core::Result<Message>::makeError("Invalid poll message response");

        const auto qTextIt = question->find("text");
        if (qTextIt == question->end() || !qTextIt->is_string())
            return Core::Result<Message>::makeError("Invalid poll message response");

        if (answers->size() < 2 || answers->size() > 10)
            return Core::Result<Message>::makeError("Invalid poll message response");

        std::array<int, 10> seenAnswerIds{};
        size_t seenCount = 0;
        for (const auto &ans : *answers) {
            if (!ans.is_object())
                return Core::Result<Message>::makeError("Invalid poll message response");

            const auto ansIdIt = ans.find("answer_id");
            if (ansIdIt == ans.end() || !ansIdIt->is_number_integer())
                return Core::Result<Message>::makeError("Invalid poll message response");
            const auto ansIdVal = ansIdIt->get<int64_t>();
            if (ansIdVal <= 0 || ansIdVal > std::numeric_limits<int>::max())
                return Core::Result<Message>::makeError("Invalid poll message response");
            const int ansId = static_cast<int>(ansIdVal);

            for (size_t i = 0; i < seenCount; ++i) {
                if (seenAnswerIds[i] == ansId)
                    return Core::Result<Message>::makeError("Invalid poll message response");
            }
            seenAnswerIds[seenCount++] = ansId;

            const auto pmIt = ans.find("poll_media");
            if (pmIt == ans.end() || !pmIt->is_object())
                return Core::Result<Message>::makeError("Invalid poll message response");
            const auto pmTextIt = pmIt->find("text");
            if (pmTextIt == pmIt->end() || !pmTextIt->is_string())
                return Core::Result<Message>::makeError("Invalid poll message response");

            const auto emojiIt = pmIt->find("emoji");
            if (emojiIt != pmIt->end() && !emojiIt->is_object())
                return Core::Result<Message>::makeError("Invalid poll message response");
        }

        const auto expiryIt = pollIt->find("expiry");
        if (expiryIt == pollIt->end() || (!expiryIt->is_null() && !expiryIt->is_string()))
            return Core::Result<Message>::makeError("Invalid poll message response");

        const auto msIt = pollIt->find("allow_multiselect");
        if (msIt == pollIt->end() || !msIt->is_boolean())
            return Core::Result<Message>::makeError("Invalid poll message response");

        const auto ltIt = pollIt->find("layout_type");
        if (ltIt == pollIt->end() || !ltIt->is_number_integer())
            return Core::Result<Message>::makeError("Invalid poll message response");

        const auto resultsIt = pollIt->find("results");
        if (resultsIt != pollIt->end()) {
            if (!resultsIt->is_object())
                return Core::Result<Message>::makeError("Invalid poll message response");

            const auto finalizedIt = resultsIt->find("is_finalized");
            if (finalizedIt == resultsIt->end() || !finalizedIt->is_boolean())
                return Core::Result<Message>::makeError("Invalid poll message response");

            const auto countsIt = resultsIt->find("answer_counts");
            if (countsIt == resultsIt->end() || !countsIt->is_array())
                return Core::Result<Message>::makeError("Invalid poll message response");

            for (const auto &ac : *countsIt) {
                if (!ac.is_object())
                    return Core::Result<Message>::makeError("Invalid poll message response");

                const auto countIdIt = ac.find("id");
                if (countIdIt == ac.end() || !countIdIt->is_number_integer())
                    return Core::Result<Message>::makeError("Invalid poll message response");
                const auto countIdVal = countIdIt->get<int64_t>();
                if (countIdVal <= 0 || countIdVal > std::numeric_limits<int>::max())
                    return Core::Result<Message>::makeError("Invalid poll message response");

                const auto countValIt = ac.find("count");
                if (countValIt == ac.end() || !countValIt->is_number_integer())
                    return Core::Result<Message>::makeError("Invalid poll message response");
                const auto countVal = countValIt->get<int64_t>();
                if (countVal < 0 || countVal > std::numeric_limits<int>::max())
                    return Core::Result<Message>::makeError("Invalid poll message response");

                const auto meVotedIt = ac.find("me_voted");
                if (meVotedIt == ac.end() || !meVotedIt->is_boolean())
                    return Core::Result<Message>::makeError("Invalid poll message response");
            }
        }

        const auto nonceIt = obj.find("nonce");
        if (nonceIt != obj.end() && nonceIt->is_number_integer())
            *nonceIt = nonceIt->dump();
    }

    Message message;
    try {
        message = Message::fromJson(obj);
    } catch (const nlohmann::json::exception &) {
        return Core::Result<Message>::makeError("Invalid poll message response");
    }

    if (!expectedMessageId && (!message.timestamp.get().valid()
        || (message.poll->expiry.hasValue() && !message.poll->expiry->valid())))
        return Core::Result<Message>::makeError("Invalid poll message response");

    if (message.channelId.get() != channelId ||
        (expectedMessageId.has_value() && message.id.get() != *expectedMessageId))
        return Core::Result<Message>::makeError("Poll message response does not match the requested message");

    return Core::Result<Message>::makeOk(std::move(message));
}

} // namespace

Core::Result<Message> endPollResult(const HttpResponse &response,
                                   Core::Snowflake channelId, Core::Snowflake messageId)
{
    return pollMessageResult(response, channelId, messageId);
}

Core::Result<Message> createPollResult(const HttpResponse &response,
                                      Core::Snowflake channelId)
{
    return pollMessageResult(response, channelId, std::nullopt);
}

} // namespace Styx::Discord
