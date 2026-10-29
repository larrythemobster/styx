#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "Core/Snowflake.hpp"
#include "Platform/Sqlite.hpp"

namespace Styx {
namespace Storage {

class DatabaseManager
{
public:
    static constexpr char const *PERSISTENT_CONN_NAME = "Styx_Persistent";
    static constexpr char const *CACHE_CONN_NAME = "Styx_Cache";

    static DatabaseManager &instance();

    bool init();
    void shutdown();

    std::string openCacheDatabase(Core::Snowflake accountId);
    void closeCacheDatabase(Core::Snowflake accountId);
    static std::string getCacheConnectionName(Core::Snowflake accountId);

    // Get Database reference by connection name
    Platform::Database &getDatabase(const std::string &connName);
    Platform::Database &getPersistentDatabase();

private:
    DatabaseManager() = default;
    ~DatabaseManager() = default;

    DatabaseManager(const DatabaseManager &) = delete;
    DatabaseManager &operator=(const DatabaseManager &) = delete;

    void setupPersistentTables();
    void setupCacheTables(Platform::Database &db);

    std::mutex m_mutex;
    Platform::Database m_persistentDb;
    std::unordered_map<std::string, std::unique_ptr<Platform::Database>> m_cacheDbs;
};

} // namespace Storage
} // namespace Styx
