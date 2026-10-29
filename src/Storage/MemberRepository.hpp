#pragma once

#include <optional>
#include <vector>

#include "BaseRepository.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"
#include "Platform/Sqlite.hpp"

namespace Styx {
namespace Storage {

class MemberRepository : public BaseRepository
{
public:
    MemberRepository(Core::Snowflake accountId);

    void saveMember(Core::Snowflake guildId, Core::Snowflake userId, const Discord::Member &member);
    void saveMember(Core::Snowflake guildId, Core::Snowflake userId, const Discord::Member &member,
                    Platform::Database &db);
    void saveMembers(Core::Snowflake guildId, const std::vector<Discord::Member> &members);
    void deleteMembersForGuild(Core::Snowflake guildId, Platform::Database &db);
    void deleteMember(Core::Snowflake guildId, Core::Snowflake userId);

    std::optional<Discord::Member> getMember(Core::Snowflake guildId, Core::Snowflake userId);
    std::vector<Discord::Member> getMembersWithUsers(Core::Snowflake guildId);

private:
    static std::string rolesToJson(const std::vector<Core::Snowflake> &roles);
    static std::vector<Core::Snowflake> rolesFromJson(const std::string &json);
    static Discord::Member readMember(const Platform::Statement &q, int firstColumn);
};

} // namespace Storage
} // namespace Styx
