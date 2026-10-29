#include "ActivityFormat.hpp"

#include <algorithm>
#include <format>

namespace Styx {
namespace Core {
namespace ActivityFormat {

using Discord::Activity;
using Discord::ActivityType;

namespace {

bool sameCard(const Activity &a, const Activity &b)
{
    if (a.applicationId.hasValue() && b.applicationId.hasValue() && a.applicationId.get() == b.applicationId.get())
        return true;
    return a.name.hasValue() && b.name.hasValue() && a.name.get() == b.name.get();
}

int sortRank(const Activity &activity)
{
    switch (activity.kind()) {
    case ActivityType::CUSTOM:
        return 4;
    case ActivityType::COMPETING:
        return 3;
    case ActivityType::STREAMING:
        return 2;
    case ActivityType::PLAYING:
        return 1;
    default:
        return 0;
    }
}

bool isRich(const Activity &activity)
{
    if (activity.isCustom())
        return false;

    if (!activity.detailsText().empty() || !activity.stateText().empty())
        return true;
    if (activity.party.hasValue() && activity.party->hasSize())
        return true;
    if (!activity.assets.hasValue())
        return false;

    // the client does this apparently
    return activity.assets->largeImage.hasValue() || activity.assets->smallText.hasValue();
}

} // namespace

std::vector<Activity> sortAndFilter(const std::vector<Activity> &activities)
{
    std::vector<Activity> sorted = activities;

    std::stable_sort(sorted.begin(), sorted.end(), [](const Activity &a, const Activity &b) {
        const int rankA = sortRank(a);
        const int rankB = sortRank(b);
        if (rankA != rankB)
            return rankA > rankB;

        const bool richA = isRich(a);
        const bool richB = isRich(b);
        if (richA != richB)
            return richA;

        return a.createdAtMs() > b.createdAtMs();
    });

    std::vector<Activity> filtered;
    filtered.reserve(sorted.size());
    bool seenPlaying = false;
    for (const Activity &activity : sorted) {
        if (activity.kind() == ActivityType::PLAYING) {
            if (seenPlaying)
                continue;
            seenPlaying = true;
        }
        filtered.push_back(activity);
    }

    return filtered;
}

const Activity *primary(const std::vector<Activity> &sorted)
{
    for (const Activity &activity : sorted) {
        if (activity.kind() == ActivityType::HANG)
            continue;
        return &activity;
    }
    return nullptr;
}

const Activity *custom(const std::vector<Activity> &activities)
{
    for (const Activity &activity : activities) {
        if (activity.isCustom())
            return &activity;
    }
    return nullptr;
}

std::vector<Activity> profileCardActivities(const std::vector<Activity> &sorted)
{
    std::vector<Activity> cards;
    for (const Activity &activity : sorted) {
        if (activity.isCustom() || activity.kind() == ActivityType::HANG)
            continue;

        const bool alreadyShown = std::any_of(cards.cbegin(), cards.cend(), [&activity](const Activity &shown) { return sameCard(shown, activity); });
        if (!alreadyShown)
            cards.push_back(activity);
    }
    return cards;
}

std::string secondaryText(const Activity &activity)
{
    if (activity.isCustom())
        return activity.stateText();

    const std::string name = activity.nameText();
    const std::string details = activity.detailsText();
    const std::string state = activity.stateText();

    std::string text = activity.kind() == ActivityType::STREAMING && !details.empty() ? details : name;

    if (activity.statusDisplayType.hasValue()) {
        switch (activity.statusDisplayType.get()) {
        case Discord::StatusDisplayType::NAME:
            if (!name.empty())
                text = name;
            break;
        case Discord::StatusDisplayType::STATE:
            if (!state.empty())
                text = state;
            break;
        case Discord::StatusDisplayType::DETAILS:
            if (!details.empty())
                text = details;
            break;
        }
    }

    return text;
}

std::string cardHeading(const Activity &activity)
{
    const std::string name = activity.nameText();

    switch (activity.kind()) {
    case ActivityType::STREAMING:
        return "Streaming";
    case ActivityType::LISTENING:
        return std::format("Listening to {}", name);
    case ActivityType::WATCHING:
        return std::format("Watching {}", name);
    case ActivityType::COMPETING:
        return std::format("Competing in {}", name);
    default:
        return "Playing a game";
    }
}

bool headingHasName(const Activity &activity)
{
    switch (activity.kind()) {
    case ActivityType::LISTENING:
    case ActivityType::WATCHING:
    case ActivityType::COMPETING:
        return true;
    default:
        return false;
    }
}

std::string partyText(const Activity &activity)
{
    const std::string state = activity.stateText();
    if (!activity.party.hasValue() || !activity.party->hasSize())
        return state;

    const Discord::ActivityParty &party = activity.party.get();
    const std::string count = party.maxSize > 0
            ? std::format("{} of {}", party.currentSize, party.maxSize)
            : std::to_string(party.currentSize);

    if (state.empty())
        return std::format("({})", count);
    return std::format("{} ({})", state, count);
}

TimerInfo timerFor(const Activity &activity)
{
    TimerInfo info;

    if (activity.isCustom() || activity.kind() == ActivityType::HANG)
        return info;

    const std::int64_t start = activity.startMs();
    if (start <= 0)
        return info;

    info.startMs = start;
    info.endMs = activity.endMs();

    if (info.endMs <= 0)
        info.kind = TimerInfo::Kind::Elapsed;
    else if (activity.kind() == ActivityType::LISTENING)
        info.kind = TimerInfo::Kind::Progress;
    else
        info.kind = TimerInfo::Kind::Countdown;

    return info;
}

namespace {

std::string formatDuration(std::int64_t ms)
{
    if (ms < 0)
        ms = 0;

    const std::int64_t total = ms / 1000;
    const std::int64_t hours = total / 3600;
    const std::int64_t minutes = (total % 3600) / 60;
    const std::int64_t seconds = total % 60;

    if (hours > 0)
        return std::format("{}:{:02}:{:02}", hours, minutes, seconds);

    return std::format("{}:{:02}", minutes, seconds);
}

} // namespace

std::string timerText(const TimerInfo &info, std::int64_t nowMs)
{
    switch (info.kind) {
    case TimerInfo::Kind::Elapsed:
        return std::format("{} elapsed", formatDuration(nowMs - info.startMs));
    case TimerInfo::Kind::Countdown:
        if (nowMs < info.endMs)
            return std::format("{} left", formatDuration(info.endMs - nowMs));
        return std::format("{} elapsed", formatDuration(nowMs - info.startMs));
    case TimerInfo::Kind::Progress: {
        const std::int64_t elapsed = std::min(info.endMs, nowMs) - info.startMs;
        return std::format("{} of {}", formatDuration(elapsed), formatDuration(info.endMs - info.startMs));
    }
    case TimerInfo::Kind::None:
        break;
    }

    return {};
}

double progress(const TimerInfo &info, std::int64_t nowMs)
{
    const std::int64_t span = info.endMs - info.startMs;
    if (span <= 0)
        return 0.0;

    const std::int64_t elapsed = std::clamp<std::int64_t>(nowMs - info.startMs, 0, span);
    return static_cast<double>(elapsed) / static_cast<double>(span);
}

} // namespace ActivityFormat
} // namespace Core
} // namespace Styx
