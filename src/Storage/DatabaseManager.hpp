#pragma once

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QDir>
#include <QDebug>

#include "Core/Snowflake.hpp"

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

    QString openCacheDatabase(Core::Snowflake accountId);
    void closeCacheDatabase(Core::Snowflake accountId);
    static QString getCacheConnectionName(Core::Snowflake accountId);

private:
    void setupPersistentTables();
    void setupCacheTables(const QString &connName);

    QSqlDatabase persistentDb;
    QString persistentPath;
};
} // namespace Storage
} // namespace Styx