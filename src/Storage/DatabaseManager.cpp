#include "DatabaseManager.hpp"

#include "Core/SettingsKeys.hpp"
#include "Platform/Settings.hpp"

#include "Core/Logging.hpp"
#include "Platform/Paths.hpp"
#include "Transaction.hpp"

namespace Styx {
namespace Storage {

namespace {
bool columnExists(Platform::Database &db, const std::string &table, const std::string &column)
{
    try {
        auto query = db.prepare("PRAGMA table_info(" + table + ")");
        while (query.step()) {
            if (query.getText(1) == column)
                return true;
        }
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "columnExists check failed for" << table << ":" << e.what();
    }
    return false;
}
} // namespace

DatabaseManager &DatabaseManager::instance()
{
    static DatabaseManager instance;
    return instance;
}

bool DatabaseManager::init()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto configDir = Platform::Paths::configDir();
    Platform::Paths::ensureDirExists(configDir);

    auto persistentPath = configDir / "styx.sqlite";

    try {
        if (!m_persistentDb.open(persistentPath)) {
            Core::log(Core::LogLevel::Error, LogDB) << "Persistent DB init failed to open" << persistentPath.string();
            return false;
        }

        m_persistentDb.execute("PRAGMA journal_mode = WAL");
        m_persistentDb.execute("PRAGMA synchronous = NORMAL");

        setupPersistentTables();
        return true;
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Error, LogDB) << "Persistent DB init exception:" << e.what();
        return false;
    }
}

void DatabaseManager::shutdown()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cacheDbs.clear();
    m_persistentDb.close();
}

std::string DatabaseManager::openCacheDatabase(Core::Snowflake accountId)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::string key = getCacheConnectionName(accountId);

    auto it = m_cacheDbs.find(key);
    if (it != m_cacheDbs.end() && it->second && it->second->isOpen())
        return key;

    auto db = std::make_unique<Platform::Database>();
    bool inMemory = Platform::Settings::instance().getBool(Core::Settings::Keys::InMemoryCache, Core::Settings::Defaults::InMemoryCache);

    try {
        if (inMemory) {
            if (!db->open(":memory:")) {
                Core::log(Core::LogLevel::Error, LogDB) << "Cache DB init failed to open in-memory database";
                return "";
            }
        } else {
            auto dataDir = Platform::Paths::dataDir();
            Platform::Paths::ensureDirExists(dataDir);

            std::string fileName = "cache_" + accountId.toString() + ".sqlite";
            auto dbPath = dataDir / fileName;

            if (!db->open(dbPath)) {
                Core::log(Core::LogLevel::Error, LogDB) << "Cache DB init failed to open" << dbPath.string();
                return "";
            }

            db->execute("PRAGMA journal_mode = WAL");
            db->execute("PRAGMA synchronous = OFF");
        }

        setupCacheTables(*db);
        m_cacheDbs[key] = std::move(db);
        return key;
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Error, LogDB) << "Cache DB init exception:" << e.what();
        return "";
    }
}

void DatabaseManager::closeCacheDatabase(Core::Snowflake accountId)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::string key = getCacheConnectionName(accountId);

    auto it = m_cacheDbs.find(key);
    if (it != m_cacheDbs.end()) {
        if (it->second)
            it->second->close();
        m_cacheDbs.erase(it);
    }
}

std::string DatabaseManager::getCacheConnectionName(Core::Snowflake accountId)
{
    return "Styx_Cache_" + accountId.toString();
}

Platform::Database &DatabaseManager::getPersistentDatabase()
{
    return m_persistentDb;
}

Platform::Database &DatabaseManager::getDatabase(const std::string &connName)
{
    if (connName == PERSISTENT_CONN_NAME)
        return m_persistentDb;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_cacheDbs.find(connName);
    if (it != m_cacheDbs.end() && it->second)
        return *(it->second);

    // Fallback if not found
    return m_persistentDb;
}

