#include "SearchCompletion.hpp"
#include "Platform/Native.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Styx::Core {

namespace {

struct FilterDef
{
    std::string_view name;
    std::string_view detail;
    bool requiresChannels = false;
};

constexpr FilterDef FILTERS[] = {
    {"from:", "user", false},
    {"mentions:", "user", false},
    {"in:", "channel", true},
    {"has:", "link, embed, file, image…", false},
    {"pinned:", "true or false", false},
    {"before:", "date", false},
    {"after:", "date", false},
    {"during:", "date", false},
};

constexpr std::string_view HAS_VALUES[] = {
    "link", "embed", "poll", "file", "video", "image", "sound", "sticker", "forward",
};

constexpr std::string_view PINNED_VALUES[] = {
    "true", "false",
};

// Not called quoted(): with a std::string argument, lookup would find std::quoted instead.
std::string quoteIfSpaced(std::string_view text)
{
    const bool hasSpace = std::any_of(text.begin(), text.end(), [](char c) {
        return std::isspace(static_cast<unsigned char>(c));
    });
    if (hasSpace)
        return "\"" + std::string(text) + "\"";
    return std::string(text);
}

const std::string *findMatchingName(const SearchName &item, std::string_view lowerValue)
{
    if (item.names.empty())
        return nullptr;
    if (lowerValue.empty())
        return &item.names.front();
    for (const auto &name : item.names) {
        if (Platform::unicodeLower(name).starts_with(lowerValue))
            return &name;
    }
    return nullptr;
}

std::string formatOtherNames(const std::vector<std::string> &names)
{
    if (names.size() <= 1)
        return {};
    std::string detail;
    for (std::size_t i = 1; i < names.size(); ++i) {
        if (!detail.empty())
            detail += ", ";
        detail += names[i];
    }
    return detail;
}

SearchSuggestion buildValueSuggestion(std::string_view prefix, std::string_view key,
                                      std::string_view value, std::string label = {},
                                      std::string detail = {})
{
    if (label.empty())
        label = std::string(value);
    SearchSuggestion suggestion;
    suggestion.label = std::move(label);
    suggestion.detail = std::move(detail);
    suggestion.replacement = std::string(prefix) + std::string(key) + ":" + std::string(value) + " ";
    return suggestion;
}

void suggestChoices(std::vector<SearchSuggestion> &result, std::string_view prefix,
                    std::string_view key, std::string_view lowerValue,
                    std::span<const std::string_view> choices, std::size_t limit)
{
    for (std::string_view choice : choices) {
        if (result.size() >= limit)
            break;
        if (choice.starts_with(lowerValue))
            result.push_back(buildValueSuggestion(prefix, key, choice));
    }
}

} // namespace

std::vector<SearchSuggestion> searchSuggestions(std::string_view input, const std::vector<SearchName> &people,
                                              const std::vector<SearchName> &channels, std::size_t limit)
{
    if (limit == 0)
        return {};

    std::size_t lastSpace = std::string_view::npos;
    bool inQuotes = false;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '"') {
            inQuotes = !inQuotes;
        } else if (!inQuotes && std::isspace(static_cast<unsigned char>(input[i]))) {
            lastSpace = i;
        }
    }

    std::string_view prefix;
    std::string_view lastWord = input;
    if (lastSpace != std::string_view::npos) {
        prefix = input.substr(0, lastSpace + 1);
        lastWord = input.substr(lastSpace + 1);
    }

    std::vector<SearchSuggestion> result;
    const std::size_t colon = lastWord.find(':');
    if (colon == std::string_view::npos) {
        const auto lowerLastWord = Platform::unicodeLower(lastWord);
        for (const auto &filter : FILTERS) {
            if (result.size() >= limit)
                break;
            if (filter.requiresChannels && channels.empty())
                continue;
            if (filter.name.starts_with(lowerLastWord)) {
                SearchSuggestion suggestion;
                suggestion.label = std::string(filter.name);
                suggestion.detail = std::string(filter.detail);
                suggestion.replacement = std::string(prefix) + std::string(filter.name);
                result.push_back(std::move(suggestion));
            }
        }
        return result;
    }

    const std::string key = Platform::unicodeLower(lastWord.substr(0, colon));
    std::string_view val = lastWord.substr(colon + 1);
    if (!val.empty() && val.front() == '"')
        val.remove_prefix(1);
    const std::string lowerValue = Platform::unicodeLower(val);

    if (key == "from" || key == "mentions") {
        for (const auto &person : people) {
            if (result.size() >= limit)
                break;
            const auto *match = findMatchingName(person, lowerValue);
            if (match)
                result.push_back(buildValueSuggestion(prefix, key, quoteIfSpaced(*match), person.names.front(),
                                                      formatOtherNames(person.names)));
        }
    } else if (key == "in") {
        for (const auto &channel : channels) {
            if (result.size() >= limit)
                break;
            const auto *match = findMatchingName(channel, lowerValue);
            if (match)
                result.push_back(buildValueSuggestion(prefix, key, quoteIfSpaced(*match), "#" + channel.names.front()));
        }
    } else if (key == "has") {
        suggestChoices(result, prefix, key, lowerValue, HAS_VALUES, limit);
    } else if (key == "pinned") {
        suggestChoices(result, prefix, key, lowerValue, PINNED_VALUES, limit);
    }

    return result;
}

} // namespace Styx::Core
