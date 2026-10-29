#include "AccountRepository.hpp"

#include "Core/Logging.hpp"
#include "Core/TokenStore.hpp"
#include "Platform/Sqlite.hpp"

namespace Styx {
namespace Storage {

namespace {
Core::ProxyConfig parseProxy(const std::string &stored, Core::Snowflake accountId)
{
    std::optional<Core::ProxyConfig> parsed = Core::ProxyConfig::parse(stored);
    if (!parsed) {
        Core::log(Core::LogLevel::Warning, LogDB) << "AccountRepository: Ignoring malformed proxy for account" << accountId.toString();
        return {};
    }

    return *parsed;
}
} // namespace

void AccountRepository::saveAccount(const Core::AccountInfo &acc)
{
    auto &db = DatabaseManager::instance().getPersistentDatabase();
    if (!db.isOpen()) {
        Core::log(Core::LogLevel::Warning, LogDB) << "AccountRepository: Persistent DB not open!";
        return;
    }

    try {
        auto query = db.prepare(R"(
            INSERT INTO accounts
            (id, username, display_name, avatar, gateway_url, rest_url, cdn_url, display_order, auto_connect, proxy_url)
            VALUES (:id, :username, :display_name, :avatar, :gateway_url, :rest_url, :cdn_url, :display_order, :auto_connect, :proxy_url)
            ON CONFLICT(id) DO UPDATE SET
                username = excluded.username,
                display_name = excluded.display_name,
                avatar = excluded.avatar,
                gateway_url = excluded.gateway_url,
                rest_url = excluded.rest_url,
                cdn_url = excluded.cdn_url,
                display_order = excluded.display_order,
                auto_connect = excluded.auto_connect,
                proxy_url = excluded.proxy_url
        )");

        query.bind(":id", static_cast<int64_t>(acc.id));
        query.bind(":username", std::string_view(acc.username));
        query.bind(":display_name", std::string_view(acc.displayName));
        query.bind(":avatar", std::string_view(acc.avatar));
        query.bind(":gateway_url", std::string_view(acc.gatewayUrl));
        query.bind(":rest_url", std::string_view(acc.restUrl));
        query.bind(":cdn_url", std::string_view(acc.cdnUrl));
        query.bind(":display_order", acc.displayOrder);
        query.bind(":auto_connect", acc.autoConnect ? 1 : 0);
        std::string pr = acc.proxy.toString();
        query.bind(":proxy_url", std::string_view(pr));

        query.execute();
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "AccountRepository: Save failed:" << e.what();
    }
}

Core::AccountInfo AccountRepository::getAccount(std::uint64_t id)
{
    auto &db = DatabaseManager::instance().getPersistentDatabase();
    if (!db.isOpen())
        return {};

    try {
        auto query = db.prepare(R"(
            SELECT id, username, display_name, avatar, gateway_url, rest_url, cdn_url, display_order, auto_connect, proxy_url
            FROM accounts WHERE id = :id
        )");

        query.bind(":id", static_cast<int64_t>(id));

        if (!query.step()) {
            Core::log(Core::LogLevel::Warning, LogDB) << "AccountRepository: Account not found";
            return {};
        }

        Core::AccountInfo acc;
        acc.id = static_cast<Core::Snowflake>(query.getInt64(0));
        acc.username = query.getText(1);
        acc.displayName = query.getText(2);
        acc.avatar = query.getText(3);
        acc.gatewayUrl = query.getText(4);
        acc.restUrl = query.getText(5);
        acc.cdnUrl = query.getText(6);
        acc.displayOrder = query.getInt(7);
        acc.autoConnect = query.getInt(8) != 0;
        acc.proxy = parseProxy(query.getText(9), acc.id);

        return acc;
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "AccountRepository: Get failed:" << e.what();
        return {};
    }
}

std::vector<Core::AccountInfo> AccountRepository::getAllAccounts()
{
    std::vector<Core::AccountInfo> results;
    auto &db = DatabaseManager::instance().getPersistentDatabase();

    if (!db.isOpen())
        return results;

    try {
        auto query = db.prepare(R"(
            SELECT id, username, display_name, avatar, gateway_url, rest_url, cdn_url, display_order, auto_connect, proxy_url
            FROM accounts ORDER BY display_order ASC
        )");

        while (query.step()) {
            Core::AccountInfo acc;

            acc.id = static_cast<std::uint64_t>(query.getInt64(0));
            acc.username = query.getText(1);
            acc.displayName = query.getText(2);
            acc.avatar = query.getText(3);
            acc.gatewayUrl = query.getText(4);
            acc.restUrl = query.getText(5);
            acc.cdnUrl = query.getText(6);
            acc.displayOrder = query.getInt(7);
            acc.autoConnect = query.getInt(8) != 0;
            acc.proxy = parseProxy(query.getText(9), acc.id);

            acc.state = Core::ConnectionState::Disconnected;

            results.push_back(acc);
        }
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "AccountRepository: getAllAccounts failed:" << e.what();
    }

    return results;
}

void AccountRepository::removeAccount(std::uint64_t id)
{
    Core::TokenStore::deleteToken(Core::Snowflake(id));

    auto &db = DatabaseManager::instance().getPersistentDatabase();
    try {
        auto query = db.prepare("DELETE FROM accounts WHERE id = :id");
        query.bind(":id", static_cast<int64_t>(id));
        query.execute();
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "AccountRepository: Remove failed:" << e.what();
    }
}

void AccountRepository::updateDisplayOrder(std::uint64_t id, int order)
{
    auto &db = DatabaseManager::instance().getPersistentDatabase();
    try {
        auto query = db.prepare("UPDATE accounts SET display_order = :order WHERE id = :id");
        query.bind(":order", order);
        query.bind(":id", static_cast<int64_t>(id));
        query.execute();
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "AccountRepository: Update display order failed:" << e.what();
    }
}

void AccountRepository::updateProxy(std::uint64_t id, const Core::ProxyConfig &proxy)
{
    auto &db = DatabaseManager::instance().getPersistentDatabase();
    try {
        auto query = db.prepare("UPDATE accounts SET proxy_url = :proxy_url WHERE id = :id");
        std::string pr = proxy.toString();
        query.bind(":proxy_url", std::string_view(pr));
        query.bind(":id", static_cast<int64_t>(id));
        query.execute();
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "AccountRepository: Update proxy failed:" << e.what();
    }
}

std::optional<Discord::HeartbeatSession> AccountRepository::getHeartbeatSession(std::uint64_t id)
{
    auto &db = DatabaseManager::instance().getPersistentDatabase();
    try {
        auto query = db.prepare(R"(
            SELECT heartbeat_session_id, heartbeat_session_created_at, heartbeat_session_last_used_at
            FROM accounts WHERE id = :id
        )");
        query.bind(":id", static_cast<int64_t>(id));

        if (!query.step() || query.isNull(0))
            return std::nullopt;

        Discord::HeartbeatSession session;
        session.id = query.getText(0);
        session.createdAtMs = query.getInt64(1);
        session.lastUsedAtMs = query.getInt64(2);
        return session;
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "AccountRepository: Get heartbeat session failed:" << e.what();
        return std::nullopt;
    }
}

void AccountRepository::updateHeartbeatSession(std::uint64_t id, const Discord::HeartbeatSession &session)
{
    auto &db = DatabaseManager::instance().getPersistentDatabase();
    try {
        auto query = db.prepare(R"(
            UPDATE accounts SET
                heartbeat_session_id = :session_id,
                heartbeat_session_created_at = :created_at,
                heartbeat_session_last_used_at = :last_used_at
            WHERE id = :id
        )");
        std::string sid = session.id;
        query.bind(":session_id", std::string_view(sid));
        query.bind(":created_at", session.createdAtMs);
        query.bind(":last_used_at", session.lastUsedAtMs);
        query.bind(":id", static_cast<int64_t>(id));
        query.execute();
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "AccountRepository: Update heartbeat session failed:" << e.what();
    }
}

void AccountRepository::clearHeartbeatSession(std::uint64_t id)
{
    auto &db = DatabaseManager::instance().getPersistentDatabase();
    try {
        auto query = db.prepare(R"(
            UPDATE accounts SET
                heartbeat_session_id = NULL,
                heartbeat_session_created_at = NULL,
                heartbeat_session_last_used_at = NULL
            WHERE id = :id
        )");
        query.bind(":id", static_cast<int64_t>(id));
        query.execute();
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "AccountRepository: Clear heartbeat session failed:" << e.what();
    }
}

void AccountRepository::updateAutoConnect(std::uint64_t id, bool autoConnect)
{
    auto &db = DatabaseManager::instance().getPersistentDatabase();
    try {
        auto query = db.prepare("UPDATE accounts SET auto_connect = :auto_connect WHERE id = :id");
        query.bind(":auto_connect", autoConnect ? 1 : 0);
        query.bind(":id", static_cast<int64_t>(id));
        query.execute();
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "AccountRepository: Update auto_connect failed:" << e.what();
    }
}

} // namespace Storage
} // namespace Styx
