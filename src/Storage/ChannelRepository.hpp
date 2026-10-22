#pragma once

#include <optional>
#include <QList>
#include <QHash>

#include "BaseRepository.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"
#include "Platform/Sqlite.hpp"

namespace Styx {
namespace Storage {

class ChannelRepository : public BaseRepository
{
public:
    ChannelRepository(Core::Snowflake accountId);

    void saveChannel(const Discord::Channel &channel, Platform::Database &db);
    void saveChannel(const Discord::Channel &channel);
    void deleteChannel(Core::Snowflake channelId, Platform::Database &db);
    void deleteChannel(Core::Snowflake channelId);
    void savePermissionOverwrites(Core::Snowflake channelId,
                                  const QList<Discord::PermissionOverwrite> &overwrites,
                                  Platform::Database &db);
    void saveChannelRecipients(Core::Snowflake channelId,
                               const QList<Core::Snowflake> &recipientIds,
                               Platform::Database &db);
    QList<Discord::PermissionOverwrite> getPermissionOverwrites(Core::Snowflake channelId);
    QList<Core::Snowflake> getChannelRecipientIds(Core::Snowflake channelId);

    QHash<Core::Snowflake, QList<Discord::PermissionOverwrite>> getPermissionOverwritesForGuild(
            Core::Snowflake guildId);

    std::optional<Discord::Channel> getChannel(Core::Snowflake channelId);
    QList<Discord::Channel> getChannelsForGuild(Core::Snowflake guildId);
    std::optional<Core::Snowflake> findDmChannelWithUser(Core::Snowflake userId);
};

} // namespace Storage
} // namespace Styx
