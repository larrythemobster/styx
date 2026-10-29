#pragma once

#include <optional>
#include <vector>

#include "BaseRepository.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"
#include "Platform/Sqlite.hpp"

namespace Styx {
namespace Storage {

class RoleRepository : public BaseRepository
{
public:
    RoleRepository(Core::Snowflake accountId);

    void saveRole(Core::Snowflake guildId, const Discord::Role &role, Platform::Database &db);
    void saveRole(Core::Snowflake guildId, const Discord::Role &role);
    void saveRoles(Core::Snowflake guildId, const std::vector<Discord::Role> &roles, Platform::Database &db);
    void saveRoles(Core::Snowflake guildId, const std::vector<Discord::Role> &roles);
    void deleteRole(Core::Snowflake guildId, Core::Snowflake roleId, Platform::Database &db);
    void deleteRole(Core::Snowflake guildId, Core::Snowflake roleId);
    void deleteRolesForGuild(Core::Snowflake guildId, Platform::Database &db);
    void deleteRolesForGuild(Core::Snowflake guildId);

    std::optional<Discord::Role> getRole(Core::Snowflake guildId, Core::Snowflake roleId);
    std::vector<Discord::Role> getRolesForGuild(Core::Snowflake guildId);

private:
    static void bindRole(Platform::Statement &q, Core::Snowflake guildId, const Discord::Role &role);
};

} // namespace Storage
} // namespace Styx
