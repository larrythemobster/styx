#include "PermissionManager.hpp"
#include "PermissionComputer.hpp"
#include "Logging.hpp"

#include "Storage/RoleRepository.hpp"
#include "Storage/GuildRepository.hpp"
#include "Storage/ChannelRepository.hpp"
#include "Storage/MemberRepository.hpp"
#include <unordered_set>

namespace Styx {
namespace Core {

PermissionManager::PermissionManager(Snowflake accountId)
    : accountId(accountId),
      roleRepo(accountId),
      guildRepo(accountId),
      channelRepo(accountId),
      memberRepo(accountId)
{
}

void PermissionManager::setSelfMfaEnabled(bool enabled)
{
    selfMfaEnabled = enabled;
}

Discord::Permissions PermissionManager::applyMfaRequirement(Discord::Permissions permissions, Snowflake userId, const Discord::Guild &guild) const
{
    if (userId != accountId || selfMfaEnabled || guild.mfaLevel.valueOr(Discord::MfaLevel::NONE) != Discord::MfaLevel::ELEVATED)
        return permissions;
    return permissions & ~Discord::MFA_ELEVATED_PERMISSIONS;
}

Discord::Permissions PermissionManager::getChannelPermissions(Snowflake userId, Snowflake channelId)
{
    auto cacheKey = std::make_pair(userId, channelId);
    if (auto it = permissionCache.find(cacheKey); it != permissionCache.end())
        return it->second;

    auto permissions = computeChannelPermissions(userId, channelId);

    permissionCache.insert_or_assign(cacheKey, permissions);

    return permissions;
}

bool PermissionManager::hasChannelPermission(Snowflake userId, Snowflake channelId,
                                             Discord::Permissions permission)
{
    auto perms = getChannelPermissions(userId, channelId);
    return (perms & permission) == permission;
}

Discord::Permissions PermissionManager::getGuildPermissions(Snowflake userId, Snowflake guildId)
{
    if (userId != accountId)
        return computeGuildPermissions(userId, guildId).value_or(Discord::NO_PERMISSIONS);

    auto cacheKey = std::make_pair(userId, guildId);
    auto cached = guildPermissionCache.find(cacheKey);
    if (cached != guildPermissionCache.end())
        return cached->second;

    const auto permissions = computeGuildPermissions(userId, guildId);
    if (permissions)
        guildPermissionCache.insert_or_assign(cacheKey, *permissions);
    return permissions.value_or(Discord::NO_PERMISSIONS);
}

bool PermissionManager::hasGuildPermission(Snowflake userId, Snowflake guildId, Discord::Permissions permission)
{
    return (getGuildPermissions(userId, guildId) & permission) == permission;
}

std::optional<Discord::Permissions> PermissionManager::computeGuildPermissions(Snowflake userId, Snowflake guildId)
{
    auto guildOpt = guildRepo.getGuild(guildId);
    if (!guildOpt)
        return std::nullopt;

    auto memberOpt = memberRepo.getMember(guildId, userId);
    if (!memberOpt && guildOpt->ownerId.get() != userId)
        return std::nullopt;

    std::vector<Snowflake> memberRoleIds;
    if (memberOpt && memberOpt->roles.hasValue())
        memberRoleIds = memberOpt->roles.get();

    auto permissions = PermissionComputer::computeBasePermissions(guildOpt->ownerId.get(), userId, guildId, memberRoleIds, roleRepo.getRolesForGuild(guildId));
    return applyMfaRequirement(permissions, userId, *guildOpt);
}

void PermissionManager::precomputeGuildPermissions(const Discord::Guild &guild,
                                                   const Discord::Member &member,
                                                   const std::vector<Discord::Role> &roles,
                                                   const std::vector<Discord::Channel> &channels,
                                                   Snowflake userId)
{
    std::vector<Snowflake> memberRoleIds;
    if (member.roles.hasValue())
        memberRoleIds = member.roles.get();

    auto guildPermissions = PermissionComputer::computeBasePermissions(guild.ownerId.get(), userId, guild.id.get(), memberRoleIds, roles);
    guildPermissionCache.insert_or_assign(std::make_pair(userId, guild.id.get()), applyMfaRequirement(guildPermissions, userId, guild));

    for (const auto &channel : channels) {
        std::vector<Discord::PermissionOverwrite> overwrites;
        if (channel.permissionOverwrites.hasValue())
            overwrites = channel.permissionOverwrites.get();

        auto permissions = PermissionComputer::computeChannelPermissions(
                guild.ownerId.get(), userId, guild.id.get(), false, memberRoleIds, roles,
                overwrites);

        auto cacheKey = std::make_pair(userId, channel.id.get());
        permissionCache.insert_or_assign(cacheKey, applyMfaRequirement(permissions, userId, guild));
    }
}

Discord::Permissions PermissionManager::computeChannelPermissions(Snowflake userId,
                                                                  Snowflake channelId)
{
    auto channelOpt = channelRepo.getChannel(channelId);
    if (!channelOpt) {
        log(LogLevel::Warning, LogCore) << "PermissionManager: Channel not found:" << channelId;
        return Discord::NO_PERMISSIONS;
    }

    const auto &channel = *channelOpt;

    bool isDM = !channel.guildId.hasValue();

    if (isDM) {
        return PermissionComputer::computeChannelPermissions(Snowflake::Invalid, userId,
                                                             Snowflake::Invalid, true, {}, {}, {});
    }

    Snowflake guildId = channel.guildId.get();

    auto guildOpt = guildRepo.getGuild(guildId);
    if (!guildOpt) {
        log(LogLevel::Warning, LogCore) << "PermissionManager: Guild not found:" << guildId;
        return Discord::NO_PERMISSIONS;
    }

    const auto &guild = *guildOpt;

    auto memberOpt = memberRepo.getMember(guildId, userId);
    if (!memberOpt) {
        log(LogLevel::Warning, LogCore) << "PermissionManager: Member not found:" << userId << "in" << guildId;
        return Discord::NO_PERMISSIONS;
    }

    const auto &member = *memberOpt;

    auto allRoles = roleRepo.getRolesForGuild(guildId);

    std::vector<Discord::PermissionOverwrite> overwrites;
    if (channel.permissionOverwrites.hasValue())
        overwrites = channel.permissionOverwrites.get();

    std::vector<Snowflake> memberRoleIds;
    if (member.roles.hasValue())
        memberRoleIds = member.roles.get();

    auto permissions = PermissionComputer::computeChannelPermissions(guild.ownerId.get(), userId, guildId, false, memberRoleIds, allRoles, overwrites);
    return applyMfaRequirement(permissions, userId, guild);
}

void PermissionManager::invalidateChannelCache(Snowflake channelId)
{
    auto it = permissionCache.begin();
    while (it != permissionCache.end()) {
        if (it->first.second == channelId)
            it = permissionCache.erase(it);
        else
            ++it;
    }

    log(LogLevel::Debug, LogCore) << "Invalidated permission cache for channel:" << channelId;
    channelPermissionsChanged.publish(channelId);
}

void PermissionManager::invalidateUserGuildCache(Snowflake userId, Snowflake guildId)
{
    guildPermissionCache.erase(std::make_pair(userId, guildId));

    auto channels = channelRepo.getChannelsForGuild(guildId);

    std::unordered_set<Snowflake> channelIds;
    channelIds.reserve(channels.size());
    for (const auto &channel : channels)
        channelIds.insert(channel.id.get());

    std::vector<Snowflake> invalidated;
    auto it = permissionCache.begin();
    while (it != permissionCache.end()) {
        if (it->first.first == userId && channelIds.contains(it->first.second)) {
            invalidated.push_back(it->first.second);
            it = permissionCache.erase(it);
        } else {
            ++it;
        }
    }

    for (Snowflake channelId : invalidated)
        channelPermissionsChanged.publish(channelId);
    guildPermissionsChanged.publish(guildId);

    log(LogLevel::Debug, LogCore) << "Invalidated" << invalidated.size() << "cached permissions for user" << userId << "in guild:" << guildId;
}

} // namespace Core
} // namespace Styx
