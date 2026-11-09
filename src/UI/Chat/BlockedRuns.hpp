#pragma once

#include <vector>

namespace Styx {
namespace UI {

// A stretch of consecutive messages by blocked users, which the chat shows as one
// "N blocked messages" bar.
struct BlockedRun
{
    int first = 0;
    int count = 0;
};

// The run containing row, given which rows are by blocked users; count 0 when row isn't
// blocked (or is out of range).
[[nodiscard]] BlockedRun blockedRunAt(const std::vector<bool> &blocked, int row);

} // namespace UI
} // namespace Styx
