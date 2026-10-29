#pragma once

#include <type_traits>

namespace Styx::Core {

template <typename Enum>
class Flags
{
    static_assert(std::is_enum_v<Enum>);
public:
    using enum_type = Enum;
    using Int = std::underlying_type_t<Enum>;

    constexpr Flags() noexcept = default;
    constexpr Flags(Enum value) noexcept : bits(static_cast<Int>(value)) {}
    constexpr explicit Flags(Int value) noexcept : bits(value) {}
    static constexpr Flags fromInt(Int value) noexcept { return Flags(value); }
    constexpr Int toInt() const noexcept { return bits; }
    constexpr operator Int() const noexcept { return bits; }
    constexpr bool operator==(const Flags &) const noexcept = default;
    constexpr Flags operator~() const noexcept { return fromInt(static_cast<Int>(~bits)); }
    constexpr Flags operator&(Flags other) const noexcept { return fromInt(static_cast<Int>(bits & other.bits)); }
    constexpr Flags operator|(Flags other) const noexcept { return fromInt(static_cast<Int>(bits | other.bits)); }
    constexpr Flags operator^(Flags other) const noexcept { return fromInt(static_cast<Int>(bits ^ other.bits)); }
    constexpr Flags &operator&=(Flags other) noexcept { bits &= other.bits; return *this; }
    constexpr Flags &operator|=(Flags other) noexcept { bits |= other.bits; return *this; }
    constexpr Flags &operator^=(Flags other) noexcept { bits ^= other.bits; return *this; }
    constexpr bool testFlag(Enum flag) const noexcept {
        const Int mask = static_cast<Int>(flag);
        return (bits & mask) == mask && (mask != 0 || bits == 0);
    }
    constexpr bool testAnyFlag(Enum flag) const noexcept { return (bits & static_cast<Int>(flag)) != 0; }
    constexpr Flags &setFlag(Enum flag, bool on = true) noexcept {
        return on ? (*this |= flag) : (*this &= ~Flags(flag));
    }
private:
    Int bits = 0;
};

} // namespace Styx::Core
