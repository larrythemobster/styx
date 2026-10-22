#pragma once

#include <QDateTime>
#include <QMetaType>

namespace Styx {
namespace Core {

class Snowflake
{
public:
    Snowflake();
    Snowflake(quint64 n);

    [[nodiscard]] bool isValid() const noexcept;

    auto operator<=>(const Snowflake &other) const noexcept { return value <=> other.value; }

    QString toString() const noexcept;
    // operator QString() const noexcept;

    operator quint64() const noexcept;

    [[nodiscard]] QDateTime toDateTime() const noexcept;

    const static Snowflake Invalid;
    const static quint64 SecondsInterval = 4194304000ULL;

    static Snowflake generateNonce();

    static Snowflake fromUnixMs(qint64 ms);

private:
    friend struct ::std::hash<Snowflake>;
    friend struct ::std::less<Snowflake>;

    quint64 value;
};

} // namespace Core
} // namespace Styx

namespace std {
template <>
struct hash<Styx::Core::Snowflake>
{
    std::size_t operator()(const Styx::Core::Snowflake &k) const { return k.value; }
};

template <>
struct less<Styx::Core::Snowflake>
{
    bool operator()(const Styx::Core::Snowflake &l, const Styx::Core::Snowflake &r) const
    {
        return l.value < r.value;
    }
};
} // namespace std

Q_DECLARE_METATYPE(Styx::Core::Snowflake)
