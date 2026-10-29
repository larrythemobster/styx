#pragma once

#include <map>
#include <optional>
#include <vector>
#include "Platform/Signal.hpp"

#include "Snowflake.hpp"
#include "Discord/Enums.hpp"
#include "Discord/Entities.hpp"

#include "Storage/GuildRepository.hpp"
#include "Storage/ChannelRepository.hpp"
#include "Storage/RoleRepository.hpp"
#include "Storage/MemberRepository.hpp"

namespace Styx {
namespace Core {

class PermissionManager
{
public:
    explicit PermissionManager(Snowflake accountId);

    Discord::Permissions getChannelPermissions(Snowflake userId, Snowflake channelId);
    bool hasChannelPermission(Snowflake userId, Snowflake channelId,
                              Discord::Permissions permission);

    Discord::Permissions getGuildPermissions(Snowflake userId, Snowflake guildId);
    bool hasGuildPermission(Snowflake userId, Snowflake guildId, Discord::Permissions permission);

    void precomputeGuildPermissions(const Discord::Guild &guild, const Discord::Member &member,
                                    const std::vector<Discord::Role> &roles,
                                    const std::vector<Discord::Channel> &channels, Snowflake userId);

    void setSelfMfaEnabled(bool enabled);

    void invalidateChannelCache(Snowflake channelId);
    void invalidateUserGuildCache(Snowflake userId, Snowflake guildId);

    Platform::Signal<Snowflake> channelPermissionsChanged;
    Platform::Signal<Snowflake> guildPermissionsChanged;

private:
    Discord::Permissions computeChannelPermissions(Snowflake userId, Snowflake channelId);
    std::optional<Discord::Permissions> computeGuildPermissions(Snowflake userId, Snowflake guildId);
    [[nodiscard]] Discord::Permissions applyMfaRequirement(Discord::Permissions permissions, Snowflake userId, const Discord::Guild &guild) const;

    Snowflake accountId;
    bool selfMfaEnabled = true;

    Storage::RoleRepository roleRepo;
    Storage::GuildRepository guildRepo;
    Storage::ChannelRepository channelRepo;
    Storage::MemberRepository memberRepo;

    std::map<std::pair<Snowflake, Snowflake>, Discord::Permissions> permissionCache;
    std::map<std::pair<Snowflake, Snowflake>, Discord::Permissions> guildPermissionCache;
};

} // namespace Core
} // namespace Styx
