#include "UserRepository.hpp"

#include "DatabaseManager.hpp"
#include "Transaction.hpp"
#include "Core/Logging.hpp"

namespace Styx {
namespace Storage {

UserRepository::UserRepository(Core::Snowflake accountId)
    : BaseRepository(DatabaseManager::getCacheConnectionName(accountId))
{
}

void UserRepository::saveUser(const Discord::User &user)
{
    auto &db = getDb();
    saveUser(user, db);
}

void UserRepository::saveUser(const Discord::User &user, Platform::Database &db)
{
    try {
        auto q = db.prepare(R"(
            INSERT OR REPLACE INTO users
            (id, username, global_name, avatar, bot)
            VALUES (:id, :username, :global_name, :avatar, :bot)
        )");

        q.bind(":id", static_cast<int64_t>(user.id.get()));
        bindOptional(q, ":username", user.username);
        bindOptional(q, ":global_name", user.globalName);
        bindOptional(q, ":avatar", user.avatar);
        bindOptional(q, ":bot", user.bot);

        execLogged(q, "UserRepository: Save user");
    } catch (const Platform::SqliteError &e) {
        qCWarning(LogDB) << "UserRepository: Save user failed:" << e.what();
    }
}

void UserRepository::saveUsers(const QList<Discord::User> &users)
{
    auto &db = getDb();
    saveUsers(users, db);
}

void UserRepository::saveUsers(const QList<Discord::User> &users, Platform::Database &db)
{
    if (users.isEmpty())
        return;

    Transaction txn(db);
    for (const auto &user : users)
        saveUser(user, db);

    txn.commit();
}

std::optional<Discord::User> UserRepository::getUser(Core::Snowflake userId)
{
    auto &db = getDb();
    try {
        auto q = db.prepare(R"(
            SELECT id, username, global_name, avatar, bot
            FROM users WHERE id = :id
        )");
        q.bind(":id", static_cast<int64_t>(userId));

        if (!q.step())
            return std::nullopt;

        Discord::User user;
        user.id = static_cast<Core::Snowflake>(q.getInt64(0));
        user.username = QString::fromStdString(q.getText(1));
        if (!q.isNull(2))
            user.globalName = QString::fromStdString(q.getText(2));
        if (!q.isNull(3))
            user.avatar = QString::fromStdString(q.getText(3));
        if (!q.isNull(4))
            user.bot = q.getInt(4) != 0;

        return user;
    } catch (const Platform::SqliteError &e) {
        qCWarning(LogDB) << "UserRepository: getUser failed:" << e.what();
        return std::nullopt;
    }
}

} // namespace Storage
} // namespace Styx
