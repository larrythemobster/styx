#include "MemberRepository.hpp"

#include <QJsonArray>
#include <QJsonDocument>

#include "DatabaseManager.hpp"
#include "Transaction.hpp"
#include "Core/Logging.hpp"

namespace Styx {
namespace Storage {

namespace {

const std::string MemberColumns = "m.nick, m.avatar, m.roles, m.joined_at, m.premium_since, m.deaf, m.mute, "
                                  "m.flags, m.pending, m.communication_disabled_until";
constexpr int MemberColumnCount = 10;

} // namespace

MemberRepository::MemberRepository(Core::Snowflake accountId)
    : BaseRepository(DatabaseManager::getCacheConnectionName(accountId))
{
}

void MemberRepository::deleteMembersForGuild(Core::Snowflake guildId, Platform::Database &db)
{
    try {
        auto q = db.prepare("DELETE FROM members WHERE guild_id = :guild_id");
        q.bind(":guild_id", static_cast<int64_t>(guildId));
        execLogged(q, "MemberRepository: Delete members for guild");
    } catch (const Platform::SqliteError &e) {
        qCWarning(LogDB) << "MemberRepository: Delete members for guild failed:" << e.what();
    }
}

void MemberRepository::deleteMember(Core::Snowflake guildId, Core::Snowflake userId)
{
    auto &db = getDb();
    try {
        auto q = db.prepare("DELETE FROM members WHERE guild_id = :guild_id AND user_id = :user_id");
        q.bind(":guild_id", static_cast<int64_t>(guildId));
        q.bind(":user_id", static_cast<int64_t>(userId));
        execLogged(q, "MemberRepository: Delete member");
    } catch (const Platform::SqliteError &e) {
        qCWarning(LogDB) << "MemberRepository: Delete member failed:" << e.what();
    }
}

QString MemberRepository::rolesToJson(const QList<Core::Snowflake> &roles)
{
    QJsonArray arr;
    for (const auto &role : roles)
        arr.append(QString::number(static_cast<quint64>(role)));

    return QJsonDocument(arr).toJson(QJsonDocument::Compact);
}

QList<Core::Snowflake> MemberRepository::rolesFromJson(const QString &json)
{
    QList<Core::Snowflake> roles;
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isArray())
        return roles;

    for (const auto &val : doc.array())
        roles.append(static_cast<Core::Snowflake>(val.toString().toULongLong()));

