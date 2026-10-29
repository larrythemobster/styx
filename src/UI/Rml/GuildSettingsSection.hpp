#pragma once

#include <string>
#include <vector>

#include "Core/Snowflake.hpp"

namespace Styx {
namespace Core {
class ClientInstance;
}
namespace UI {

enum class GuildSettingsSection {
    Profile,
    Engagement,
    BoostPerks,
    Emoji,
    Stickers,
    Members,
    Roles,
    Invites,
    AuditLog,
    Bans,
    DeleteServer,
};

enum class GuildSettingsGroup {
    Server,
    Expression,
    People,
    Moderation,
    Danger,
};

namespace GuildSettingsAccess {

[[nodiscard]] bool canOpen(Core::ClientInstance *instance, Core::Snowflake guildId);
[[nodiscard]] std::vector<GuildSettingsSection> visibleSections(Core::ClientInstance *instance, Core::Snowflake guildId);
[[nodiscard]] bool canEdit(Core::ClientInstance *instance, Core::Snowflake guildId, GuildSettingsSection section);
[[nodiscard]] bool selfHasMfa(Core::ClientInstance *instance);

[[nodiscard]] std::string title(GuildSettingsSection section);
[[nodiscard]] GuildSettingsGroup group(GuildSettingsSection section);
[[nodiscard]] std::string groupTitle(GuildSettingsGroup group);


} // namespace GuildSettingsAccess


} // namespace UI
} // namespace Styx
