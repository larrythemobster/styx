#include "GuildSettingsSection.hpp"


#include "Core/ClientInstance.hpp"

namespace Styx {
namespace UI {
namespace GuildSettingsAccess {

using Discord::Permission;

namespace {


bool hasAny(Core::ClientInstance *instance, Core::Snowflake guildId, Discord::Permissions permissions)
{
    const auto granted = instance->permissions()->getGuildPermissions(instance->accountId(), guildId);
    return (granted & permissions) != Discord::NO_PERMISSIONS;
}


} // namespace

bool canOpen(Core::ClientInstance *instance, Core::Snowflake guildId)
{
    return instance && instance->getGuild(guildId).has_value();
}

std::vector<GuildSettingsSection> visibleSections(Core::ClientInstance *instance, Core::Snowflake guildId)
{
    const auto guild = instance ? instance->getGuild(guildId) : std::nullopt;
    if (!guild)
        return {};
    auto has = [&](Discord::Permissions permissions) { return hasAny(instance, guildId, permissions); };

    std::vector<GuildSettingsSection> sections{ GuildSettingsSection::Profile, GuildSettingsSection::Engagement,
                                          GuildSettingsSection::BoostPerks, GuildSettingsSection::Emoji,
                                          GuildSettingsSection::Stickers, GuildSettingsSection::Members,
                                          GuildSettingsSection::Roles };

    if (has(Permission::MANAGE_GUILD | Permission::VIEW_AUDIT_LOG))
        sections.push_back(GuildSettingsSection::Invites);
    if (has(Permission::VIEW_AUDIT_LOG))
        sections.push_back(GuildSettingsSection::AuditLog);
    if (has(Permission::BAN_MEMBERS | Permission::VIEW_AUDIT_LOG))
        sections.push_back(GuildSettingsSection::Bans);
    if (guild->ownerId.get() == instance->accountId())
        sections.push_back(GuildSettingsSection::DeleteServer);
    return sections;
}

bool selfHasMfa(Core::ClientInstance *instance)
{
    return instance && instance->discord()->getMe().mfaEnabled.valueOr(false);
}

bool canEdit(Core::ClientInstance *instance, Core::Snowflake guildId, GuildSettingsSection section)
{
    if (!instance)
        return false;
    auto has = [&](Discord::Permissions permissions) { return hasAny(instance, guildId, permissions); };

    switch (section) {
    case GuildSettingsSection::Profile:
    case GuildSettingsSection::Engagement:
    case GuildSettingsSection::BoostPerks:
        return has(Permission::MANAGE_GUILD);
    case GuildSettingsSection::Invites:
        return has(Permission::MANAGE_GUILD);
    case GuildSettingsSection::Emoji:
    case GuildSettingsSection::Stickers:
        return has(Permission::MANAGE_EXPRESSIONS | Permission::CREATE_EXPRESSIONS);
    case GuildSettingsSection::Members:
        return has(Permission::ADMINISTRATOR |
                   Permission::MANAGE_GUILD |
                   Permission::BAN_MEMBERS |
                   Permission::KICK_MEMBERS |
                   Permission::MODERATE_MEMBERS |
                   Permission::MANAGE_ROLES |
                   Permission::MANAGE_NICKNAMES);
    case GuildSettingsSection::Roles:
        return has(Permission::MANAGE_ROLES);
    case GuildSettingsSection::AuditLog:
        return has(Permission::VIEW_AUDIT_LOG);
    case GuildSettingsSection::Bans:
        return has(Permission::BAN_MEMBERS);
    case GuildSettingsSection::DeleteServer: {
        const auto guild = instance->getGuild(guildId);
        return guild &&
               guild->ownerId.get() == instance->accountId() &&
               (selfHasMfa(instance) || guild->mfaLevel.valueOr(Discord::MfaLevel::NONE) != Discord::MfaLevel::ELEVATED);
    }
    }
    return false;
}

std::string title(GuildSettingsSection section)
{
    switch (section) {
    case GuildSettingsSection::Profile:
        return "Server Profile";
    case GuildSettingsSection::Engagement:
        return "Engagement";
    case GuildSettingsSection::BoostPerks:
        return "Boost Perks";
    case GuildSettingsSection::Emoji:
        return "Emoji";
    case GuildSettingsSection::Stickers:
        return "Stickers";
    case GuildSettingsSection::Members:
        return "Members";
    case GuildSettingsSection::Roles:
        return "Roles";
    case GuildSettingsSection::Invites:
        return "Invites";
    case GuildSettingsSection::AuditLog:
        return "Audit Log";
    case GuildSettingsSection::Bans:
        return "Bans";
    case GuildSettingsSection::DeleteServer:
        return "Delete Server";
    }
    return {};
}

GuildSettingsGroup group(GuildSettingsSection section)
{
    switch (section) {
    case GuildSettingsSection::Profile:
    case GuildSettingsSection::Engagement:
    case GuildSettingsSection::BoostPerks:
        return GuildSettingsGroup::Server;
    case GuildSettingsSection::Emoji:
    case GuildSettingsSection::Stickers:
        return GuildSettingsGroup::Expression;
    case GuildSettingsSection::Members:
    case GuildSettingsSection::Roles:
    case GuildSettingsSection::Invites:
        return GuildSettingsGroup::People;
    case GuildSettingsSection::AuditLog:
    case GuildSettingsSection::Bans:
        return GuildSettingsGroup::Moderation;
    case GuildSettingsSection::DeleteServer:
        return GuildSettingsGroup::Danger;
    }
    return GuildSettingsGroup::Server;
}

std::string groupTitle(GuildSettingsGroup group)
{
    switch (group) {
    case GuildSettingsGroup::Expression:
        return "Expression";
    case GuildSettingsGroup::People:
        return "People";
    case GuildSettingsGroup::Moderation:
        return "Moderation";
    case GuildSettingsGroup::Server:
    case GuildSettingsGroup::Danger:
        return {};
    }
    return {};
}


} // namespace GuildSettingsAccess
} // namespace UI
} // namespace Styx
