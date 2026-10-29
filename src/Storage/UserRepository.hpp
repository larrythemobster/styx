#pragma once

#include <optional>
#include <vector>

#include "BaseRepository.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"
#include "Platform/Sqlite.hpp"

namespace Styx {
namespace Storage {

class UserRepository : public BaseRepository
{
public:
    UserRepository(Core::Snowflake accountId);

    void saveUser(const Discord::User &user);
    void saveUser(const Discord::User &user, Platform::Database &db);
    void saveUsers(const std::vector<Discord::User> &users);
    void saveUsers(const std::vector<Discord::User> &users, Platform::Database &db);

    std::optional<Discord::User> getUser(Core::Snowflake userId);
};

} // namespace Storage
} // namespace Styx
