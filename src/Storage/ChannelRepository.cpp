#include "ChannelRepository.hpp"

#include "DatabaseManager.hpp"
#include "Core/Logging.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace Styx {
namespace Storage {

static Discord::Channel readChannelFromQuery(const Platform::Statement &q)
{
    Discord::Channel channel;
    channel.id = static_cast<Core::Snowflake>(q.getInt64(0));
    channel.type = static_cast<Discord::ChannelType>(q.getInt(1));
    if (!q.isNull(2))
        channel.position = q.getInt(2);
    if (!q.isNull(3))
        channel.name = QString::fromStdString(q.getText(3));
    if (!q.isNull(4))
        channel.guildId = static_cast<Core::Snowflake>(q.getInt64(4));
    if (!q.isNull(5))
        channel.parentId = static_cast<Core::Snowflake>(q.getInt64(5));
    if (!q.isNull(6))
        channel.lastMessageId = static_cast<Core::Snowflake>(q.getInt64(6));
    if (!q.isNull(7))
        channel.icon = QString::fromStdString(q.getText(7));
    if (!q.isNull(8))
        channel.ownerId = static_cast<Core::Snowflake>(q.getInt64(8));
    if (!q.isNull(9))
        channel.rateLimitPerUser = q.getInt(9);
    if (!q.isNull(10)) {
        channel.availableTagsJson = QString::fromStdString(q.getText(10));
        QJsonDocument doc = QJsonDocument::fromJson(channel.availableTagsJson.toUtf8());
        if (doc.isArray()) {
            QList<Discord::ForumTag> tags;
            for (const QJsonValue &val : doc.array())
                tags.append(Discord::ForumTag::fromJson(val.toObject()));
            channel.availableTags = tags;
        }
    }
    if (!q.isNull(11))
        channel.defaultSortOrder = q.getInt(11);
    if (!q.isNull(12))
        channel.flags = Discord::ChannelFlags::fromInt(q.getInt(12));
    if (!q.isNull(13))
        channel.topic = QString::fromStdString(q.getText(13));
    return channel;
}

static Discord::PermissionOverwrite readOverwrite(const Platform::Statement &q, int base)
{
    Discord::PermissionOverwrite ow;
    ow.id = static_cast<Core::Snowflake>(q.getInt64(base));
    ow.type = static_cast<Discord::PermissionOverwrite::Type>(q.getInt(base + 1));
    ow.allow = Discord::Permissions::fromInt(q.getInt64(base + 2));
    ow.deny = Discord::Permissions::fromInt(q.getInt64(base + 3));
    return ow;
}

ChannelRepository::ChannelRepository(Core::Snowflake accountId)
    : BaseRepository(DatabaseManager::getCacheConnectionName(accountId))
{
}

void ChannelRepository::saveChannel(const Discord::Channel &channel)
{
    auto &db = getDb();
    saveChannel(channel, db);
}

void ChannelRepository::saveChannel(const Discord::Channel &channel, Platform::Database &db)
{
    try {
        auto q = db.prepare(R"(
            INSERT OR REPLACE INTO channels
            (id, type, position, name, guild_id, parent_id, last_message_id, icon, owner_id, rate_limit_per_user, available_tags, default_sort_order, flags, topic)
            VALUES (:id, :type, :position, :name, :guild_id, :parent_id, :last_message_id, :icon, :owner_id, :rate_limit_per_user, :available_tags, :default_sort_order, :flags, :topic)
        )");

        q.bind(":id", static_cast<int64_t>(channel.id.get()));
        q.bind(":type", static_cast<int64_t>(channel.type.get()));
        q.bind(":position", static_cast<int64_t>(channel.position.get()));
        bindOptional(q, ":name", channel.name);
        bindOptional(q, ":guild_id", channel.guildId);
        bindOptional(q, ":parent_id", channel.parentId);
        bindOptional(q, ":last_message_id", channel.lastMessageId);
        bindOptional(q, ":icon", channel.icon);
        bindOptional(q, ":owner_id", channel.ownerId);
        bindOptional(q, ":rate_limit_per_user", channel.rateLimitPerUser);

        if (!channel.availableTagsJson.isEmpty()) {
            std::string tags = channel.availableTagsJson.toStdString();
            q.bind(":available_tags", std::string_view(tags));
        } else {
            q.bindNull(":available_tags");
        }

        bindOptional(q, ":default_sort_order", channel.defaultSortOrder);

        if (channel.flags.hasValue())
            q.bind(":flags", static_cast<int64_t>(channel.flags.get()));
        else
            q.bindNull(":flags");

        bindOptional(q, ":topic", channel.topic);

        execLogged(q, "ChannelRepository: Save");
    } catch (const Platform::SqliteError &e) {
        qCWarning(LogDB) << "ChannelRepository: Save failed:" << e.what();
    }
}

void ChannelRepository::deleteChannel(Core::Snowflake channelId)
{
    auto &db = getDb();
    deleteChannel(channelId, db);
}

void ChannelRepository::deleteChannel(Core::Snowflake channelId, Platform::Database &db)
{
    try {
        auto q1 = db.prepare("DELETE FROM permission_overwrites WHERE channel_id = :channel_id");
        q1.bind(":channel_id", static_cast<int64_t>(channelId));
        execLogged(q1, "ChannelRepository: Delete overwrites");

        auto q2 = db.prepare("DELETE FROM channel_recipients WHERE channel_id = :channel_id");
        q2.bind(":channel_id", static_cast<int64_t>(channelId));
        execLogged(q2, "ChannelRepository: Delete recipients");

        auto q3 = db.prepare("DELETE FROM channels WHERE id = :id");
        q3.bind(":id", static_cast<int64_t>(channelId));
        execLogged(q3, "ChannelRepository: Delete channel");
    } catch (const Platform::SqliteError &e) {
        qCWarning(LogDB) << "ChannelRepository: Delete failed:" << e.what();
    }
}

void ChannelRepository::savePermissionOverwrites(
        Core::Snowflake channelId, const QList<Discord::PermissionOverwrite> &overwrites,
        Platform::Database &db)
{
    try {
        auto delQ = db.prepare("DELETE FROM permission_overwrites WHERE channel_id = :channel_id");
        delQ.bind(":channel_id", static_cast<int64_t>(channelId));
        execLogged(delQ, "ChannelRepository: Delete overwrites");

        if (overwrites.isEmpty())
            return;

        auto q = db.prepare(R"(
            INSERT INTO permission_overwrites
            (channel_id, target_id, type, allow, deny)
            VALUES (:channel_id, :target_id, :type, :allow, :deny)
        )");

        for (const auto &ow : overwrites) {
            q.bind(":channel_id", static_cast<int64_t>(channelId));
            q.bind(":target_id", static_cast<int64_t>(ow.id.get()));
            q.bind(":type", static_cast<int>(ow.type.get()));
            q.bind(":allow", static_cast<int64_t>(ow.allow.get()));
            q.bind(":deny", static_cast<int64_t>(ow.deny.get()));

            execLogged(q, "ChannelRepository: Save overwrite");
        }
    } catch (const Platform::SqliteError &e) {
        qCWarning(LogDB) << "ChannelRepository: savePermissionOverwrites failed:" << e.what();
    }
}

void ChannelRepository::saveChannelRecipients(Core::Snowflake channelId,
                                              const QList<Core::Snowflake> &recipientIds,
                                              Platform::Database &db)
{
    try {
        auto delQ = db.prepare("DELETE FROM channel_recipients WHERE channel_id = :channel_id");
        delQ.bind(":channel_id", static_cast<int64_t>(channelId));
        execLogged(delQ, "ChannelRepository: Delete recipients");

        if (recipientIds.isEmpty())
            return;

        auto q = db.prepare(R"(
            INSERT INTO channel_recipients
            (channel_id, user_id)
            VALUES (:channel_id, :user_id)
        )");

        for (const auto &userId : recipientIds) {
            q.bind(":channel_id", static_cast<int64_t>(channelId));
            q.bind(":user_id", static_cast<int64_t>(userId));

            execLogged(q, "ChannelRepository: Save recipient");
        }
    } catch (const Platform::SqliteError &e) {
        qCWarning(LogDB) << "ChannelRepository: saveChannelRecipients failed:" << e.what();
    }
}

QList<Discord::PermissionOverwrite> ChannelRepository::getPermissionOverwrites(
        Core::Snowflake channelId)
{
    QList<Discord::PermissionOverwrite> overwrites;
    auto &db = getDb();
    try {
        auto q = db.prepare(R"(
            SELECT target_id, type, allow, deny
            FROM permission_overwrites WHERE channel_id = :channel_id
        )");
        q.bind(":channel_id", static_cast<int64_t>(channelId));

        while (q.step())
            overwrites.append(readOverwrite(q, 0));
    } catch (const Platform::SqliteError &e) {
        qCWarning(LogDB) << "ChannelRepository: getPermissionOverwrites failed:" << e.what();
    }

    return overwrites;
}

QList<Core::Snowflake> ChannelRepository::getChannelRecipientIds(Core::Snowflake channelId)
{
    QList<Core::Snowflake> recipientIds;
    auto &db = getDb();
    try {
        auto q = db.prepare(R"(
            SELECT user_id
            FROM channel_recipients WHERE channel_id = :channel_id
        )");
        q.bind(":channel_id", static_cast<int64_t>(channelId));

        while (q.step())
            recipientIds.append(static_cast<Core::Snowflake>(q.getInt64(0)));
    } catch (const Platform::SqliteError &e) {
        qCWarning(LogDB) << "ChannelRepository: getChannelRecipientIds failed:" << e.what();
    }

    return recipientIds;
}

QHash<Core::Snowflake, QList<Discord::PermissionOverwrite>> ChannelRepository::
        getPermissionOverwritesForGuild(Core::Snowflake guildId)
{
    QHash<Core::Snowflake, QList<Discord::PermissionOverwrite>> result;
    auto &db = getDb();

    try {
        auto channelIdsQuery = db.prepare("SELECT id FROM channels WHERE guild_id = :guild_id");
        channelIdsQuery.bind(":guild_id", static_cast<int64_t>(guildId));

        QList<Core::Snowflake> channelIds;
        while (channelIdsQuery.step())
            channelIds.append(static_cast<Core::Snowflake>(channelIdsQuery.getInt64(0)));

        if (channelIds.isEmpty())
            return result;

        std::string placeholders;
        for (int i = 0; i < channelIds.size(); ++i) {
            if (i > 0)
                placeholders += ",";
            placeholders += "?";
        }

        std::string sql = "SELECT channel_id, target_id, type, allow, deny FROM permission_overwrites WHERE channel_id IN (" + placeholders + ")";
        auto q = db.prepare(sql);

        for (int i = 0; i < channelIds.size(); ++i)
            q.bind(i + 1, static_cast<int64_t>(channelIds[i]));

        while (q.step()) {
            Core::Snowflake channelId = static_cast<Core::Snowflake>(q.getInt64(0));
            result[channelId].append(readOverwrite(q, 1));
        }
    } catch (const Platform::SqliteError &e) {
        qCWarning(LogDB) << "ChannelRepository: getPermissionOverwritesForGuild failed:" << e.what();
    }

    return result;
}

std::optional<Discord::Channel> ChannelRepository::getChannel(Core::Snowflake channelId)
{
    auto &db = getDb();
    try {
        auto q = db.prepare(R"(
            SELECT id, type, position, name, guild_id, parent_id, last_message_id, icon, owner_id, rate_limit_per_user, available_tags, default_sort_order, flags, topic
            FROM channels WHERE id = :id
        )");
        q.bind(":id", static_cast<int64_t>(channelId));

        if (!q.step())
            return std::nullopt;

        Discord::Channel channel = readChannelFromQuery(q);

        channel.permissionOverwrites = getPermissionOverwrites(channelId);

        if (channel.type == Discord::ChannelType::DM || channel.type == Discord::ChannelType::GROUP_DM)
            channel.recipientIds = getChannelRecipientIds(channelId);

        return channel;
    } catch (const Platform::SqliteError &e) {
        qCWarning(LogDB) << "ChannelRepository: getChannel failed:" << e.what();
        return std::nullopt;
    }
}

std::optional<Core::Snowflake>
ChannelRepository::findDmChannelWithUser(Core::Snowflake userId)
{
    auto &db = getDb();
    try {
        auto q = db.prepare(R"(
            SELECT c.id
            FROM channels c
            JOIN channel_recipients cr ON cr.channel_id = c.id
            WHERE c.type = 1 AND cr.user_id = :user_id
            LIMIT 1
        )");
        q.bind(":user_id", static_cast<int64_t>(userId));

        if (!q.step())
            return std::nullopt;

        return static_cast<Core::Snowflake>(q.getInt64(0));
    } catch (const Platform::SqliteError &e) {
        qCWarning(LogDB) << "ChannelRepository: findDmChannelWithUser failed:" << e.what();
        return std::nullopt;
    }
}

QList<Discord::Channel> ChannelRepository::getChannelsForGuild(Core::Snowflake guildId)
{
    QList<Discord::Channel> channels;
    auto &db = getDb();

    try {
        auto q = db.prepare(R"(
            SELECT id, type, position, name, guild_id, parent_id, last_message_id, icon, owner_id, rate_limit_per_user, available_tags, default_sort_order, flags, topic
            FROM channels WHERE guild_id = :guild_id
        )");
        q.bind(":guild_id", static_cast<int64_t>(guildId));

        while (q.step()) {
            channels.append(readChannelFromQuery(q));
        }
    } catch (const Platform::SqliteError &e) {
        qCWarning(LogDB) << "ChannelRepository: Get channels for guild failed:" << e.what();
    }

    return channels;
}

} // namespace Storage
} // namespace Styx
