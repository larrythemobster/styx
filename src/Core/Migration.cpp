#include "Migration.hpp"
#include "Logging.hpp"
#include "Platform/Native.hpp"
#include "Platform/Paths.hpp"
#include "Platform/SecureStore.hpp"
#include "Platform/Settings.hpp"
#include "Platform/Sqlite.hpp"

#include <filesystem>
#include <fstream>
#include <regex>

namespace Styx::Core {
namespace {
namespace fs = std::filesystem;

fs::path oldPath(const fs::path &newPath)
{
    return newPath.parent_path().parent_path() / "ouwou" / "Acheron";
}

std::string renamed(std::string value)
{
    value = std::regex_replace(value, std::regex("acheron", std::regex::icase), "styx");
    return std::regex_replace(value, std::regex("ouwou", std::regex::icase), "larrythemobster");
}

void renameValues(nlohmann::json &value)
{
    if (value.is_string())
        value = renamed(value.get<std::string>());
    else if (value.is_array())
        for (auto &entry : value)
            renameValues(entry);
}

bool copyDirectoryRecursively(const fs::path &source, const fs::path &target)
{
    std::error_code error;
    fs::create_directories(target, error);
    if (error) return false;
    for (fs::recursive_directory_iterator it(source, error), end; !error && it != end; it.increment(error)) {
        const auto destination = target / it->path().lexically_relative(source);
        if (it->is_directory(error))
            fs::create_directories(destination, error);
        else if (it->is_regular_file(error) && !fs::exists(destination, error))
            fs::copy_file(it->path(), destination, fs::copy_options::none, error);
        if (error) return false;
    }
    return !error;
}
}

bool Migration::isMigrationDone()
{
    return fs::exists(Platform::Paths::dataDir() / ".migration_done");
}

void Migration::writeMigrationMarker()
{
    const auto dir = Platform::Paths::dataDir();
    fs::create_directories(dir);
    std::ofstream marker(dir / ".migration_done");
    if (marker) marker << "Migrated on " << Platform::DateTime::now().iso8601() << '\n';
}

void Migration::migrateSettingsAndData()
{
    if (isMigrationDone()) return;
    const auto newRoam = Platform::Paths::dataDir();
    const auto oldRoam = oldPath(newRoam);
    const auto newLocal = Platform::Paths::cacheDir();
    const auto oldLocal = oldPath(newLocal);
    auto oldSettings = Platform::Settings::importFromRegistry(L"Software\\ouwou\\Acheron");
    if (!fs::exists(oldRoam) && !fs::exists(oldLocal) && oldSettings.empty()) {
        writeMigrationMarker();
        return;
    }
    Core::log(Core::LogLevel::Info, LogCore) << "Migrating Acheron data to Styx...";
    auto &settings = Platform::Settings::instance();
    for (auto &[key, value] : oldSettings) {
        renameValues(value);
        settings.setJson(renamed(key), value);
    }
    if (!oldSettings.empty()) settings.save();
    if (fs::exists(oldRoam)) {
        fs::create_directories(newRoam);
        for (const auto &entry : fs::directory_iterator(oldRoam)) {
            if (!entry.is_regular_file()) continue;
            auto name = entry.path().filename().string();
            auto destination = name;
            if (name == "acheron.sqlite" || name == "acheron.sqlite-wal" || name == "acheron.sqlite-shm" || name == "acheron.log" || name.starts_with("acheron.log."))
                destination.replace(0, 7, "styx");
            std::error_code error;
            fs::copy_file(entry.path(), newRoam / destination, fs::copy_options::skip_existing, error);
            if (!error) Core::log(Core::LogLevel::Info, LogCore) << "Migrated AppData file: " << name << " -> " << destination;
        }
    }
    if (fs::exists(oldLocal) && copyDirectoryRecursively(oldLocal, newLocal))
        Core::log(Core::LogLevel::Info, LogCore) << "Migrated LocalAppData cache directory.";
}

void Migration::migrateKeychain()
{
    if (isMigrationDone()) return;
    const auto dbPath = Platform::Paths::dataDir() / "styx.sqlite";
    if (fs::exists(dbPath)) {
        try {
            Platform::Database db(dbPath);
            auto query = db.prepare("SELECT id FROM accounts");
            while (query.step()) {
                auto id = static_cast<std::uint64_t>(query.getInt64(0));
                auto key = "account_" + std::to_string(id) + "_token";
                auto token = Platform::SecureStore::readPassword("Acheron", key);
                if (token && !token->empty()) {
                    if (Platform::SecureStore::writePassword("Styx", key, *token))
                        Core::log(Core::LogLevel::Info, LogCore) << "Migrated keychain token for account " << id;
                    else
                        Core::log(Core::LogLevel::Warning, LogCore) << "Failed to store migrated keychain token for account " << id;
                }
            }
        } catch (const Platform::SqliteError &error) {
            Core::log(Core::LogLevel::Warning, LogCore) << "Migration DB query failed: " << error.what();
        }
    }
    writeMigrationMarker();
    Core::log(Core::LogLevel::Info, LogCore) << "Migration from Acheron completed.";
}

} // namespace Styx::Core
