#pragma once

#include <optional>
#include <vector>
#include <unordered_map>

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
                                  const std::vector<Discord::PermissionOverwrite> &overwrites,
                                  Platform::Database &db);
    void saveChannelRecipients(Core::Snowflake channelId,
                               const std::vector<Core::Snowflake> &recipientIds,
                               Platform::Database &db);
    std::vector<Discord::PermissionOverwrite> getPermissionOverwrites(Core::Snowflake channelId);
    std::vector<Core::Snowflake> getChannelRecipientIds(Core::Snowflake channelId);

    std::unordered_map<Core::Snowflake, std::vector<Discord::PermissionOverwrite>> getPermissionOverwritesForGuild(
            Core::Snowflake guildId);

    std::optional<Discord::Channel> getChannel(Core::Snowflake channelId);
    std::vector<Discord::Channel> getChannelsForGuild(Core::Snowflake guildId);
    std::optional<Core::Snowflake> findDmChannelWithUser(Core::Snowflake userId);
};

} // namespace Storage
} // namespace Styx
