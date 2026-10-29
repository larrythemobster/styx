#include "BaseRepository.hpp"

namespace Styx {
namespace Storage {

BaseRepository::BaseRepository(const std::string &connName) : connName(connName) {}

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
        Core::log(Core::LogLevel::Warning, LogDB) << context << "failed:" << e.what();
        return false;
    }
}

} // namespace Storage
} // namespace Styx
