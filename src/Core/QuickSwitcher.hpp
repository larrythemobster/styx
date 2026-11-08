#pragma once

#include "Snowflake.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Styx::Core {

struct SwitcherItem
{
    enum class Kind {
        Server,
        TextChannel,
        VoiceChannel,
        Dm,
        Friend
    };

    Kind kind = Kind::TextChannel;
    Snowflake id;
    // The server a channel belongs to; invalid otherwise.
    Snowflake guildId;
    std::string name;
    // Recent-use score; 0 when never used.
    double frecency = 0;
};

// Indices into items, best first. A leading # keeps only text channels, ! voice channels,
// @ DMs and friends, * servers; the rest of the query is matched against names ignoring case.
// Better matches come first (exact, prefix, start of a word, then letters in order), then
// higher frecency, then shorter names. An empty query (after any prefix) lists items with
// frecency above zero, highest first.
[[nodiscard]] std::vector<std::size_t> rankSwitcher(std::string_view query, const std::vector<SwitcherItem> &items,
                                                    std::size_t limit = 20);

} // namespace Styx::Core
