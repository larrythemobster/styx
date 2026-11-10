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
    const int64_t milliseconds = now.millisUntil(expiry);
    if (milliseconds <= 0)
        return "Poll closed";
    const int64_t seconds = milliseconds / 1000;
    if (seconds >= 86400)
        return std::format("{}d left", seconds / 86400);
    if (seconds >= 3600)
        return std::format("{}h left", seconds / 3600);
    if (seconds >= 60)
        return std::format("{}m left", seconds / 60);
    return "Less than a minute left";
}

bool pollIsClosed(const Discord::Poll &poll, const Platform::DateTime &now)
{
    const bool finalized = poll.results.hasValue() && poll.results->isFinalized.valueOr(false);
    const bool expired = poll.expiry.hasValue() && poll.expiry->valid() && now.valid() && *poll.expiry <= now;
    return finalized || expired;
}

bool pollCanVote(const Discord::Message &message, Core::Snowflake selfId, const Platform::DateTime &now)
{
    return selfId.isValid() && !message.isForwarded() && message.poll.hasValue()
        && !pollIsClosed(message.poll.get(), now);
}

bool pollCanEnd(const Discord::Message &message, Core::Snowflake selfId, const Platform::DateTime &now)
{
    return message.author->id.get() == selfId && pollCanVote(message, selfId, now);
}

std::vector<int> pollOwnVotes(const Discord::Poll &poll)
{
    if (!poll.answers.hasValue() || !poll.results.hasValue() || !poll.results->answerCounts.hasValue())
        return {};

    const auto &counts = poll.results->answerCounts.get();
    std::vector<int> votes;
    for (const auto &ans : poll.answers.get()) {
        const int id = ans.answerId.valueOr(0);
        const auto count = std::find_if(counts.begin(), counts.end(), [id](const auto &entry) { return entry.id.get() == id; });
        if (count != counts.end() && count->meVoted.get())
            votes.push_back(id);
    }
    return votes;
}

bool togglePollChoice(std::vector<int> &selected, const Discord::Poll &poll, int answerId)
{
    if (!poll.answers.hasValue())
        return false;

    const auto &answers = poll.answers.get();
    const bool isDeclared = std::any_of(answers.begin(), answers.end(), [answerId](const Discord::PollAnswer &ans) {
        return ans.answerId.valueOr(0) == answerId;
    });
    if (!isDeclared)
        return false;

    if (std::erase(selected, answerId) > 0)
        return true;

    if (poll.allowMultiselect.valueOr(false))
        selected.push_back(answerId);
    else
        selected = { answerId };
    return true;
}

std::string renderPoll(const Discord::Poll &poll, const PollRenderContext &context)
{
    const bool closed = pollIsClosed(poll, context.now);
    const bool hasExpiry = poll.expiry.hasValue() && poll.expiry->valid();
    const bool finalized = poll.results.hasValue() && poll.results->isFinalized.valueOr(false);
    const bool interactive = context.interactive && !closed;
    const auto confirmed = interactive ? pollOwnVotes(poll) : std::vector<int>{};
    const std::vector<int> &selection = context.selectedAnswers ? *context.selectedAnswers : confirmed;

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
            const bool isSelected = interactive && std::find(selection.begin(), selection.end(), id) != selection.end();

            std::string classes = "poll-answer";
            if (isSelected)
                classes += " poll-answer-selected";
            if (meVoted)
                classes += " poll-answer-mine";
            if (isWinner)
                classes += " poll-answer-winner";

            if (interactive) {
                rml += std::format("<button type=\"button\" class=\"{}\" data-action=\"poll-answer\" data-answer-id=\"{}\"{}>",
                                   classes, id, context.pending ? " disabled" : "");
            } else {
                rml += std::format("<div class=\"{}\" data-answer-id=\"{}\">", classes, id);
            }

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

            rml += interactive ? "</button>" : "</div>";
        }
    }

    if (interactive) {
        rml += "<div class=\"poll-vote-actions\">";

        const bool canVote = !selection.empty() && !context.pending;
        rml += std::format("<button class=\"poll-vote-submit\" data-action=\"poll-vote\"{}>Vote</button>",
                           canVote ? "" : " disabled");

        const bool showRemove = !confirmed.empty() || !poll.results.hasValue();
        if (showRemove) {
            const bool canRemove = !context.pending;
            rml += std::format("<button class=\"poll-vote-remove\" data-action=\"poll-remove-vote\"{}>Remove vote</button>",
                               canRemove ? "" : " disabled");
        }

        if (context.canEnd)
            rml += std::format("<button class=\"poll-end\" data-action=\"poll-end\"{}>End poll</button>",
                               context.pending ? " disabled" : "");

        rml += "</div>";
    }

    if (context.pending) {
        rml += "<div class=\"poll-vote-status\">";
        rml += context.ending ? "Ending poll..." : "Submitting vote...";
        rml += "</div>";
    }
    if (!context.error.empty())
        rml += std::format("<div class=\"poll-vote-error\">{}</div>", Platform::htmlEscape(context.error));

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
