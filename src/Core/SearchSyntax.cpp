#include "SearchSyntax.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Styx::Core {

namespace {

constexpr std::int64_t MS_PER_DAY = 24LL * 60 * 60 * 1000;

std::string lowercase(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Days since 1970-01-01 for a proleptic Gregorian date.
std::int64_t daysFromCivil(int year, int month, int day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const int yearOfEra = year - era * 400;
    const int dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return static_cast<std::int64_t>(era) * 146097 + dayOfEra - 719468;
}

// Unix milliseconds at local midnight starting a YYYY-MM-DD date, or nothing if it isn't one.
std::optional<std::int64_t> localMidnight(std::string_view text, int utcOffsetMinutes)
{
    if (text.size() != 10 || text[4] != '-' || text[7] != '-')
        return std::nullopt;
    for (std::size_t i : std::array<std::size_t, 8>{0, 1, 2, 3, 5, 6, 8, 9})
        if (!std::isdigit(static_cast<unsigned char>(text[i])))
            return std::nullopt;
    const int year = std::stoi(std::string(text.substr(0, 4)));
    const int month = std::stoi(std::string(text.substr(5, 2)));
    const int day = std::stoi(std::string(text.substr(8, 2)));
    if (month < 1 || month > 12 || day < 1 || day > 31)
        return std::nullopt;
    return daysFromCivil(year, month, day) * MS_PER_DAY - static_cast<std::int64_t>(utcOffsetMinutes) * 60 * 1000;
}

// The has: values the search endpoint takes, with forward as the client's name for snapshot.
std::optional<std::string> hasValue(std::string_view value)
{
    static constexpr std::array<std::string_view, 9> ACCEPTED = {"link", "embed", "poll", "file", "video", "image", "sound", "sticker", "snapshot"};
    if (value == "forward")
        return std::string("snapshot");
    if (std::find(ACCEPTED.begin(), ACCEPTED.end(), value) != ACCEPTED.end())
        return std::string(value);
    return std::nullopt;
}

std::string stripPrefix(std::string value, char prefix)
{
    if (!value.empty() && value.front() == prefix)
        value.erase(0, 1);
    return value;
}

struct Token
{
    std::string raw;      // exactly as typed, kept when the filter isn't recognised
    std::string key;      // lowercased, empty for a plain word
    std::string value;    // unquoted
};

std::size_t nextSpace(std::string_view input, std::size_t from)
{
    return static_cast<std::size_t>(std::find_if(input.begin() + static_cast<std::ptrdiff_t>(from), input.end(), [](char c) {
        return std::isspace(static_cast<unsigned char>(c));
    }) - input.begin());
}

// Splits on whitespace, keeping a quoted value (key:"two words") as one token.
std::vector<Token> tokenize(std::string_view input)
{
    std::vector<Token> tokens;
    std::size_t i = 0;
    while (i < input.size()) {
        while (i < input.size() && std::isspace(static_cast<unsigned char>(input[i])))
            ++i;
        if (i >= input.size())
            break;
        const std::size_t start = i;
        Token token;
        const std::size_t colon = input.find(':', i);
        const std::size_t space = nextSpace(input, i);
        if (colon != std::string_view::npos && colon < space && colon > i) {
            token.key = lowercase(input.substr(i, colon - i));
            i = colon + 1;
            if (i < input.size() && input[i] == '"') {
                const std::size_t close = input.find('"', i + 1);
                const std::size_t end = close == std::string_view::npos ? input.size() : close;
                token.value = std::string(input.substr(i + 1, end - i - 1));
                i = close == std::string_view::npos ? input.size() : close + 1;
            } else {
                const std::size_t end = nextSpace(input, i);
                token.value = std::string(input.substr(i, end - i));
                i = end;
            }
        } else {
            i = space;
        }
        token.raw = std::string(input.substr(start, i - start));
        tokens.push_back(std::move(token));
    }
    return tokens;
}

} // namespace

SearchSyntax parseSearch(std::string_view input, int utcOffsetMinutes)
{
    SearchSyntax result;
    std::vector<std::string> words;
    for (auto &token : tokenize(input)) {
        const std::string &key = token.key;
        bool used = !token.value.empty();
        if (used && key == "from")
            result.from.push_back(stripPrefix(token.value, '@'));
        else if (used && key == "mentions")
            result.mentions.push_back(stripPrefix(token.value, '@'));
        else if (used && key == "in")
            result.in.push_back(stripPrefix(token.value, '#'));
        else if (used && key == "has") {
            const auto value = hasValue(lowercase(token.value));
            if (value)
                result.has.push_back(*value);
            used = value.has_value();
        } else if (used && key == "pinned") {
            const std::string value = lowercase(token.value);
            used = value == "true" || value == "false";
            if (used)
                result.pinned = value == "true";
        } else if (used && (key == "before" || key == "after" || key == "during")) {
            const auto midnight = localMidnight(token.value, utcOffsetMinutes);
            used = midnight.has_value();
            if (key == "before" && midnight)
                result.before = Snowflake::fromUnixMs(*midnight);
            // after: a day means from the end of that day.
            if (key == "after" && midnight)
                result.after = Snowflake::fromUnixMs(*midnight + MS_PER_DAY);
            if (key == "during" && midnight) {
                result.after = Snowflake::fromUnixMs(*midnight);
                result.before = Snowflake::fromUnixMs(*midnight + MS_PER_DAY);
            }
        } else {
            used = false;
        }
        if (!used)
            words.push_back(std::move(token.raw));
    }
    for (const auto &word : words) {
        if (!result.text.empty())
            result.text += ' ';
        result.text += word;
    }
    return result;
}

} // namespace Styx::Core