void DatabaseManager::setupPersistentTables()
{
    m_persistentDb.execute(R"(
        CREATE TABLE IF NOT EXISTS accounts (
            id INTEGER PRIMARY KEY,
            username TEXT,
            display_name TEXT,
            avatar TEXT,
            gateway_url TEXT,
            rest_url TEXT,
            cdn_url TEXT,
            display_order INTEGER DEFAULT 0,
            auto_connect INTEGER NOT NULL DEFAULT 0,
            proxy_url TEXT
        )
    )");

    if (!columnExists(m_persistentDb, "accounts", "auto_connect"))
        m_persistentDb.execute("ALTER TABLE accounts ADD COLUMN auto_connect INTEGER NOT NULL DEFAULT 0");

    if (!columnExists(m_persistentDb, "accounts", "proxy_url"))
        m_persistentDb.execute("ALTER TABLE accounts ADD COLUMN proxy_url TEXT");

    if (!columnExists(m_persistentDb, "accounts", "heartbeat_session_id")) {
        m_persistentDb.execute("ALTER TABLE accounts ADD COLUMN heartbeat_session_id TEXT");
        m_persistentDb.execute("ALTER TABLE accounts ADD COLUMN heartbeat_session_created_at INTEGER");
        m_persistentDb.execute("ALTER TABLE accounts ADD COLUMN heartbeat_session_last_used_at INTEGER");
    }
}

