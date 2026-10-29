#pragma once

namespace Styx {
namespace Core {

class Migration
{
public:
    // Migrates settings, database, and local cache files from Acheron to Styx.
    static void migrateSettingsAndData();

    // Migrates keychain entries before accounts and tokens are loaded.
    static void migrateKeychain();

    // Returns true if migration has already run or was skipped.
    static bool isMigrationDone();

private:
    static void writeMigrationMarker();
};

} // namespace Core
} // namespace Styx
