#include "Transaction.hpp"

#include "Core/Logging.hpp"

#include <unordered_set>

namespace Styx {
namespace Storage {

namespace {
thread_local std::unordered_set<void *> activeTransactions;
} // namespace

Transaction::Transaction(Platform::Database &db)
    : db(db),
      dbHandle(db.rawHandle())
{
    if (activeTransactions.contains(dbHandle))
        return;

    try {
        db.execute("BEGIN IMMEDIATE");
        activeTransactions.insert(dbHandle);
        owns = true;
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "Failed to begin transaction:" << e.what();
    }
}

Transaction::~Transaction()
{
    if (!owns || finished)
        return;

    try {
        db.execute("ROLLBACK");
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "Failed to rollback transaction:" << e.what();
    }
    activeTransactions.erase(dbHandle);
}

bool Transaction::commit()
{
    if (!owns || finished)
        return true;

    finished = true;
    activeTransactions.erase(dbHandle);

    try {
        db.execute("COMMIT");
        return true;
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "Failed to commit transaction:" << e.what();
        try {
            db.execute("ROLLBACK");
        } catch (...) {
        }
        return false;
    }
}

} // namespace Storage
} // namespace Styx
