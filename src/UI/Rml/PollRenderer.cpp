#include "UI/Rml/PollRenderer.hpp"

#include "Discord/Entities.hpp"
#include "Platform/Native.hpp"
#include "UI/Rml/ComponentRenderer.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Styx::UI::Rml {
namespace {

std::string formatVotes(int count)
{
    if (count == 1)
        return "1 vote";
    return std::format("{} votes", count);
}

template<typename T>
T parseInteger(std::string_view text)
{
    T value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size())
        return 0;
    return value;
}

std::optional<std::string_view> lookupField(const Discord::Embed &embed, std::string_view name)
{
    if (!embed.fields.hasValue())
        return std::nullopt;
    for (const auto &field : embed.fields.get()) {
        if (field.name.hasValue() && field.name.get() == name && field.value.hasValue())
            return field.value.get();
    }
    return std::nullopt;
}

// An answer's emoji and already-escaped text, with a space between when both are there.
std::string emojiAndText(const std::string &emojiHtml, const std::string &escapedText)
{
    if (emojiHtml.empty() || escapedText.empty())
        return emojiHtml + escapedText;
    return emojiHtml + " " + escapedText;
}

struct AnswerStats {
    int count = 0;
    bool meVoted = false;
};

} // namespace

std::string pollTimeLeft(const Platform::DateTime &expiry, const Platform::DateTime &now)
{
    if (!expiry.valid())
        return {};
    const int64_t seconds = now.secondsUntil(expiry);
    if (seconds <= 0)
        return "Poll closed";
    if (seconds >= 86400)
        return std::format("{}d left", seconds / 86400);
    if (seconds >= 3600)
        return std::format("{}h left", seconds / 3600);
    if (seconds >= 60)
        return std::format("{}m left", seconds / 60);
    return "Less than a minute left";
}

std::string renderPoll(const Discord::Poll &poll, const PollRenderContext &context)
{
    const bool hasExpiry = poll.expiry.hasValue() && poll.expiry->valid();
    const int64_t secondsLeft = hasExpiry ? context.now.secondsUntil(*poll.expiry) : 1;
    const bool finalized = poll.results.hasValue() && poll.results->isFinalized.valueOr(false);
    const bool closed = finalized || (hasExpiry && secondsLeft <= 0);

    // Answers missing from the counts have no votes; a repeated id keeps its first entry.
    std::unordered_map<int, AnswerStats> answerStats;
    if (poll.results.hasValue() && poll.results->answerCounts.hasValue()) {
        for (const auto &ac : poll.results->answerCounts.get())
            answerStats.emplace(ac.id.valueOr(0), AnswerStats{ac.count.valueOr(0), ac.meVoted.valueOr(false)});
    }
    const auto statsFor = [&answerStats](const Discord::PollAnswer &answer) {
        const auto it = answerStats.find(answer.answerId.valueOr(0));
        return it != answerStats.end() ? it->second : AnswerStats{};
    };

    int totalVotes = 0;
    int maxVotes = 0;
    if (poll.answers.hasValue()) {
        for (const auto &ans : poll.answers.get()) {
            const int count = statsFor(ans).count;
            totalVotes += count;
            maxVotes = std::max(maxVotes, count);
        }
    }

    std::string rml = "<div class=\"poll\">";

    const std::string question = (poll.question.hasValue() && poll.question->text.hasValue())
        ? poll.question->text.get()
        : std::string{};
    rml += std::format("<div class=\"poll-question\">{}</div>", Platform::htmlEscape(question));

    const bool multiselect = poll.allowMultiselect.valueOr(false);
    rml += std::format("<div class=\"poll-hint\">{}</div>",
                       multiselect ? "Select one or more answers" : "Select one answer");

    if (poll.answers.hasValue()) {
        for (const auto &ans : poll.answers.get()) {
            const int id = ans.answerId.valueOr(0);
            const auto [count, meVoted] = statsFor(ans);
            const bool isWinner = closed && maxVotes > 0 && count == maxVotes;

            std::string classes = "poll-answer";
            if (meVoted)
                classes += " poll-answer-mine";
            if (isWinner)
                classes += " poll-answer-winner";

            rml += std::format("<div class=\"{}\" data-answer-id=\"{}\">", classes, id);

            std::string emojiHtml;
            if (ans.pollMedia.hasValue() && ans.pollMedia->emoji.hasValue())
                emojiHtml = renderInlineEmoji(ans.pollMedia->emoji.get(), context.imageSource);
            const std::string text = ans.pollMedia.hasValue() && ans.pollMedia->text.hasValue()
                ? Platform::htmlEscape(ans.pollMedia->text.get())
                : std::string{};
            rml += emojiAndText(emojiHtml, std::format("<span class=\"poll-answer-text\">{}</span>", text));

            if (poll.results.hasValue()) {
                const int percent = totalVotes > 0
                    ? static_cast<int>(std::round(static_cast<double>(count) * 100.0 / totalVotes))
                    : 0;
                rml += std::format("<span class=\"poll-answer-votes\">{} · {}%</span>",
                                   formatVotes(count), percent);
                rml += std::format("<div class=\"poll-bar\"><div class=\"poll-bar-fill\" style=\"width: {}%\"></div></div>",
                                   percent);
            }

            rml += "</div>";
        }
    }

    std::string footer = poll.results.hasValue() ? formatVotes(totalVotes) : "Results not loaded";
    const std::string timeLeft = closed ? "Poll closed" : hasExpiry ? pollTimeLeft(*poll.expiry, context.now) : std::string{};
    if (!timeLeft.empty())
        footer += " · " + timeLeft;
    if (closed && !finalized)
        footer += " · Final count pending";

    rml += std::format("<div class=\"poll-footer\">{}</div>", footer);
    rml += "</div>";

    return rml;
}

std::string renderPollResultNotice(const Discord::Embed &embed, const PollRenderContext &context)
{
    const std::string prefix = std::format("'s poll <strong>{}</strong> has closed.",
                                           Platform::htmlEscape(lookupField(embed, "poll_question_text").value_or("")));

    const int totalVotes = parseInteger<int>(lookupField(embed, "total_votes").value_or(""));

    const auto victorIdStr = lookupField(embed, "victor_answer_id");
    const auto victorVotesStr = lookupField(embed, "victor_answer_votes");

    if (!victorIdStr || !victorVotesStr || totalVotes <= 0)
        return prefix + " There was no winner.";

    const int victorVotes = parseInteger<int>(*victorVotesStr);
    const int percent = static_cast<int>(std::round(static_cast<double>(victorVotes) * 100.0 / totalVotes));

    Discord::Emoji emoji;
    const auto emojiId = parseInteger<uint64_t>(lookupField(embed, "victor_answer_emoji_id").value_or(""));
    if (emojiId != 0)
        emoji.id = Core::Snowflake(emojiId);
    else
        emoji.id = nullptr;
    if (const auto emojiName = lookupField(embed, "victor_answer_emoji_name"))
        emoji.name = std::string(*emojiName);
    emoji.animated = lookupField(embed, "victor_answer_emoji_animated") == std::optional<std::string_view>("true");

    const std::string winnerContent = emojiAndText(renderInlineEmoji(emoji, context.imageSource),
                                                   Platform::htmlEscape(lookupField(embed, "victor_answer_text").value_or("")));

    return std::format("{} <span class=\"poll-result-winner\">{}</span> won with {} ({}%).",
                       prefix,
                       winnerContent,
                       formatVotes(victorVotes),
                       percent);
}

} // namespace Styx::UI::Rml
