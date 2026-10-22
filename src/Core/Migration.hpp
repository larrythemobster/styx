#pragma once

namespace Styx {
namespace Core {

class Migration
{
public:
    // Migrates settings, database, and local cache files from Acheron to Styx.
    // Safe to run before QApplication is created.
    static void migrateSettingsAndData();

    // Migrates keychain entries from Acheron to Styx using QKeychain and QEventLoop.
    // Must be called after QApplication is created, before any accounts/tokens are loaded.
    static void migrateKeychain();

    // Returns true if migration has already run or was skipped.
    static bool isMigrationDone();

private:
    static void writeMigrationMarker();
};

} // namespace Core
} // namespace Styx
