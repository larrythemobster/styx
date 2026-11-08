#include "SearchQuery.hpp"
#include "Platform/Native.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace Styx::Core {

namespace {

void resolveNames(const std::vector<std::string> &typed, const std::vector<SearchName> &candidates,
                  std::vector<Snowflake> &ids, std::vector<std::string> &unknown)
{
    for (const auto &name : typed) {
        const auto lower = Platform::unicodeLower(name);
        const auto it = std::find_if(candidates.begin(), candidates.end(), [&](const SearchName &candidate) {
            return std::any_of(candidate.names.begin(), candidate.names.end(), [&](const std::string &candidateName) {
                return Platform::unicodeLower(candidateName) == lower;
            });
        });

        if (it != candidates.end()) {
            if (std::find(ids.begin(), ids.end(), it->id) == ids.end())
                ids.push_back(it->id);
        } else {
            unknown.push_back(name);
        }
    }
}

} // namespace

ResolvedSearch resolveSearch(const SearchSyntax &syntax, const std::vector<SearchName> &people,
                             const std::vector<SearchName> &channels)
{
    ResolvedSearch result;
    result.query.content = syntax.text;
    result.query.has = syntax.has;
    result.query.pinned = syntax.pinned;
    result.query.before = syntax.before;
    result.query.after = syntax.after;

    resolveNames(syntax.from, people, result.query.authorIds, result.unknownNames);
    resolveNames(syntax.mentions, people, result.query.mentions, result.unknownNames);
    resolveNames(syntax.in, channels, result.query.channelIds, result.unknownNames);

    return result;
}

} // namespace Styx::Core
