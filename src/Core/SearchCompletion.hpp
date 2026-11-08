#pragma once

#include "SearchQuery.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Styx::Core {

struct SearchSuggestion
{
    // What the row shows, e.g. "from:" or a person's first name.
    std::string label;
    // Grey text beside it, e.g. "user" for a filter or a person's other names. May be empty.
    std::string detail;
    // The whole search box text after picking this suggestion.
    std::string replacement;
};

// Suggestions for the last word of a search box, as the official client offers them:
// filter names while typing a word, then people after from:/mentions:, channels after in:,
// and the accepted values after has: and pinned:. Picking a value adds a trailing space.
// in: is only offered when channels isn't empty (server searches).
[[nodiscard]] std::vector<SearchSuggestion> searchSuggestions(std::string_view input, const std::vector<SearchName> &people,
                                                              const std::vector<SearchName> &channels, std::size_t limit = 8);

} // namespace Styx::Core
