#include "BaseRepository.hpp"

namespace Styx {
namespace Storage {

BaseRepository::BaseRepository(const QString &connName) : connName(connName) {}

Platform::Database &BaseRepository::getDb() const
{
    return DatabaseManager::instance().getDatabase(connName);
}

bool BaseRepository::execLogged(Platform::Statement &stmt, const char *context)
{
    try {
        stmt.execute();
        return true;
    } catch (const Platform::SqliteError &e) {
        qCWarning(LogDB) << context << "failed:" << e.what();
        return false;
    }
}

} // namespace Storage
} // namespace Styx
