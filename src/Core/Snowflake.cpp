#include "Snowflake.hpp"
#include <chrono>

namespace Styx::Core {
namespace { constexpr std::uint64_t DiscordEpoch = 1420070400000ULL; }
const Snowflake Snowflake::Invalid = UINT64_MAX;
Snowflake::Snowflake() : value(static_cast<std::uint64_t>(Invalid)) {}
Snowflake::Snowflake(std::uint64_t n) : value(n) {}
bool Snowflake::isValid() const noexcept { return *this != Invalid; }
std::string Snowflake::toString() const { return std::to_string(value); }
Snowflake::operator std::uint64_t() const noexcept { return value; }
Platform::DateTime Snowflake::toDateTime() const noexcept {
    return isValid() ? Platform::DateTime::fromUnixMillis(static_cast<std::int64_t>((value >> 22) + DiscordEpoch)) : Platform::DateTime{};
}
Snowflake Snowflake::generateNonce() {
    return fromUnixMs(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}
Snowflake Snowflake::fromUnixMs(std::int64_t ms) {
    if (ms <= static_cast<std::int64_t>(DiscordEpoch)) return Snowflake(0);
    return Snowflake(static_cast<std::uint64_t>(ms - DiscordEpoch) << 22);
}
} // namespace Styx::Core
