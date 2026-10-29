#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Discord/Entities.hpp"

namespace Styx {
namespace Core {
namespace ActivityFormat {

// sorted by rank, then rich, then most recent
std::vector<Discord::Activity> sortAndFilter(const std::vector<Discord::Activity> &activities);

const Discord::Activity *primary(const std::vector<Discord::Activity> &sorted);
const Discord::Activity *custom(const std::vector<Discord::Activity> &activities);

std::vector<Discord::Activity> profileCardActivities(const std::vector<Discord::Activity> &sorted);

// bare status text
std::string secondaryText(const Discord::Activity &activity);

std::string cardHeading(const Discord::Activity &activity);

bool headingHasName(const Discord::Activity &activity);
std::string partyText(const Discord::Activity &activity);

struct TimerInfo
{
    enum class Kind {
        None,
        Elapsed,
        Countdown,
        Progress,
    };

    Kind kind = Kind::None;
    std::int64_t startMs = 0;
    std::int64_t endMs = 0;
};

TimerInfo timerFor(const Discord::Activity &activity);

std::string timerText(const TimerInfo &info, std::int64_t nowMs);
double progress(const TimerInfo &info, std::int64_t nowMs);

} // namespace ActivityFormat
} // namespace Core
} // namespace Styx
