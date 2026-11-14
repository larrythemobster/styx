#include "AccountRepository.hpp"

#include "Core/Logging.hpp"
#include "Core/TokenStore.hpp"
#include "Platform/Sqlite.hpp"
#include "Platform/SecureStore.hpp"
#include "Platform/Native.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace Styx {
namespace Storage {

namespace {
constexpr auto service = "Styx";
constexpr auto proxyError = "Proxy credentials are unavailable. Edit the proxy to enter them again.";

void loadProxy(Core::AccountInfo &acc, const std::string &stored, const std::string &key)
{
    auto parsed = Core::ProxyConfig::parse(stored);
    if (!parsed || !parsed->username.empty() || !parsed->password.empty()) {
        acc.proxyError = "Stored proxy configuration is invalid. Edit the proxy to repair it.";
        return;
    }
    acc.proxy = *parsed;
    if (key.empty()) return;
    auto secret = Platform::SecureStore::readPassword(service, key);
    auto json = secret ? nlohmann::json::parse(*secret, nullptr, false) : nlohmann::json();
    if (!json.is_object() || !json.contains("username") || !json["username"].is_string() ||
        !json.contains("password") || !json["password"].is_string()) {
        acc.proxyError = proxyError;
        return;
    }
    acc.proxy.username = json["username"].get<std::string>();
    acc.proxy.password = json["password"].get<std::string>();
}

struct Credential {
    std::string oldKey;
    std::string key;
    bool created = false;
};

Credential prepareCredential(Platform::Database &db, std::uint64_t id, const Core::ProxyConfig &proxy)
{
    Credential result;
    auto query = db.prepare("SELECT proxy_credential_key FROM accounts WHERE id = :id");
    query.bind(":id", static_cast<int64_t>(id));
    if (query.step()) result.oldKey = query.getText(0);
    if (proxy.username.empty() && proxy.password.empty()) return result;
    auto secret = nlohmann::json{{"username", proxy.username}, {"password", proxy.password}}.dump();
    if (!result.oldKey.empty()) {
        auto old = Platform::SecureStore::readPassword(service, result.oldKey);
        if (old && *old == secret) {
            result.key = result.oldKey;
            return result;
        }
    }
    result.key = "proxy_" + Platform::randomUuid();
    if (!Platform::SecureStore::writePassword(service, result.key, secret))
        throw std::runtime_error("Failed to save proxy credentials in the secure store.");
    result.created = true;
    return result;
}

void discardNew(const Credential &credential)
{
    if (credential.created) Platform::SecureStore::deletePassword(service, credential.key);
}

void discardOld(const Credential &credential)
{
    if (!credential.oldKey.empty() && credential.oldKey != credential.key)
        Platform::SecureStore::deletePassword(service, credential.oldKey);
}

std::string endpoint(const Core::ProxyConfig &proxy)
{
    return proxy.toDisplayString();
}
} // namespace

bool AccountRepository::migrateLegacyProxies(Platform::Database &db)
{
    std::vector<Credential> credentials;
    try {
        db.execute("BEGIN IMMEDIATE");
        auto rows = db.prepare("SELECT id, proxy_url FROM accounts WHERE proxy_credential_key IS NULL OR proxy_credential_key = ''");
        while (rows.step()) {
            const auto id = static_cast<std::uint64_t>(rows.getInt64(0));
            auto proxy = Core::ProxyConfig::parse(rows.getText(1));
            if (!proxy) throw std::runtime_error("Malformed legacy proxy configuration.");
            if (proxy->username.empty() && proxy->password.empty()) continue;
            credentials.push_back(prepareCredential(db, id, *proxy));
            auto update = db.prepare("UPDATE accounts SET proxy_url = :url, proxy_credential_key = :key WHERE id = :id");
            auto url = endpoint(*proxy);
            update.bind(":url", std::string_view(url));
            update.bind(":key", std::string_view(credentials.back().key));
            update.bind(":id", static_cast<int64_t>(id));
            update.execute();
        }
        db.execute("COMMIT");
        return true;
    } catch (const std::exception &e) {
        try { db.execute("ROLLBACK"); } catch (...) {}
        for (const auto &credential : credentials) discardNew(credential);
        Core::log(Core::LogLevel::Error, LogDB) << "Proxy migration failed:" << e.what();
        return false;
    }
}

Core::Result<void> AccountRepository::saveAccount(const Core::AccountInfo &acc)
{
    auto &db = DatabaseManager::instance().getPersistentDatabase();
    if (!db.isOpen()) {
        Core::log(Core::LogLevel::Warning, LogDB) << "AccountRepository: Persistent DB not open!";
        return Core::Result<void>::makeError("Account database is not open.");
    }

    if (!acc.proxyError.empty()) return Core::Result<void>::makeError(acc.proxyError);
    Credential credential;
    try {
        credential = prepareCredential(db, static_cast<std::uint64_t>(acc.id), acc.proxy);
        auto query = db.prepare(R"(
            INSERT INTO accounts
            (id, username, display_name, avatar, gateway_url, rest_url, cdn_url, display_order, auto_connect, proxy_url, proxy_credential_key)
            VALUES (:id, :username, :display_name, :avatar, :gateway_url, :rest_url, :cdn_url, :display_order, :auto_connect, :proxy_url, :proxy_credential_key)
            ON CONFLICT(id) DO UPDATE SET
                username = excluded.username,
                display_name = excluded.display_name,
                avatar = excluded.avatar,
                gateway_url = excluded.gateway_url,
                rest_url = excluded.rest_url,
                cdn_url = excluded.cdn_url,
                display_order = excluded.display_order,
                auto_connect = excluded.auto_connect,
                proxy_url = excluded.proxy_url,
                proxy_credential_key = excluded.proxy_credential_key
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
        std::string pr = endpoint(acc.proxy);
        query.bind(":proxy_url", std::string_view(pr));
        query.bind(":proxy_credential_key", std::string_view(credential.key));

        query.execute();
        discardOld(credential);
        return Core::Result<void>::makeOk();
    } catch (const std::exception &e) {
        discardNew(credential);
        Core::log(Core::LogLevel::Warning, LogDB) << "AccountRepository: Save failed:" << e.what();
        return Core::Result<void>::makeError("Failed to save account or proxy credentials.");
    }
}

Core::AccountInfo AccountRepository::getAccount(std::uint64_t id)
{
    auto &db = DatabaseManager::instance().getPersistentDatabase();
    if (!db.isOpen())
        return {};

    try {
        auto query = db.prepare(R"(
            SELECT id, username, display_name, avatar, gateway_url, rest_url, cdn_url, display_order, auto_connect, proxy_url, proxy_credential_key
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
        loadProxy(acc, query.getText(9), query.getText(10));

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
            SELECT id, username, display_name, avatar, gateway_url, rest_url, cdn_url, display_order, auto_connect, proxy_url, proxy_credential_key
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
            loadProxy(acc, query.getText(9), query.getText(10));

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
    auto &db = DatabaseManager::instance().getPersistentDatabase();
    try {
        auto current = db.prepare("SELECT proxy_credential_key FROM accounts WHERE id = :id");
        current.bind(":id", static_cast<int64_t>(id));
        std::string key = current.step() ? current.getText(0) : std::string();
        auto query = db.prepare("DELETE FROM accounts WHERE id = :id");
        query.bind(":id", static_cast<int64_t>(id));
        query.execute();
        if (!key.empty()) Platform::SecureStore::deletePassword(service, key);
        Core::TokenStore::deleteToken(Core::Snowflake(id));
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

Core::Result<void> AccountRepository::updateProxy(std::uint64_t id, const Core::ProxyConfig &proxy)
{
    auto &db = DatabaseManager::instance().getPersistentDatabase();
    Credential credential;
    try {
        auto exists = db.prepare("SELECT 1 FROM accounts WHERE id = :id");
        exists.bind(":id", static_cast<int64_t>(id));
        if (!exists.step()) return Core::Result<void>::makeError("Account not found.");
        credential = prepareCredential(db, id, proxy);
        auto query = db.prepare("UPDATE accounts SET proxy_url = :proxy_url, proxy_credential_key = :key WHERE id = :id");
        std::string pr = endpoint(proxy);
        query.bind(":proxy_url", std::string_view(pr));
        query.bind(":key", std::string_view(credential.key));
        query.bind(":id", static_cast<int64_t>(id));
        query.execute();
        discardOld(credential);
        return Core::Result<void>::makeOk();
    } catch (const std::exception &e) {
        discardNew(credential);
        Core::log(Core::LogLevel::Warning, LogDB) << "AccountRepository: Update proxy failed:" << e.what();
        return Core::Result<void>::makeError("Failed to save proxy credentials or account settings.");
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
