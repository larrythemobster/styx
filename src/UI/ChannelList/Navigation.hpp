#pragma once

#include "Core/Snowflake.hpp"

#include <optional>
#include <vector>

namespace Styx {
namespace UI {

// A channel, DM or server in the order the sidebar or rail shows it.
struct NavigationEntry
{
    Core::Snowflake id;
    bool unread = false;
};

// The entry after (direction 1) or before (direction -1) current, wrapping around the ends.
// With unreadOnly, entries that aren't unread are skipped. When current isn't in the list,
// stepping forward starts from the first entry and back from the last. Nothing when no other
// entry qualifies.
[[nodiscard]] std::optional<Core::Snowflake> stepNavigation(const std::vector<NavigationEntry> &entries, Core::Snowflake current,
                                                            int direction, bool unreadOnly);

} // namespace UI
} // namespace Styx
