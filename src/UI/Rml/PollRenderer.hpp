#pragma once

#include "Platform/Native.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Styx::Discord { struct Poll; struct Embed; }

namespace Styx::UI::Rml {

struct PollRenderContext {
    // Turns a CDN URL into an image source for custom emoji; required.
    std::function<std::string(const std::string &, Platform::Size)> imageSource;
    // The time the poll's countdown is measured from.
    Platform::DateTime now;
    // Forwarded snapshots and previews leave this false.
    bool interactive = false;
    bool pending = false;
    // Null uses the confirmed votes; otherwise show this unsent selection.
    const std::vector<int> *selectedAnswers = nullptr;
    std::string_view error;
};

// "5d left", "3h left", "12m left" or "Less than a minute left" while the poll runs,
// rounding down; "Poll closed" once expiry is reached. Empty when there's no expiry.
[[nodiscard]] std::string pollTimeLeft(const Platform::DateTime &expiry, const Platform::DateTime &now);

[[nodiscard]] bool pollIsClosed(const Discord::Poll &poll, const Platform::DateTime &now);
// Confirmed votes in declared answer order, ignoring unknown answer IDs.
[[nodiscard]] std::vector<int> pollOwnVotes(const Discord::Poll &poll);
// Toggle one declared answer. Single-answer polls replace the previous selection.
// Returns false for an unknown answer ID, without changing the selection.
bool togglePollChoice(std::vector<int> &selected, const Discord::Poll &poll, int answerId);

// A poll as a `div.poll`:
// - `div.poll-question` (escaped text) and `div.poll-hint`: "Select one answer" or
//   "Select one or more answers".
// - one `div.poll-answer` per answer, in order, with data-answer-id. Inside: the emoji
//   (custom emoji as <img class="emoji">, others as rich text), `span.poll-answer-text`,
//   and when counts are known `div.poll-bar` holding `div.poll-bar-fill` with an inline
//   width in percent, plus `span.poll-answer-votes` "N votes · P%" ("1 vote", percent
//   rounded to a whole number, 0% when nobody voted). Answers you voted for get the
//   class `poll-answer-mine`; the answer(s) with the most votes in a closed poll get
//   `poll-answer-winner` (none if nobody voted).
// - `div.poll-footer`: "N votes" (or "Results not loaded" when the poll has no results)
//   and, separated by " · ", pollTimeLeft. A closed poll whose count isn't final adds
//   " · Final count pending".
// The same answer id counted twice in the results counts once (the first entry).
// With interactive=true on an open poll, answers are native buttons carrying
// data-action="poll-answer" and data-answer-id; selected answers get
// poll-answer-selected. Vote (poll-vote) requires a nonempty selection. Remove vote
// (poll-remove-vote) is offered when you have confirmed votes or results are unknown. Pending disables
// all controls and shows "Submitting vote..."; errors are escaped in poll-vote-error.
[[nodiscard]] std::string renderPoll(const Discord::Poll &poll, const PollRenderContext &context);

// The text of a POLL_RESULT notice from its "poll_result" embed fields, as RML that follows
// the poll author's name on the system message line: `'s poll <strong>question</strong> has closed.` followed by
// ` <span class="poll-result-winner">emoji text</span> won with N votes (P%).` or
// ` There was no winner.` when the victor fields are missing or total_votes is 0.
// Field values are strings on the wire; numbers that don't parse count as 0.
[[nodiscard]] std::string renderPollResultNotice(const Discord::Embed &embed, const PollRenderContext &context);

} // namespace Styx::UI::Rml