void DatabaseManager::setupCacheTables(Platform::Database &db)
{
    db.execute(R"(
        CREATE TABLE IF NOT EXISTS "guilds" (
            "id" INTEGER NOT NULL,
            "name" TEXT NOT NULL,
            "icon" TEXT,
            "owner_id" INTEGER NOT NULL,
            "features" TEXT,
            "premium_tier" INTEGER,
            "premium_subscription_count" INTEGER,
            "rules_channel_id" INTEGER,
            "default_message_notifications" INTEGER,
            "description" TEXT,
            "banner" TEXT,
            "splash" TEXT,
            "afk_channel_id" INTEGER,
            "afk_timeout" INTEGER,
            "system_channel_id" INTEGER,
            "system_channel_flags" INTEGER,
            "verification_level" INTEGER,
            "explicit_content_filter" INTEGER,
            "mfa_level" INTEGER,
            "premium_progress_bar_enabled" INTEGER,
            "additional_emoji_slots" INTEGER,
            "additional_sticker_slots" INTEGER,
            "invites_disabled_until" TEXT,
            "dms_disabled_until" TEXT,
            "lockdown_duration_hours" INTEGER,
            PRIMARY KEY("id")
        );
    )");

    db.execute(R"(
        CREATE TABLE IF NOT EXISTS "channels" (
            "id" INTEGER NOT NULL,
            "type" INTEGER NOT NULL,
            "position" INTEGER,
            "name" TEXT,
            "guild_id" INTEGER,
            "parent_id" INTEGER,
            "last_message_id" INTEGER,
            "icon" TEXT,
            "owner_id" INTEGER,
            "rate_limit_per_user" INTEGER,
            "available_tags" TEXT,
            "default_sort_order" INTEGER,
            "flags" INTEGER,
            "topic" TEXT,
            PRIMARY KEY("id")
        );
    )");

    db.execute(R"(
        CREATE TABLE IF NOT EXISTS "messages" (
            "id" INTEGER NOT NULL,
            "author_id" INTEGER NOT NULL,
            "channel_id" INTEGER NOT NULL,
            "content" TEXT NOT NULL,
            "timestamp" TEXT NOT NULL,
            "edited_timestamp" TEXT,
            "type" INTEGER NOT NULL,
            "flags" INTEGER NOT NULL,
            "embeds" TEXT,
            "reactions" TEXT,
            "stickers" TEXT,
            "components" TEXT,
            "poll" TEXT,
            "deleted" INTEGER NOT NULL,
            "referenced_message_id" INTEGER,
            "reference_type" INTEGER,
            "reference_channel_id" INTEGER,
            "reference_guild_id" INTEGER,
            "snapshot" TEXT,
            "context_only" INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY("id")
        );
    )");

    if (!columnExists(db, "messages", "components"))
        db.execute("ALTER TABLE messages ADD COLUMN components TEXT");

    if (!columnExists(db, "messages", "poll"))
        db.execute("ALTER TABLE messages ADD COLUMN poll TEXT");

    db.execute(R"(
        CREATE TABLE IF NOT EXISTS "users" (
            "id" INTEGER NOT NULL,
            "username" TEXT NOT NULL,
            "global_name" TEXT,
            "avatar" TEXT,
            "bot" INTEGER,
            PRIMARY KEY("id")
        );
    )");

    db.execute(R"(
        CREATE TABLE IF NOT EXISTS "members" (
            "user_id" INTEGER NOT NULL,
            "guild_id" INTEGER NOT NULL,
            "nick" TEXT,
            "avatar" TEXT,
            "roles" TEXT,
            "joined_at" TEXT,
            "premium_since" TEXT,
            "deaf" INTEGER,
            "mute" INTEGER,
            "flags" INTEGER,
            "pending" INTEGER,
            "communication_disabled_until" TEXT,
            PRIMARY KEY("user_id", "guild_id")
        );
    )");

    db.execute(R"(
        CREATE TABLE IF NOT EXISTS "attachments" (
            "id" INTEGER NOT NULL,
            "message_id" INTEGER NOT NULL,
            "filename" TEXT NOT NULL,
            "content_type" TEXT,
            "size" INTEGER NOT NULL,
            "url" TEXT NOT NULL,
            "proxy_url" TEXT NOT NULL,
            "width" INTEGER,
            "height" INTEGER,
            "duration_secs" REAL,
            "flags" INTEGER,
            PRIMARY KEY("id")
        );
    )");

    db.execute("CREATE INDEX IF NOT EXISTS idx_attachments_message_id ON attachments(message_id);");

    db.execute(R"(
        CREATE TABLE IF NOT EXISTS "roles" (
            "id" INTEGER NOT NULL,
            "guild_id" INTEGER NOT NULL,
            "name" TEXT NOT NULL,
            "permissions" INTEGER NOT NULL,
            "position" INTEGER NOT NULL,
            "color" INTEGER,
            "hoist" INTEGER,
            "icon" TEXT,
            "unicode_emoji" TEXT,
            "managed" INTEGER,
            "mentionable" INTEGER,
            "colors_primary" INTEGER,
            "colors_secondary" INTEGER,
            "colors_tertiary" INTEGER,
            "flags" INTEGER,
            "tags" TEXT,
            PRIMARY KEY("id", "guild_id"),
            FOREIGN KEY("guild_id") REFERENCES guilds("id") ON DELETE CASCADE
        );
    )");

    db.execute("CREATE INDEX IF NOT EXISTS idx_roles_guild_id ON roles(guild_id);");
    db.execute("CREATE INDEX IF NOT EXISTS idx_roles_position ON roles(guild_id, position);");
    db.execute("CREATE INDEX IF NOT EXISTS idx_roles_lookup ON roles(guild_id, id);");

    db.execute(R"(
        CREATE TABLE IF NOT EXISTS "permission_overwrites" (
            "channel_id" INTEGER NOT NULL,
            "target_id" INTEGER NOT NULL,
            "type" INTEGER NOT NULL,
            "allow" INTEGER NOT NULL,
            "deny" INTEGER NOT NULL,
            PRIMARY KEY("channel_id", "target_id"),
            FOREIGN KEY("channel_id") REFERENCES channels("id") ON DELETE CASCADE
        );
    )");

    db.execute("CREATE INDEX IF NOT EXISTS idx_overwrites_channel_id ON permission_overwrites(channel_id);");
    db.execute("CREATE INDEX IF NOT EXISTS idx_members_lookup ON members(guild_id, user_id);");
    db.execute("CREATE INDEX IF NOT EXISTS idx_channels_guild ON channels(guild_id);");

    db.execute(R"(
        CREATE TABLE IF NOT EXISTS "channel_recipients" (
            "channel_id" INTEGER NOT NULL,
            "user_id" INTEGER NOT NULL,
            PRIMARY KEY("channel_id", "user_id"),
            FOREIGN KEY("channel_id") REFERENCES channels("id") ON DELETE CASCADE,
            FOREIGN KEY("user_id") REFERENCES users("id")
        );
    )");

    db.execute("CREATE INDEX IF NOT EXISTS idx_channel_recipients_channel ON channel_recipients(channel_id);");

    try {
        Transaction txn(db);
        db.execute("DELETE FROM attachments WHERE message_id IN (SELECT id FROM messages WHERE (flags & 64) != 0)");
        db.execute("DELETE FROM messages WHERE (flags & 64) != 0");
        txn.commit();
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "setupCacheTables: Legacy ephemeral cleanup failed:" << e.what();
    }
}

} // namespace Storage
} // namespace Styx
