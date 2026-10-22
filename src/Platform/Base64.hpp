#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Styx::Platform {

// Encodes data into a base64 string.
std::string toBase64(std::span<const uint8_t> data);
inline std::string toBase64(std::string_view data)
{
    return toBase64(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(data.data()), data.size()));
}

// Decodes a base64 string into raw bytes. Returns std::nullopt if input has invalid characters or padding.
std::optional<std::vector<uint8_t>> fromBase64(std::string_view base64Str);

} // namespace Styx::Platform
