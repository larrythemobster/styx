#include "QuickSwitcher.hpp"
#include "Platform/Native.hpp"
#include "Platform/Utf8.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Styx::Core {

namespace {

bool isWordSeparator(char c)
{
    return c == ' ' || c == '-' || c == '_' || c == '.';
}

int matchTier(std::string_view name, std::string_view query)
{
    if (query.empty())
        return 0;
    if (name == query)
        return 4;
    if (name.starts_with(query))
        return 3;

    std::size_t pos = name.find(query, 1);
    while (pos != std::string_view::npos) {
        if (isWordSeparator(name[pos - 1]))
            return 2;
        pos = name.find(query, pos + 1);
    }

    std::size_t q = 0;
    for (char c : name) {
        if (c == query[q]) {
            ++q;
            if (q == query.size())
                return 1;
        }
    }
    return 0;
}

bool matchesKind(SwitcherItem::Kind kind, char prefix)
{
    switch (prefix) {
    case '#':
        return kind == SwitcherItem::Kind::TextChannel;
    case '!':
        return kind == SwitcherItem::Kind::VoiceChannel;
    case '@':
        return kind == SwitcherItem::Kind::Dm || kind == SwitcherItem::Kind::Friend;
    case '*':
        return kind == SwitcherItem::Kind::Server;
    default:
        return true;
    }
}

template <typename T>
std::vector<std::size_t> takeIndices(const std::vector<T> &matches, std::size_t limit)
{
    const std::size_t count = std::min(matches.size(), limit);
    std::vector<std::size_t> result;
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        result.push_back(matches[i].index);
    return result;
}

} // namespace

std::vector<std::size_t> rankSwitcher(std::string_view query, const std::vector<SwitcherItem> &items,
                                      std::size_t limit)
{
    std::string_view trimmed = Platform::trim(query);
    char prefix = '\0';
    if (!trimmed.empty()) {
        const char first = trimmed.front();
        if (first == '#' || first == '!' || first == '@' || first == '*') {
            prefix = first;
            trimmed.remove_prefix(1);
            trimmed = Platform::trim(trimmed);
        }
    }

    if (trimmed.empty()) {
        struct EmptyMatch
        {
            std::size_t index;
            double frecency;
        };

        std::vector<EmptyMatch> matches;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (!matchesKind(items[i].kind, prefix))
                continue;
            if (items[i].frecency > 0)
                matches.push_back({i, items[i].frecency});
        }

        std::sort(matches.begin(), matches.end(), [](const EmptyMatch &a, const EmptyMatch &b) {
            if (a.frecency != b.frecency)
                return a.frecency > b.frecency;
            return a.index < b.index;
        });

        return takeIndices(matches, limit);
    }

    const std::string lowerQuery = Platform::unicodeLower(trimmed);

    struct Match
    {
        std::size_t index;
        int tier;
        double frecency;
        std::size_t nameLength;
    };

    std::vector<Match> matches;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!matchesKind(items[i].kind, prefix))
            continue;

        const std::string lowerName = Platform::unicodeLower(items[i].name);
        const int tier = matchTier(lowerName, lowerQuery);
        if (tier > 0)
            matches.push_back({i, tier, items[i].frecency, items[i].name.size()});
    }

    std::sort(matches.begin(), matches.end(), [](const Match &a, const Match &b) {
        if (a.tier != b.tier)
            return a.tier > b.tier;
        if (a.frecency != b.frecency)
            return a.frecency > b.frecency;
        if (a.nameLength != b.nameLength)
            return a.nameLength < b.nameLength;
        return a.index < b.index;
    });

    return takeIndices(matches, limit);
}

} // namespace Styx::Core
