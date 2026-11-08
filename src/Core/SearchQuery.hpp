#pragma once

#include "SearchSyntax.hpp"
#include "Snowflake.hpp"
#include "Discord/MessageSearch.hpp"

#include <string>
#include <vector>

namespace Styx::Core {

// Something a search filter can name: a person (nickname, display name, username) or a channel.
struct SearchName
{
    Snowflake id;
    std::vector<std::string> names;
};

struct ResolvedSearch
{
    Discord::MessageSearchQuery query;
    // Names from from:, mentions: or in: that matched nobody, as typed.
    std::vector<std::string> unknownNames;
};

// Fills a search query from parsed syntax. Names match case-insensitively against any of a
// candidate's names; the first candidate that matches wins.
[[nodiscard]] ResolvedSearch resolveSearch(const SearchSyntax &syntax, const std::vector<SearchName> &people,
                                           const std::vector<SearchName> &channels);

} // namespace Styx::Core
