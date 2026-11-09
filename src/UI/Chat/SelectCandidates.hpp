#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Styx::UI {

// Something a user, role, mentionable or channel select can offer. The value is the
// snowflake sent to the bot; kind is "user", "role" or "channel", as in default_values.
struct SelectCandidate
{
    std::string value;
    std::string label;
    std::string kind;
};

// Candidates whose label contains the query, ignoring case: labels starting with it first,
// then the rest, each group in the original order, at most `limit` of them. An empty query
// keeps the first `limit` candidates.
[[nodiscard]] std::vector<SelectCandidate> filterSelectCandidates(const std::vector<SelectCandidate> &candidates,
                                                                  std::string_view query, std::size_t limit);

} // namespace Styx::UI
