#include "Base64.hpp"

#include <array>

namespace Styx::Platform {

namespace {

constexpr std::string_view kBase64Chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

constexpr std::array<int8_t, 256> buildDecodeTable()
{
    std::array<int8_t, 256> table{};
    for (size_t i = 0; i < 256; ++i) {
        table[i] = -1;
    }
    for (size_t i = 0; i < kBase64Chars.size(); ++i) {
        table[static_cast<uint8_t>(kBase64Chars[i])] = static_cast<int8_t>(i);
    }
    return table;
}

constexpr auto kDecodeTable = buildDecodeTable();

} // namespace

std::string toBase64(std::span<const uint8_t> data)
{
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    size_t i = 0;
    while (i + 2 < data.size()) {
        uint32_t triple = (static_cast<uint32_t>(data[i]) << 16) |
                          (static_cast<uint32_t>(data[i + 1]) << 8) |
                          static_cast<uint32_t>(data[i + 2]);
        out.push_back(kBase64Chars[(triple >> 18) & 0x3F]);
        out.push_back(kBase64Chars[(triple >> 12) & 0x3F]);
        out.push_back(kBase64Chars[(triple >> 6) & 0x3F]);
        out.push_back(kBase64Chars[triple & 0x3F]);
        i += 3;
    }

    if (i < data.size()) {
        if (data.size() - i == 1) {
            uint32_t triple = static_cast<uint32_t>(data[i]) << 16;
            out.push_back(kBase64Chars[(triple >> 18) & 0x3F]);
            out.push_back(kBase64Chars[(triple >> 12) & 0x3F]);
            out.push_back('=');
            out.push_back('=');
        } else if (data.size() - i == 2) {
            uint32_t triple = (static_cast<uint32_t>(data[i]) << 16) |
                              (static_cast<uint32_t>(data[i + 1]) << 8);
            out.push_back(kBase64Chars[(triple >> 18) & 0x3F]);
            out.push_back(kBase64Chars[(triple >> 12) & 0x3F]);
            out.push_back(kBase64Chars[(triple >> 6) & 0x3F]);
            out.push_back('=');
        }
    }

    return out;
}

std::optional<std::vector<uint8_t>> fromBase64(std::string_view base64Str)
{
    // Ignore trailing whitespace/newlines if any, but clean base64 must have length % 4 == 0
    while (!base64Str.empty() && (base64Str.back() == '\r' || base64Str.back() == '\n' || base64Str.back() == ' ' || base64Str.back() == '\t')) {
        base64Str.remove_suffix(1);
    }
    while (!base64Str.empty() && (base64Str.front() == '\r' || base64Str.front() == '\n' || base64Str.front() == ' ' || base64Str.front() == '\t')) {
        base64Str.remove_prefix(1);
    }

    if (base64Str.empty()) {
        return std::vector<uint8_t>{};
    }

    if (base64Str.size() % 4 != 0) {
        return std::nullopt;
    }

    std::vector<uint8_t> out;
    out.reserve((base64Str.size() / 4) * 3);

    for (size_t i = 0; i < base64Str.size(); i += 4) {
        char c0 = base64Str[i];
        char c1 = base64Str[i + 1];
        char c2 = base64Str[i + 2];
        char c3 = base64Str[i + 3];

        int8_t v0 = kDecodeTable[static_cast<uint8_t>(c0)];
        int8_t v1 = kDecodeTable[static_cast<uint8_t>(c1)];
        if (v0 < 0 || v1 < 0) {
            return std::nullopt;
        }

        if (c2 == '=') {
            if (c3 != '=' || i + 4 != base64Str.size()) {
                return std::nullopt;
            }
            uint32_t val = (static_cast<uint32_t>(v0) << 18) | (static_cast<uint32_t>(v1) << 12);
            out.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
        } else if (c3 == '=') {
            if (i + 4 != base64Str.size()) {
                return std::nullopt;
            }
            int8_t v2 = kDecodeTable[static_cast<uint8_t>(c2)];
            if (v2 < 0) {
                return std::nullopt;
            }
            uint32_t val = (static_cast<uint32_t>(v0) << 18) | (static_cast<uint32_t>(v1) << 12) | (static_cast<uint32_t>(v2) << 6);
            out.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
            out.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        } else {
            int8_t v2 = kDecodeTable[static_cast<uint8_t>(c2)];
            int8_t v3 = kDecodeTable[static_cast<uint8_t>(c3)];
            if (v2 < 0 || v3 < 0) {
                return std::nullopt;
            }
            uint32_t val = (static_cast<uint32_t>(v0) << 18) | (static_cast<uint32_t>(v1) << 12) | (static_cast<uint32_t>(v2) << 6) | static_cast<uint32_t>(v3);
            out.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
            out.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>(val & 0xFF));
        }
    }

    return out;
}

} // namespace Styx::Platform
