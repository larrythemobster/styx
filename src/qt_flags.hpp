#pragma once

// 64-bit QFlags arrived in 6.9

#include <QFlags>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)

#define STYX_DECLARE_FLAGS(FlagsName, EnumName) \
    Q_DECLARE_FLAGS(FlagsName, EnumName)           \
    Q_DECLARE_OPERATORS_FOR_FLAGS(FlagsName)

#else

#include <QDebug>

#include <type_traits>

namespace Styx {
namespace Compat {

template <typename Enum>
class Flags
{
public:
    using enum_type = Enum;
    using Int = std::underlying_type_t<Enum>;

    constexpr Flags() noexcept : i(0) {}
    constexpr Flags(Enum e) noexcept : i(static_cast<Int>(e)) {}
    constexpr explicit Flags(Int v) noexcept : i(v) {}

    constexpr static Flags fromInt(Int v) noexcept
    {
        Flags f;
        f.i = v;
        return f;
    }
    constexpr Int toInt() const noexcept { return i; }
    constexpr operator Int() const noexcept { return i; }

    friend QDebug operator<<(QDebug dbg, Flags f) { return dbg << f.i; }

    constexpr Flags operator~() const noexcept { return fromInt(~i); }
    constexpr Flags operator&(Flags o) const noexcept { return fromInt(i & o.i); }
    constexpr Flags operator&(Enum e) const noexcept { return fromInt(i & static_cast<Int>(e)); }
    constexpr Flags operator|(Flags o) const noexcept { return fromInt(i | o.i); }
    constexpr Flags operator|(Enum e) const noexcept { return fromInt(i | static_cast<Int>(e)); }
    constexpr Flags operator^(Flags o) const noexcept { return fromInt(i ^ o.i); }
    constexpr Flags operator^(Enum e) const noexcept { return fromInt(i ^ static_cast<Int>(e)); }

    constexpr Flags &operator&=(Flags o) noexcept
    {
        i &= o.i;
        return *this;
    }
    constexpr Flags &operator&=(Enum e) noexcept
    {
        i &= static_cast<Int>(e);
        return *this;
    }
    constexpr Flags &operator|=(Flags o) noexcept
    {
        i |= o.i;
        return *this;
    }
    constexpr Flags &operator|=(Enum e) noexcept
    {
        i |= static_cast<Int>(e);
        return *this;
    }
    constexpr Flags &operator^=(Flags o) noexcept
    {
        i ^= o.i;
        return *this;
    }
    constexpr Flags &operator^=(Enum e) noexcept
    {
        i ^= static_cast<Int>(e);
        return *this;
    }

    constexpr bool testFlag(Enum e) const noexcept
    {
        const Int f = static_cast<Int>(e);
        return (i & f) == f && (f != 0 || i == 0);
    }
    constexpr Flags &setFlag(Enum e, bool on = true) noexcept
    {
        return on ? (*this |= e) : (*this &= ~Flags(e));
    }

private:
    Int i;
};

} // namespace Compat
} // namespace Styx

#define STYX_DECLARE_FLAGS(FlagsName, EnumName)                        \
    using FlagsName = ::Styx::Compat::Flags<EnumName>;                 \
    constexpr inline FlagsName operator|(EnumName a, EnumName b) noexcept \
    {                                                                     \
        return FlagsName(a) | b;                                          \
    }                                                                     \
    constexpr inline FlagsName operator&(EnumName a, EnumName b) noexcept \
    {                                                                     \
        return FlagsName(a) & b;                                          \
    }                                                                     \
    constexpr inline FlagsName operator^(EnumName a, EnumName b) noexcept \
    {                                                                     \
        return FlagsName(a) ^ b;                                          \
    }                                                                     \
    constexpr inline FlagsName operator~(EnumName a) noexcept             \
    {                                                                     \
        return ~FlagsName(a);                                             \
    }

#endif
