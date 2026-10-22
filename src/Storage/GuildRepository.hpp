#pragma once

#include <optional>
#include <QString>

#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"
#include "BaseRepository.hpp"
#include "Platform/Sqlite.hpp"

namespace Styx {
namespace Storage {

class GuildRepository : public BaseRepository
{
public:
    GuildRepository(Core::Snowflake accountId);

    void saveGuild(const Discord::Guild &guild, Platform::Database &db);
    void saveGuild(const Discord::Guild &guild);
    void deleteGuild(Core::Snowflake guildId, Platform::Database &db);
    void deleteGuild(Core::Snowflake guildId);
    std::optional<Discord::Guild> getGuild(Core::Snowflake guildId);

private:
};

} // namespace Storage
} // namespace Styx
