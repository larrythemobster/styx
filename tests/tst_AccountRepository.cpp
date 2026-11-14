#include <catch2/catch_test_macros.hpp>

#include "Core/AccountInfo.hpp"
#include "Platform/Native.hpp"
#include "Platform/Paths.hpp"
#include "Platform/SecureStore.hpp"
#include "Platform/Sqlite.hpp"
#include "Storage/AccountRepository.hpp"
#include "Storage/DatabaseManager.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

using namespace Styx;
namespace {
class Sandbox {
public:
    Sandbox() : root(std::filesystem::temp_directory_path() / ("styx-account-test-" + Platform::randomUuid()))
    {
        std::filesystem::create_directories(root);
        for (const char *name : {"APPDATA", "HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME"}) {
            const char *value = std::getenv(name);
            previous.emplace_back(name, value ? std::optional<std::string>(value) : std::nullopt);
#ifdef _WIN32
            _putenv_s(name, root.string().c_str());
#else
            setenv(name, root.string().c_str(), 1);
#endif
        }
    }
    ~Sandbox()
    {
        auto &db = Storage::DatabaseManager::instance().getPersistentDatabase();
        if (db.isOpen()) {
            try {
                auto rows = db.prepare("SELECT proxy_credential_key FROM accounts");
                while (rows.step()) {
                    auto key = rows.getText(0);
                    if (!key.empty()) Platform::SecureStore::deletePassword("Styx", key);
                }
            } catch (...) {}
        }
        Storage::DatabaseManager::instance().shutdown();
        for (const auto &[name, value] : previous) {
#ifdef _WIN32
            _putenv_s(name.c_str(), value ? value->c_str() : "");
#else
            if (value) setenv(name.c_str(), value->c_str(), 1); else unsetenv(name.c_str());
#endif
        }
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    std::filesystem::path databasePath() const { return Platform::Paths::configDir() / "styx.sqlite"; }
    std::filesystem::path root;
    std::vector<std::pair<std::string, std::optional<std::string>>> previous;
};

std::string keyFor(std::uint64_t id)
{
    auto &db = Storage::DatabaseManager::instance().getPersistentDatabase();
    auto query = db.prepare("SELECT proxy_credential_key FROM accounts WHERE id = :id");
    query.bind(":id", static_cast<int64_t>(id));
    REQUIRE(query.step());
    return query.getText(0);
}

std::string bytes(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
}

TEST_CASE("Proxy credentials are stored separately and updates are atomic", "[account-repository]")
{
    Sandbox sandbox;
    REQUIRE(Storage::DatabaseManager::instance().init());
    Storage::AccountRepository repo;
    Core::AccountInfo account;
    account.id = Core::Snowflake(713001);
    account.proxy = *Core::ProxyConfig::parse("http://user-secret:password-secret@localhost:8080");
    REQUIRE(repo.saveAccount(account).success());
    auto first = keyFor(713001);
    REQUIRE_FALSE(first.empty());
    REQUIRE(repo.getAccount(713001).proxy == account.proxy);
    REQUIRE(repo.saveAccount(account).success());
    REQUIRE(keyFor(713001) == first);

    auto changed = *Core::ProxyConfig::parse("socks5://other-user:other-password@localhost:1080");
    auto &db = Storage::DatabaseManager::instance().getPersistentDatabase();
    db.execute("CREATE TRIGGER reject_proxy BEFORE UPDATE OF proxy_url ON accounts BEGIN SELECT RAISE(ABORT, 'rejected'); END");
    REQUIRE_FALSE(repo.updateProxy(713001, changed).success());
    REQUIRE(keyFor(713001) == first);
    REQUIRE(repo.getAccount(713001).proxy == account.proxy);
    db.execute("DROP TRIGGER reject_proxy");
    REQUIRE(repo.updateProxy(713001, changed).success());
    auto second = keyFor(713001);
    REQUIRE(second != first);
    REQUIRE_FALSE(Platform::SecureStore::readPassword("Styx", first).has_value());
    REQUIRE(repo.getAccount(713001).proxy == changed);
    REQUIRE(Platform::SecureStore::deletePassword("Styx", second));
    REQUIRE_FALSE(repo.getAccount(713001).proxyError.empty());
    REQUIRE(repo.updateProxy(713001, changed).success());
    auto third = keyFor(713001);
    REQUIRE(third != second);
    repo.removeAccount(713001);
    REQUIRE_FALSE(Platform::SecureStore::readPassword("Styx", third).has_value());
    for (const auto &path : {sandbox.databasePath(), std::filesystem::path(sandbox.databasePath().string() + "-wal")}) {
        auto data = bytes(path);
        REQUIRE(data.find("user-secret") == std::string::npos);
        REQUIRE(data.find("password-secret") == std::string::npos);
        REQUIRE(data.find("other-password") == std::string::npos);
    }
}

TEST_CASE("Legacy proxy credentials are migrated and scrubbed", "[account-repository]")
{
    Sandbox sandbox;
    std::filesystem::create_directories(Platform::Paths::configDir());
    {
        Platform::Database db;
        REQUIRE(db.open(sandbox.databasePath()));
        db.execute("CREATE TABLE accounts (id INTEGER PRIMARY KEY, username TEXT, display_name TEXT, avatar TEXT, gateway_url TEXT, rest_url TEXT, cdn_url TEXT, display_order INTEGER, auto_connect INTEGER, proxy_url TEXT)");
        db.execute("INSERT INTO accounts (id, proxy_url) VALUES (713002, 'http://legacy-user:legacy-password@localhost:8080')");
    }
    REQUIRE(Storage::DatabaseManager::instance().init());
    Storage::AccountRepository repo;
    auto account = repo.getAccount(713002);
    REQUIRE(account.proxy.username == "legacy-user");
    REQUIRE(account.proxy.password == "legacy-password");
    REQUIRE_FALSE(keyFor(713002).empty());
    for (const auto &path : {sandbox.databasePath(), std::filesystem::path(sandbox.databasePath().string() + "-wal")}) {
        auto data = bytes(path);
        REQUIRE(data.find("legacy-user") == std::string::npos);
        REQUIRE(data.find("legacy-password") == std::string::npos);
    }
}
