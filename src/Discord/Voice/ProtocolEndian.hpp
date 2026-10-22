#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Styx::Discord::Voice {
template <class T>
T readBigEndian(const uint8_t *bytes)
{
    using U = std::make_unsigned_t<T>;
    U value = 0;
    for (size_t i = 0; i < sizeof(T); ++i) value = static_cast<U>((value << 8) | bytes[i]);
    return static_cast<T>(value);
}
template <class T>
void writeBigEndian(T value, uint8_t *bytes)
{
    using U = std::make_unsigned_t<T>;
    U bits = static_cast<U>(value);
    for (size_t i = sizeof(T); i-- > 0;) { bytes[i] = static_cast<uint8_t>(bits); bits >>= 8; }
}
} // namespace Styx::Discord::Voice
