// Ported from ouwou upstream/video-streams (71b4907); original implementation retained.
#include "StreamKey.hpp"

#include <QStringList>

namespace Styx {
namespace Discord {

namespace {

const QLatin1String GUILD_TYPE("guild");
const QLatin1String CALL_TYPE("call");

std::optional<Core::Snowflake> parseId(const QString &text)
{
    bool isNumber = false;
    const uint64_t value = text.toULongLong(&isNumber);
    if (!isNumber || value == 0)
        return std::nullopt;
    return Core::Snowflake(value);
}

} // namespace

StreamKey StreamKey::forChannel(Core::Snowflake guildId, Core::Snowflake channelId, Core::Snowflake ownerId)
{
    StreamKey key;
    key.type = guildId.isValid() ? Type::Guild : Type::Call;
    if (guildId.isValid())
        key.guildId = guildId;
    key.channelId = channelId;
    key.ownerId = ownerId;
    return key;
}

std::optional<StreamKey> StreamKey::parse(const QString &key)
{
    const QStringList parts = key.split(QLatin1Char(':'));
    if (parts.isEmpty())
        return std::nullopt;

    StreamKey parsed;
    int idIndex = 1;
    if (parts.first() == GUILD_TYPE && parts.size() == 4) {
        parsed.type = Type::Guild;
        const auto guildId = parseId(parts.at(idIndex++));
        if (!guildId)
            return std::nullopt;
        parsed.guildId = *guildId;
    } else if (parts.first() == CALL_TYPE && parts.size() == 3) {
        parsed.type = Type::Call;
    } else {
        return std::nullopt;
    }

    const auto channelId = parseId(parts.at(idIndex++));
    const auto ownerId = parseId(parts.at(idIndex));
    if (!channelId || !ownerId)
        return std::nullopt;
    parsed.channelId = *channelId;
    parsed.ownerId = *ownerId;

    return parsed;
}

QString StreamKey::toString() const
{
    if (type == Type::Guild)
        return QStringLiteral("guild:%1:%2:%3").arg(static_cast<uint64_t>(guildId)).arg(static_cast<uint64_t>(channelId)).arg(static_cast<uint64_t>(ownerId));
    return QStringLiteral("call:%1:%2").arg(static_cast<uint64_t>(channelId)).arg(static_cast<uint64_t>(ownerId));
}

QString StreamKey::typeName() const
{
    return type == Type::Guild ? GUILD_TYPE : CALL_TYPE;
}

} // namespace Discord
} // namespace Styx
