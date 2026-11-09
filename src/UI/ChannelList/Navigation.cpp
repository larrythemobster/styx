#include "Navigation.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

namespace Styx::UI {

std::optional<Core::Snowflake> stepNavigation(const std::vector<NavigationEntry> &entries, Core::Snowflake current,
                                              int direction, bool unreadOnly)
{
    if (entries.empty())
        return std::nullopt;

    const int dir = direction > 0 ? 1 : -1;
    // An invalid id is looked up like any other: the server list starts with Home under one.
    const auto it = std::find_if(entries.begin(), entries.end(), [current](const NavigationEntry &entry) {
        return entry.id == current;
    });

    auto advance = [dir, size = entries.size()](std::size_t index) {
        if (dir > 0)
            return index + 1 == size ? 0 : index + 1;
        return index == 0 ? size - 1 : index - 1;
    };

    std::size_t startIndex = 0;
    std::size_t stepCount = 0;

    if (it != entries.end()) {
        const auto foundIndex = static_cast<std::size_t>(it - entries.begin());
        startIndex = advance(foundIndex);
        stepCount = entries.size() - 1;
    } else {
        startIndex = dir > 0 ? 0 : entries.size() - 1;
        stepCount = entries.size();
    }

    std::size_t idx = startIndex;
    for (std::size_t i = 0; i < stepCount; ++i) {
        const auto &entry = entries[idx];
        if (!unreadOnly || entry.unread)
            return entry.id;

        idx = advance(idx);
    }

    return std::nullopt;
}

} // namespace Styx::UI
