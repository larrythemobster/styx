#ifndef _WIN32
#include "Utf8.hpp"

#include <cstdint>

namespace Styx::Platform {

std::wstring toUtf16(std::string_view utf8)
{
    std::wstring utf16;
    size_t i = 0;
    while (i < utf8.size()) {
        uint32_t cp = 0;
        uint8_t c = static_cast<uint8_t>(utf8[i++]);
        if (c < 0x80) {
            cp = c;
        } else if ((c >> 5) == 0x6) {
            if (i >= utf8.size()) break;
            cp = ((c & 0x1F) << 6) | (static_cast<uint8_t>(utf8[i++]) & 0x3F);
        } else if ((c >> 4) == 0xE) {
            if (i + 1 >= utf8.size()) break;
            cp = ((c & 0x0F) << 12) | ((static_cast<uint8_t>(utf8[i++]) & 0x3F) << 6);
            cp |= (static_cast<uint8_t>(utf8[i++]) & 0x3F);
        } else if ((c >> 3) == 0x1E) {
            if (i + 2 >= utf8.size()) break;
            cp = ((c & 0x07) << 18) | ((static_cast<uint8_t>(utf8[i++]) & 0x3F) << 12);
            cp |= ((static_cast<uint8_t>(utf8[i++]) & 0x3F) << 6);
            cp |= (static_cast<uint8_t>(utf8[i++]) & 0x3F);
        }

        if (cp < 0x10000) {
            utf16.push_back(static_cast<wchar_t>(cp));
        } else {
            cp -= 0x10000;
            utf16.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            utf16.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        }
    }
    return utf16;
}

std::string fromUtf16(std::wstring_view utf16)
{
    std::string utf8;
    for (size_t i = 0; i < utf16.size(); ++i) {
        uint32_t cp = static_cast<uint32_t>(utf16[i]);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < utf16.size()) {
            uint32_t next = static_cast<uint32_t>(utf16[i + 1]);
            if (next >= 0xDC00 && next <= 0xDFFF) {
                cp = 0x10000 + (((cp - 0xD800) << 10) | (next - 0xDC00));
                ++i;
            }
        }
        if (cp < 0x80) {
            utf8.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            utf8.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            utf8.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            utf8.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            utf8.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            utf8.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return utf8;
}

} // namespace Styx::Platform
#endif
