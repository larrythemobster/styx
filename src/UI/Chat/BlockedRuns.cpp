#include "BlockedRuns.hpp"

#include <cstddef>
#include <vector>

namespace Styx::UI {

BlockedRun blockedRunAt(const std::vector<bool> &blocked, int row)
{
    if (row < 0 || static_cast<std::size_t>(row) >= blocked.size() || !blocked[static_cast<std::size_t>(row)])
        return {row, 0};

    int first = row;
    while (first > 0 && blocked[static_cast<std::size_t>(first - 1)])
        --first;

    int end = row;
    const int rowCount = static_cast<int>(blocked.size());
    while (end + 1 < rowCount && blocked[static_cast<std::size_t>(end + 1)])
        ++end;

    return {first, end - first + 1};
}

} // namespace Styx::UI
