#pragma once

#include <unordered_map>
#include <vector>

#include "Snowflake.hpp"
#include "Discord/Enums.hpp"
#include "Discord/Entities.hpp"

namespace Styx {
namespace Core {

class PermissionComputer
{
public:
    static Discord::Permissions computeBasePermissions(Snowflake guildOwnerId, Snowflake userId,
                                                       Snowflake guildId,
                                                       const std::vector<Snowflake> &memberRoleIds,
                                                       const std::vector<Discord::Role> &allRoles);

    static Discord::Permissions computeOverwrites(
            Discord::Permissions basePermissions, Snowflake userId, Snowflake guildId,
            const std::vector<Snowflake> &memberRoleIds,
            const std::vector<Discord::PermissionOverwrite> &overwrites);

    static Discord::Permissions computeChannelPermissions(
            Snowflake guildOwnerId, Snowflake userId, Snowflake guildId, bool isDM,
            const std::vector<Snowflake> &memberRoleIds, const std::vector<Discord::Role> &allRoles,
            const std::vector<Discord::PermissionOverwrite> &overwrites);

private:
    static std::unordered_map<Snowflake, const Discord::Role *> buildRoleMap(const std::vector<Discord::Role> &roles);
};

} // namespace Core
} // namespace Styx
