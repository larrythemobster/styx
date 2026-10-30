#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace Styx::Platform {

// Conversions between UTF-8 (std::string) and UTF-16 (std::wstring on Windows)
std::wstring toUtf16(std::string_view utf8);
std::string fromUtf16(std::wstring_view utf16);

// Whitespace trimming (ASCII space, tab, carriage return, newline)
std::string_view trimLeft(std::string_view str);
std::string_view trimRight(std::string_view str);
std::string_view trim(std::string_view str);
std::string simplified(std::string_view str);
std::string truncateUtf8(std::string_view str, size_t codePoints);
std::string caseFold(std::string_view str);

// String splitting by char or substring delimiter
std::vector<std::string_view> split(std::string_view str, char delimiter, bool skipEmpty = false);
std::vector<std::string_view> split(std::string_view str, std::string_view delimiter, bool skipEmpty = false);

// ASCII case-folding
std::string toLowerAscii(std::string_view str);
std::string toUpperAscii(std::string_view str);
bool equalsIgnoreCaseAscii(std::string_view a, std::string_view b);

// Common prefix/suffix checks
bool startsWith(std::string_view str, std::string_view prefix);
bool endsWith(std::string_view str, std::string_view suffix);

// Substring replacement
std::string replace(std::string_view str, std::string_view search, std::string_view replacement);

} // namespace Styx::Platform