    return roles;
}

void MemberRepository::saveMember(Core::Snowflake guildId, Core::Snowflake userId,
                                  const Discord::Member &member)
{
    auto &db = getDb();
    saveMember(guildId, userId, member, db);
}

void MemberRepository::saveMember(Core::Snowflake guildId, Core::Snowflake userId,
                                  const Discord::Member &member, Platform::Database &db)
{
    try {
        auto q = db.prepare(R"(
            INSERT OR REPLACE INTO members
            (user_id, guild_id, nick, avatar, roles, joined_at, premium_since,
             deaf, mute, flags, pending, communication_disabled_until)
            VALUES (:user_id, :guild_id, :nick, :avatar, :roles, :joined_at, :premium_since,
                    :deaf, :mute, :flags, :pending, :communication_disabled_until)
        )");

        q.bind(":user_id", static_cast<int64_t>(userId));
        q.bind(":guild_id", static_cast<int64_t>(guildId));
        bindOptional(q, ":nick", member.nick);
        bindOptional(q, ":avatar", member.avatar);

        if (member.roles.hasValue()) {
            std::string r = rolesToJson(member.roles.get()).toStdString();
            q.bind(":roles", std::string_view(r));
        } else {
            q.bindNull(":roles");
        }

        bindOptional(q, ":joined_at", member.joinedAt);
        bindOptional(q, ":premium_since", member.premiumSince);
        bindOptional(q, ":deaf", member.deaf);
        bindOptional(q, ":mute", member.mute);
        bindOptional(q, ":flags", member.flags);
        bindOptional(q, ":pending", member.pending);
        bindOptional(q, ":communication_disabled_until", member.communicationDisabledUntil);

        execLogged(q, "MemberRepository: Save member");
    } catch (const Platform::SqliteError &e) {
        qCWarning(LogDB) << "MemberRepository: Save member failed:" << e.what();
    }
}

void MemberRepository::saveMembers(Core::Snowflake guildId, const QList<Discord::Member> &members)
{
    if (members.isEmpty())
        return;

    auto &db = getDb();
    Transaction txn(db);
    for (const auto &member : members) {
        if (!member.user.hasValue() || !member.user->id.hasValue())
            continue;
        saveMember(guildId, member.user->id.get(), member, db);
    }
    txn.commit();
}

std::optional<Discord::Member> MemberRepository::getMember(Core::Snowflake guildId,
                                                           Core::Snowflake userId)
{
    auto &db = getDb();
    try {
        std::string sql = "SELECT " + MemberColumns + " FROM members m WHERE m.user_id = :user_id AND m.guild_id = :guild_id";
        auto q = db.prepare(sql);
        q.bind(":user_id", static_cast<int64_t>(userId));
        q.bind(":guild_id", static_cast<int64_t>(guildId));

        if (!q.step()) {
            return std::nullopt;
        }
        return readMember(q, 0);
    } catch (const Platform::SqliteError &e) {
        qCWarning(LogDB) << "MemberRepository: getMember failed:" << e.what();
        return std::nullopt;
    }
}

Discord::Member MemberRepository::readMember(const Platform::Statement &q, int firstColumn)
{
    Discord::Member member;
    if (!q.isNull(firstColumn + 0))
        member.nick = QString::fromStdString(q.getText(firstColumn + 0));
    if (!q.isNull(firstColumn + 1))
        member.avatar = QString::fromStdString(q.getText(firstColumn + 1));
    if (!q.isNull(firstColumn + 2))
        member.roles = rolesFromJson(QString::fromStdString(q.getText(firstColumn + 2)));
    if (!q.isNull(firstColumn + 3))
        member.joinedAt = QDateTime::fromString(QString::fromStdString(q.getText(firstColumn + 3)), Qt::ISODateWithMs);
    if (!q.isNull(firstColumn + 4))
        member.premiumSince = QDateTime::fromString(QString::fromStdString(q.getText(firstColumn + 4)), Qt::ISODateWithMs);
    if (!q.isNull(firstColumn + 5))
        member.deaf = q.getInt(firstColumn + 5) != 0;
    if (!q.isNull(firstColumn + 6))
        member.mute = q.getInt(firstColumn + 6) != 0;
    if (!q.isNull(firstColumn + 7))
        member.flags = q.getInt(firstColumn + 7);
    if (!q.isNull(firstColumn + 8))
        member.pending = q.getInt(firstColumn + 8) != 0;
    if (!q.isNull(firstColumn + 9))
        member.communicationDisabledUntil = QDateTime::fromString(QString::fromStdString(q.getText(firstColumn + 9)), Qt::ISODateWithMs);
    return member;
}

QList<Discord::Member> MemberRepository::getMembersWithUsers(Core::Snowflake guildId)
{
    QList<Discord::Member> members;
    auto &db = getDb();
    try {
        std::string sql = "SELECT m.user_id, " + MemberColumns +
                          ", u.username, u.global_name, u.avatar, u.bot FROM members m JOIN users u ON u.id = m.user_id WHERE m.guild_id = :guild_id";
        auto q = db.prepare(sql);
        q.bind(":guild_id", static_cast<int64_t>(guildId));

        constexpr int FirstUserColumn = 1 + MemberColumnCount;
        while (q.step()) {
            Discord::Member member = readMember(q, 1);
            Discord::User user;
            user.id = static_cast<Core::Snowflake>(q.getInt64(0));
            member.userId = user.id.get();
            user.username = QString::fromStdString(q.getText(FirstUserColumn));
            if (q.isNull(FirstUserColumn + 1))
                user.globalName = nullptr;
            else
                user.globalName = QString::fromStdString(q.getText(FirstUserColumn + 1));
            if (q.isNull(FirstUserColumn + 2))
                user.avatar = nullptr;
            else
                user.avatar = QString::fromStdString(q.getText(FirstUserColumn + 2));
            if (!q.isNull(FirstUserColumn + 3))
                user.bot = q.getInt(FirstUserColumn + 3) != 0;
            member.user = user;
            members.append(member);
        }
    } catch (const Platform::SqliteError &e) {
        qCWarning(LogDB) << "MemberRepository: Get members with users failed:" << e.what();
    }
    return members;
}

} // namespace Storage
} // namespace Styx
