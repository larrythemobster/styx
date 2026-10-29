#pragma once

#include <algorithm>
#include <vector>

#include <optional>

#include "Discord/Entities.hpp"
#include "Snowflake.hpp"

namespace Styx {
namespace Core {

class RoleHierarchy
{
public:
    RoleHierarchy(Snowflake guildId, Snowflake ownerId, Snowflake selfId, const std::vector<Discord::Role> &roles, const std::vector<Snowflake> &selfRoleIds);

    static bool sortsBefore(const Discord::Role &a, const Discord::Role &b, Snowflake guildId);
    static std::vector<Discord::Role> sorted(std::vector<Discord::Role> roles, Snowflake guildId);

    [[nodiscard]] const std::vector<Discord::Role> &sortedRoles() const { return roles; }
    [[nodiscard]] Snowflake owner() const { return ownerId; }
    [[nodiscard]] bool isOwner() const { return selfId == ownerId; }
    [[nodiscard]] const std::vector<Snowflake> &selfRoles() const { return selfRoleIds; }
    [[nodiscard]] bool selfHolds(Snowflake roleId) const { return roleId == guildId || std::find(selfRoleIds.begin(), selfRoleIds.end(), roleId) != selfRoleIds.end(); }

    [[nodiscard]] std::optional<Discord::Role> selfHighestRole() const;

    [[nodiscard]] bool outranksRole(const Discord::Role &role) const;
    [[nodiscard]] bool outranksMember(Snowflake userId, const std::vector<Snowflake> &roleIds) const;

private:
    [[nodiscard]] std::optional<Discord::Role> highestRole(const std::vector<Snowflake> &roleIds) const;

    Snowflake guildId;
    Snowflake ownerId;
    Snowflake selfId;
    std::vector<Discord::Role> roles;
    std::vector<Snowflake> selfRoleIds;
};

} // namespace Core
} // namespace Styx
