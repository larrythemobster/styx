#include "RoleRepository.hpp"
#include "DatabaseManager.hpp"
#include "Core/Logging.hpp"


namespace Styx {
namespace Storage {

static Discord::Role readRoleFromQuery(const Platform::Statement &q)
{
    Discord::Role role;
    role.id = static_cast<Core::Snowflake>(q.getInt64(0));
    role.name = q.getText(1);
    role.permissions = Discord::Permissions::fromInt(q.getInt64(2));
    role.position = q.getInt(3);
    if (!q.isNull(4))
        role.color = q.getInt(4);
    if (!q.isNull(5))
        role.hoist = q.getInt(5) != 0;
    if (!q.isNull(6))
        role.icon = q.getText(6);
    if (!q.isNull(7))
        role.unicodeEmoji = q.getText(7);
    if (!q.isNull(8))
        role.managed = q.getInt(8) != 0;
    if (!q.isNull(9))
        role.mentionable = q.getInt(9) != 0;
    if (!q.isNull(10)) {
        Discord::RoleColors colors;
        colors.primaryColor = q.getInt(10);
        if (q.isNull(11))
            colors.secondaryColor = nullptr;
        else
            colors.secondaryColor = q.getInt(11);
        if (q.isNull(12))
            colors.tertiaryColor = nullptr;
        else
            colors.tertiaryColor = q.getInt(12);
        role.colors = colors;
    }
    if (!q.isNull(13))
        role.flags = q.getInt(13);
    if (!q.isNull(14)) {
        role.tagsJson = q.getText(14);
        auto tags = nlohmann::json::parse(role.tagsJson, nullptr, false);
        role.tags = Discord::RoleTags::fromJson(tags.is_object() ? tags : nlohmann::json::object());
    }
    return role;
}

RoleRepository::RoleRepository(Core::Snowflake accountId)
    : BaseRepository(DatabaseManager::getCacheConnectionName(accountId))
{
}

static const char *ROLE_UPSERT_SQL = R"(
    INSERT OR REPLACE INTO roles
    (id, guild_id, name, permissions, position, color, hoist, icon,
     unicode_emoji, managed, mentionable, colors_primary, colors_secondary,
     colors_tertiary, flags, tags)
    VALUES (:id, :guild_id, :name, :permissions, :position, :color, :hoist,
            :icon, :unicode_emoji, :managed, :mentionable, :colors_primary,
            :colors_secondary, :colors_tertiary, :flags, :tags)
)";

static const char *ROLE_COLUMNS =
    "id, name, permissions, position, color, hoist, icon, unicode_emoji, managed, mentionable, "
    "colors_primary, colors_secondary, colors_tertiary, flags, tags";

void RoleRepository::bindRole(Platform::Statement &q, Core::Snowflake guildId, const Discord::Role &role)
{
    q.bind(":id", static_cast<int64_t>(role.id.get()));
    q.bind(":guild_id", static_cast<int64_t>(guildId));
    std::string name = role.name.get();
    q.bind(":name", std::string_view(name));
    q.bind(":permissions", static_cast<int64_t>(role.permissions.get()));
    q.bind(":position", static_cast<int>(role.position.get()));
    bindOptional(q, ":color", role.color);
    bindOptional(q, ":hoist", role.hoist);
    bindOptional(q, ":icon", role.icon);
    bindOptional(q, ":unicode_emoji", role.unicodeEmoji);
    bindOptional(q, ":managed", role.managed);
    bindOptional(q, ":mentionable", role.mentionable);

    const bool hasColors = role.colors.hasValue();
    if (hasColors)
        q.bind(":colors_primary", static_cast<int>(role.colors->primaryColor.get()));
    else
        q.bindNull(":colors_primary");

    if (hasColors && role.colors->secondaryColor.hasValue())
        q.bind(":colors_secondary", static_cast<int>(role.colors->secondaryColor.get()));
    else
        q.bindNull(":colors_secondary");

    if (hasColors && role.colors->tertiaryColor.hasValue())
        q.bind(":colors_tertiary", static_cast<int>(role.colors->tertiaryColor.get()));
    else
        q.bindNull(":colors_tertiary");

    bindOptional(q, ":flags", role.flags);

    if (!role.tagsJson.empty()) {
        std::string t = role.tagsJson;
        q.bind(":tags", std::string_view(t));
    } else {
        q.bindNull(":tags");
    }
}

void RoleRepository::saveRole(Core::Snowflake guildId, const Discord::Role &role)
{
    auto &db = getDb();
    saveRole(guildId, role, db);
}

void RoleRepository::saveRole(Core::Snowflake guildId, const Discord::Role &role, Platform::Database &db)
{
    try {
        auto q = db.prepare(ROLE_UPSERT_SQL);
        bindRole(q, guildId, role);
        execLogged(q, "RoleRepository: Save role");
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "RoleRepository: Save role failed:" << e.what();
    }
}

void RoleRepository::saveRoles(Core::Snowflake guildId, const std::vector<Discord::Role> &roles)
{
    auto &db = getDb();
    saveRoles(guildId, roles, db);
}

void RoleRepository::saveRoles(Core::Snowflake guildId, const std::vector<Discord::Role> &roles,
                               Platform::Database &db)
{
    if (roles.empty())
        return;

    try {
        auto q = db.prepare(ROLE_UPSERT_SQL);
        for (const auto &role : roles) {
            bindRole(q, guildId, role);
            execLogged(q, "RoleRepository: Save role");
            q.reset();
        }
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "RoleRepository: Save roles failed:" << e.what();
    }
}

std::optional<Discord::Role> RoleRepository::getRole(Core::Snowflake guildId,
                                                     Core::Snowflake roleId)
{
    auto &db = getDb();
    try {
        std::string sql = std::string("SELECT ") + ROLE_COLUMNS + " FROM roles WHERE guild_id = :guild_id AND id = :id";
        auto q = db.prepare(sql);
        q.bind(":guild_id", static_cast<int64_t>(guildId));
        q.bind(":id", static_cast<int64_t>(roleId));

        if (!q.step())
            return std::nullopt;

        return readRoleFromQuery(q);
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "RoleRepository: getRole failed:" << e.what();
        return std::nullopt;
    }
}

std::vector<Discord::Role> RoleRepository::getRolesForGuild(Core::Snowflake guildId)
{
    std::vector<Discord::Role> roles;
    auto &db = getDb();
    try {
        std::string sql = std::string("SELECT ") + ROLE_COLUMNS + " FROM roles WHERE guild_id = :guild_id ORDER BY position ASC";
        auto q = db.prepare(sql);
        q.bind(":guild_id", static_cast<int64_t>(guildId));

        while (q.step())
            roles.push_back(readRoleFromQuery(q));
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "RoleRepository: Get roles failed:" << e.what();
    }

    return roles;
}

void RoleRepository::deleteRole(Core::Snowflake guildId, Core::Snowflake roleId)
{
    auto &db = getDb();
    deleteRole(guildId, roleId, db);
}

void RoleRepository::deleteRole(Core::Snowflake guildId, Core::Snowflake roleId, Platform::Database &db)
{
    try {
        auto q = db.prepare("DELETE FROM roles WHERE guild_id = :guild_id AND id = :id");
        q.bind(":guild_id", static_cast<int64_t>(guildId));
        q.bind(":id", static_cast<int64_t>(roleId));
        execLogged(q, "RoleRepository: Delete role");
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "RoleRepository: Delete role failed:" << e.what();
    }
}

void RoleRepository::deleteRolesForGuild(Core::Snowflake guildId)
{
    auto &db = getDb();
    deleteRolesForGuild(guildId, db);
}

void RoleRepository::deleteRolesForGuild(Core::Snowflake guildId, Platform::Database &db)
{
    try {
        auto q = db.prepare("DELETE FROM roles WHERE guild_id = :guild_id");
        q.bind(":guild_id", static_cast<int64_t>(guildId));
        execLogged(q, "RoleRepository: Delete roles for guild");
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "RoleRepository: Delete roles for guild failed:" << e.what();
    }
}

} // namespace Storage
} // namespace Styx
