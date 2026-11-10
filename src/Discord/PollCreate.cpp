#include "PollCreate.hpp"
#include "Platform/Utf8.hpp"

#include <cstddef>
#include <limits>
#include <string_view>
#include <utility>
#include <utf8proc.h>

namespace Styx::Discord {

namespace {

Core::Result<void> checkText(std::string_view text, size_t maxCodePoints, const char *field)
{
    if (Platform::trim(text).empty())
        return Core::Result<void>::makeError(std::string(field) + " is required");

    size_t codePoints = 0;
    size_t offset = 0;
    while (offset < text.size()) {
        utf8proc_int32_t cp = 0;
        const auto length = utf8proc_iterate(
            reinterpret_cast<const utf8proc_uint8_t *>(text.data() + offset),
            static_cast<utf8proc_ssize_t>(text.size() - offset), &cp);
        if (length <= 0)
            return Core::Result<void>::makeError(std::string(field) + " contains invalid UTF-8");
        if (++codePoints > maxCodePoints)
            return Core::Result<void>::makeError(std::string(field) + " must be at most "
                + std::to_string(maxCodePoints) + " characters");
        offset += static_cast<size_t>(length);
    }
    return Core::Result<void>::makeOk();
}

} // namespace

Core::Result<nlohmann::json> pollCreateBody(const PollCreateRequest &request)
{
    if (request.durationHours < 1 || request.durationHours > 768)
        return Core::Result<nlohmann::json>::makeError("Poll duration must be between 1 and 768 hours");

    auto questionCheck = checkText(request.question, 300, "Question");
    if (!questionCheck.success())
        return Core::Result<nlohmann::json>::makeError(std::move(questionCheck.error));

    if (request.answers.size() < 2 || request.answers.size() > 10)
        return Core::Result<nlohmann::json>::makeError("Poll answers must have between 2 and 10 options");

    nlohmann::json answers = nlohmann::json::array();
    answers.get_ref<nlohmann::json::array_t &>().reserve(request.answers.size());
    for (const auto &answer : request.answers) {
        auto answerCheck = checkText(answer.text, 55, "Answer");
        if (!answerCheck.success())
            return Core::Result<nlohmann::json>::makeError(std::move(answerCheck.error));

        nlohmann::json pollMedia;
        pollMedia["text"] = answer.text;

        if (answer.emoji.has_value()) {
            const auto &emoji = *answer.emoji;
            if (emoji.id.isNull()) {
                auto emojiCheck = checkText(emoji.name.get(), std::numeric_limits<size_t>::max(), "Unicode emoji");
                if (!emojiCheck.success())
                    return Core::Result<nlohmann::json>::makeError(std::move(emojiCheck.error));
                pollMedia["emoji"]["name"] = emoji.name.get();
            } else {
                if (!emoji.id.hasValue() || !emoji.id.get().isValid() || emoji.id.get() == Core::Snowflake(0))
                    return Core::Result<nlohmann::json>::makeError("Custom emoji requires a valid non-zero ID");
                pollMedia["emoji"]["id"] = emoji.id.get().toString();
            }
        }

        nlohmann::json wireAnswer;
        wireAnswer["poll_media"] = std::move(pollMedia);
        answers.push_back(std::move(wireAnswer));
    }

    nlohmann::json poll;
    poll["question"]["text"] = request.question;
    poll["answers"] = std::move(answers);
    poll["duration"] = request.durationHours;
    poll["allow_multiselect"] = request.allowMultiselect;
    poll["layout_type"] = 1;

    return Core::Result<nlohmann::json>::makeOk(std::move(poll));
}

} // namespace Styx::Discord
