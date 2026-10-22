#include "Utf8.hpp"

#include <algorithm>
#include <cctype>

namespace Styx::Platform {

static bool isWhitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string_view trimLeft(std::string_view str)
{
    while (!str.empty() && isWhitespace(str.front())) {
        str.remove_prefix(1);
    }
    return str;
}

std::string_view trimRight(std::string_view str)
{
    while (!str.empty() && isWhitespace(str.back())) {
        str.remove_suffix(1);
    }
    return str;
}

std::string_view trim(std::string_view str)
{
    return trimRight(trimLeft(str));
}

std::vector<std::string_view> split(std::string_view str, char delimiter, bool skipEmpty)
{
    std::vector<std::string_view> result;
    size_t start = 0;
    while (start <= str.size()) {
        size_t end = str.find(delimiter, start);
        if (end == std::string_view::npos) {
            std::string_view token = str.substr(start);
            if (!token.empty() || !skipEmpty) {
                result.push_back(token);
            }
            break;
        }
        std::string_view token = str.substr(start, end - start);
        if (!token.empty() || !skipEmpty) {
            result.push_back(token);
        }
        start = end + 1;
    }
    return result;
}

std::vector<std::string_view> split(std::string_view str, std::string_view delimiter, bool skipEmpty)
{
    if (delimiter.empty()) {
        if (!str.empty() || !skipEmpty) {
            return { str };
        }
        return {};
    }

    std::vector<std::string_view> result;
    size_t start = 0;
    while (start <= str.size()) {
        size_t end = str.find(delimiter, start);
        if (end == std::string_view::npos) {
            std::string_view token = str.substr(start);
            if (!token.empty() || !skipEmpty) {
                result.push_back(token);
            }
            break;
        }
        std::string_view token = str.substr(start, end - start);
        if (!token.empty() || !skipEmpty) {
            result.push_back(token);
        }
        start = end + delimiter.size();
    }
    return result;
}

std::string toLowerAscii(std::string_view str)
{
    std::string result(str);
    for (char &c : result) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    return result;
}

std::string toUpperAscii(std::string_view str)
{
    std::string result(str);
    for (char &c : result) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - ('a' - 'A'));
        }
    }
    return result;
}

bool equalsIgnoreCaseAscii(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + ('a' - 'A'));
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

bool startsWith(std::string_view str, std::string_view prefix)
{
    return str.starts_with(prefix);
}

bool endsWith(std::string_view str, std::string_view suffix)
{
    return str.ends_with(suffix);
}

std::string replace(std::string_view str, std::string_view search, std::string_view replacement)
{
    if (search.empty()) {
        return std::string(str);
    }
    std::string result;
    result.reserve(str.size());
    size_t start = 0;
    while (true) {
        size_t pos = str.find(search, start);
        if (pos == std::string_view::npos) {
            result.append(str.substr(start));
            break;
        }
        result.append(str.substr(start, pos - start));
        result.append(replacement);
        start = pos + search.size();
    }
    return result;
}

} // namespace Styx::Platform
