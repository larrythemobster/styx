#pragma once

#include "Platform/Native.hpp"
#include <compare>
#include <cstdint>
#include <functional>
#include <string>

namespace Styx::Core {
class Snowflake
{
public:
    Snowflake();
    Snowflake(std::uint64_t n);
    [[nodiscard]] bool isValid() const noexcept;
    auto operator<=>(const Snowflake &) const noexcept = default;
    std::string toString() const;
    operator std::uint64_t() const noexcept;
    [[nodiscard]] Platform::DateTime toDateTime() const noexcept;
    static const Snowflake Invalid;
    static constexpr std::uint64_t SecondsInterval = 4194304000ULL;
    static Snowflake generateNonce();
    static Snowflake fromUnixMs(std::int64_t ms);
private:
    std::uint64_t value;
};
} // namespace Styx::Core
namespace std {
template <> struct hash<Styx::Core::Snowflake> {
    size_t operator()(Styx::Core::Snowflake value) const noexcept { return hash<uint64_t>{}(static_cast<uint64_t>(value)); }
};
}
