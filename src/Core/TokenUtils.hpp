#pragma once

#include "Snowflake.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace Styx {
namespace Core {
namespace TokenUtils {

inline std::string decodeBase64(std::string_view encoded)
{
    std::string decoded;
    decoded.reserve(encoded.size() * 3 / 4);
    unsigned accumulator = 0;
    int bits = 0;
    for (unsigned char c : encoded) {
        int value = -1;
        if (c >= 'A' && c <= 'Z') value = c - 'A';
        else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
        else if (c >= '0' && c <= '9') value = c - '0' + 52;
        else if (c == '+') value = 62;
        else if (c == '/') value = 63;
        if (value < 0)
            continue;
        accumulator = (accumulator << 6) | static_cast<unsigned>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<char>((accumulator >> bits) & 0xff));
        }
    }
    return decoded;
}

inline std::optional<std::uint64_t> parseId(std::string_view text)
{
    const auto isSpace = [](char c) { return c == ' ' || (c >= '\t' && c <= '\r'); };
    while (!text.empty() && isSpace(text.front())) text.remove_prefix(1);
    while (!text.empty() && isSpace(text.back())) text.remove_suffix(1);
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    if (text.empty()) return std::nullopt;
    std::uint64_t id = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), id);
    if (error != std::errc{} || end != text.data() + text.size())
        return std::nullopt;
    return id;
}

inline Snowflake getIdAndCheckToken(std::string_view token)
{
    const auto first = token.find('.');
    if (first == std::string_view::npos)
        return Snowflake::Invalid;
    const auto second = token.find('.', first + 1);
    if (second == std::string_view::npos)
        return Snowflake::Invalid;

    if (auto id = parseId(decodeBase64(token.substr(0, first))))
        return Snowflake(*id);

    const auto payload = decodeBase64(token.substr(first + 1, second - first - 1));
    const auto json = nlohmann::json::parse(payload, nullptr, false);
    if (!json.is_object())
        return Snowflake::Invalid;
    const auto it = json.find("id");
    if (it == json.end())
        return Snowflake::Invalid;
    if (it->is_string()) {
        if (auto id = parseId(it->get_ref<const std::string &>()))
            return Snowflake(*id);
    } else if (it->is_number_unsigned()) {
        return Snowflake(it->get<std::uint64_t>());
    } else if (it->is_number_integer()) {
        const auto id = it->get<std::int64_t>();
        if (id >= 0)
            return Snowflake(static_cast<std::uint64_t>(id));
    } else if (it->is_number_float()) {
        const double id = std::round(it->get<double>());
        if (std::isfinite(id) && id >= 0 && id < 18446744073709551616.0)
            return Snowflake(static_cast<std::uint64_t>(id));
    }
    return Snowflake::Invalid;
}

} // namespace TokenUtils
} // namespace Core
} // namespace Styx
