#pragma once

#include "Snowflake.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Styx::Core {

// A search box string split into plain text and filters, as the official client reads it.
struct SearchSyntax
{
    // The words that aren't filters, joined with single spaces.
    std::string text;
    // Names as typed, without a leading @ or #. Turning them into ids is up to the caller.
    std::vector<std::string> from;
    std::vector<std::string> mentions;
    std::vector<std::string> in;
    // Values Discord's search accepts (link, embed, poll, file, video, image, sound, sticker,
    // snapshot); "forward" is read as snapshot.
    std::vector<std::string> has;
    std::optional<bool> pinned;
    // Bounds from before:, after: and during: dates; invalid means none.
    Snowflake before;
    Snowflake after;
};

// Reads key:value filters with optional "quoted values". Keys are case-insensitive. A filter
// whose key or value isn't recognised stays in the text. Dates are YYYY-MM-DD in local time,
// given as minutes east of UTC.
[[nodiscard]] SearchSyntax parseSearch(std::string_view input, int utcOffsetMinutes);

} // namespace Styx::Core
