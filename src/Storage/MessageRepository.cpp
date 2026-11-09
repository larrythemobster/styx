#include "MessageRepository.hpp"


#include "DatabaseManager.hpp"
#include "Transaction.hpp"
#include "Core/Logging.hpp"
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <utility>

namespace Styx {
namespace Storage {

static void bindComponents(Platform::Statement &stmt, const std::string &componentsJson)
{
    if (!componentsJson.empty())
        stmt.bind(":components", std::string_view(componentsJson));
    else
        stmt.bindNull(":components");
}

static bool isPrivateSnapshot(const Discord::Message &message)
{
    if (!message.snapshotMessage)
        return false;
    if (message.snapshotMessage->isEphemeral())
        return true;
    if (message.snapshotMessage->referencedMessage && message.snapshotMessage->referencedMessage->isEphemeral())
        return true;
    return isPrivateSnapshot(*message.snapshotMessage);
}

static void bindSnapshot(Platform::Statement &stmt, const Discord::Message &message)
{
    if (!message.snapshotJson.empty() && !isPrivateSnapshot(message))
        stmt.bind(":snapshot", std::string_view(message.snapshotJson));
    else
        stmt.bindNull(":snapshot");
}

void MessageRepository::purgeStoredMessage(Platform::Database &db, Core::Snowflake messageId)
{
    auto qAtt = db.prepare("DELETE FROM attachments WHERE message_id = :message_id");
    qAtt.bind(":message_id", static_cast<int64_t>(messageId));
    execLogged(qAtt, "MessageRepository: Purge attachments for private message");

    auto qMsg = db.prepare("DELETE FROM messages WHERE id = :id");
    qMsg.bind(":id", static_cast<int64_t>(messageId));
    execLogged(qMsg, "MessageRepository: Purge private message");
}

MessageRepository::MessageRepository(Core::Snowflake accountId)
    : BaseRepository(DatabaseManager::getCacheConnectionName(accountId)), userRepository(accountId)
{
}

void MessageRepository::saveMessages(const std::vector<Discord::Message> &messages)
{
    auto &db = getDb();
    saveMessages(messages, db);
}

void MessageRepository::saveMessages(const std::vector<Discord::Message> &messages, Platform::Database &db)
{
    if (messages.empty())
        return;

    Transaction txn(db);

    try {
        auto qMsg = db.prepare(R"(
            INSERT OR REPLACE INTO messages
            (id, channel_id, author_id, content, timestamp, edited_timestamp, type, flags, embeds, reactions, stickers, components, deleted,
             referenced_message_id, reference_type, reference_channel_id, reference_guild_id, snapshot, context_only)
            VALUES (:id, :channel_id, :author_id, :content, :timestamp, :edited_timestamp, :type, :flags, :embeds, :reactions, :stickers, :components, 0,
                    :ref_msg_id, :ref_type, :ref_channel_id, :ref_guild_id, :snapshot, 0)
        )");

        auto qAtt = db.prepare(R"(
            INSERT OR REPLACE INTO attachments
            (id, message_id, filename, content_type, size, url, proxy_url, width, height, duration_secs, flags)
            VALUES (:id, :message_id, :filename, :content_type, :size, :url, :proxy_url, :width, :height, :duration_secs, :flags)
        )");

        // Collect referenced messages to save as their own rows
        std::vector<Discord::Message> referencedMessages;

        for (const auto &message : messages) {
            if (message.isEphemeral()) {
                purgeStoredMessage(db, message.id.get());
                continue;
            }

            qMsg.bind(":id", static_cast<int64_t>(message.id.get()));
            qMsg.bind(":channel_id", static_cast<int64_t>(message.channelId.get()));
            qMsg.bind(":author_id", static_cast<int64_t>(message.author->id.get()));
            std::string content = message.content.get();
            qMsg.bind(":content", std::string_view(content));
            std::string ts = message.timestamp.get().iso8601();
            qMsg.bind(":timestamp", std::string_view(ts));

            if (message.editedTimestamp.hasValue()) {
                std::string ets = message.editedTimestamp->iso8601();
                qMsg.bind(":edited_timestamp", std::string_view(ets));
            } else {
                qMsg.bindNull(":edited_timestamp");
            }

            qMsg.bind(":type", static_cast<int64_t>(message.type.get()));
            qMsg.bind(":flags", static_cast<int64_t>(message.flags.get()));

            if (!message.embedsJson.empty()) {
                std::string emb = message.embedsJson;
                qMsg.bind(":embeds", std::string_view(emb));
            } else {
                qMsg.bindNull(":embeds");
            }

            if (!message.reactionsJson.empty()) {
                std::string rct = message.reactionsJson;
                qMsg.bind(":reactions", std::string_view(rct));
            } else {
                qMsg.bindNull(":reactions");
            }

            if (!message.stickersJson.empty()) {
                std::string stk = message.stickersJson;
                qMsg.bind(":stickers", std::string_view(stk));
            } else {
                qMsg.bindNull(":stickers");
            }

            bindComponents(qMsg, message.componentsJson);

            if (message.referencedMessage) {
                qMsg.bind(":ref_msg_id", static_cast<int64_t>(message.referencedMessage->id.get()));
                if (message.referencedMessage->isEphemeral()) {
                    purgeStoredMessage(db, message.referencedMessage->id.get());
                } else {
                    referencedMessages.push_back(*message.referencedMessage);
                }
            } else if (message.messageReference.hasValue() && message.messageReference->messageId.hasValue()) {
                qMsg.bind(":ref_msg_id", static_cast<int64_t>(message.messageReference->messageId.get()));
            } else {
                qMsg.bindNull(":ref_msg_id");
            }

            const Discord::MessageReference *reference = message.messageReference.hasValue() ? &*message.messageReference : nullptr;
            if (reference && reference->type.hasValue())
                qMsg.bind(":ref_type", static_cast<int>(reference->type.get()));
            else
                qMsg.bindNull(":ref_type");

            if (reference && reference->channelId.hasValue())
                qMsg.bind(":ref_channel_id", static_cast<int64_t>(reference->channelId.get()));
            else
                qMsg.bindNull(":ref_channel_id");

            if (reference && reference->guildId.hasValue())
                qMsg.bind(":ref_guild_id", static_cast<int64_t>(reference->guildId.get()));
            else
                qMsg.bindNull(":ref_guild_id");

            bindSnapshot(qMsg, message);

            execLogged(qMsg, "MessageRepository: Save messages");

            userRepository.saveUser(message.author.get(), db);

            if (message.attachments.hasValue()) {
                for (const auto &att : *message.attachments) {
                    qAtt.bind(":id", static_cast<int64_t>(att.id.get()));
                    qAtt.bind(":message_id", static_cast<int64_t>(message.id.get()));
                    std::string fn = att.filename.get();
                    qAtt.bind(":filename", std::string_view(fn));
                    bindOptional(qAtt, ":content_type", att.contentType);
                    qAtt.bind(":size", static_cast<int64_t>(att.size.get()));
                    std::string url = att.url.get();
                    qAtt.bind(":url", std::string_view(url));
                    std::string purl = att.proxyUrl.get();
                    qAtt.bind(":proxy_url", std::string_view(purl));
                    bindOptional(qAtt, ":width", att.width);
                    bindOptional(qAtt, ":height", att.height);
                    bindOptional(qAtt, ":duration_secs", att.durationSecs);

                    if (att.flags.hasValue())
                        qAtt.bind(":flags", static_cast<int>(*att.flags));
                    else
                        qAtt.bindNull(":flags");

                    execLogged(qAtt, "MessageRepository: Save attachment");
                }
            }
        }

        // Save referenced messages as their own rows (INSERT OR IGNORE to not overwrite
        // a potentially more complete version already in the DB)
        if (!referencedMessages.empty()) {
            auto qRef = db.prepare(R"(
                INSERT OR IGNORE INTO messages
                (id, channel_id, author_id, content, timestamp, edited_timestamp, type, flags, embeds, stickers, components, deleted,
                 reference_type, snapshot, context_only)
                VALUES (:id, :channel_id, :author_id, :content, :timestamp, :edited_timestamp, :type, :flags, :embeds, :stickers, :components, 0,
                        :ref_type, :snapshot, 1)
            )");

            for (const auto &ref : referencedMessages) {
                if (ref.isEphemeral()) {
                    purgeStoredMessage(db, ref.id.get());
                    continue;
                }

                qRef.bind(":id", static_cast<int64_t>(ref.id.get()));
                qRef.bind(":channel_id", static_cast<int64_t>(ref.channelId.get()));
                qRef.bind(":author_id", static_cast<int64_t>(ref.author->id.get()));
                std::string content = ref.content.get();
                qRef.bind(":content", std::string_view(content));
                std::string ts = ref.timestamp.get().iso8601();
                qRef.bind(":timestamp", std::string_view(ts));

                if (ref.editedTimestamp.hasValue()) {
                    std::string ets = ref.editedTimestamp->iso8601();
                    qRef.bind(":edited_timestamp", std::string_view(ets));
                } else {
                    qRef.bindNull(":edited_timestamp");
                }

                qRef.bind(":type", static_cast<int64_t>(ref.type.get()));
                qRef.bind(":flags", static_cast<int64_t>(ref.flags.get()));

                if (!ref.embedsJson.empty()) {
                    std::string emb = ref.embedsJson;
                    qRef.bind(":embeds", std::string_view(emb));
                } else {
                    qRef.bindNull(":embeds");
                }

                if (!ref.stickersJson.empty()) {
                    std::string stk = ref.stickersJson;
                    qRef.bind(":stickers", std::string_view(stk));
                } else {
                    qRef.bindNull(":stickers");
                }

                bindComponents(qRef, ref.componentsJson);

                if (ref.messageReference.hasValue() && ref.messageReference->type.hasValue())
                    qRef.bind(":ref_type", static_cast<int>(ref.messageReference->type.get()));
                else
                    qRef.bindNull(":ref_type");

                bindSnapshot(qRef, ref);

                execLogged(qRef, "MessageRepository: Save referenced message");

                userRepository.saveUser(ref.author.get(), db);
            }
        }

        txn.commit();
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "MessageRepository: saveMessages failed:" << e.what();
    }
}

void MessageRepository::markMessageDeleted(Core::Snowflake messageId)
{
    auto &db = getDb();
    try {
        auto q = db.prepare("UPDATE messages SET deleted = 1 WHERE id = :id");
        q.bind(":id", static_cast<int64_t>(messageId));
        execLogged(q, "MessageRepository: Mark message deleted");
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "MessageRepository: Mark message deleted failed:" << e.what();
    }
}

void MessageRepository::updateMessageContent(const Discord::Message &message)
{
    auto &db = getDb();
    if (message.isEphemeral()) {
        Transaction txn(db);
        try {
            purgeStoredMessage(db, message.id.get());
            txn.commit();
        } catch (const Platform::SqliteError &e) {
            Core::log(Core::LogLevel::Warning, LogDB) << "MessageRepository: Purge private message failed:" << e.what();
        }
        return;
    }

    try {
        auto q = db.prepare(R"(
            UPDATE messages
            SET content = :content, edited_timestamp = :edited_timestamp, embeds = :embeds, flags = :flags, snapshot = :snapshot, components = :components
            WHERE id = :id
        )");

        std::string content = message.content.get();
        q.bind(":content", std::string_view(content));

        if (message.editedTimestamp.hasValue()) {
            std::string ets = message.editedTimestamp->iso8601();
            q.bind(":edited_timestamp", std::string_view(ets));
        } else {
            q.bindNull(":edited_timestamp");
        }

        if (!message.embedsJson.empty()) {
            std::string emb = message.embedsJson;
            q.bind(":embeds", std::string_view(emb));
        } else {
            q.bindNull(":embeds");
        }

        q.bind(":flags", static_cast<int64_t>(message.flags.get()));

        bindSnapshot(q, message);

        bindComponents(q, message.componentsJson);

        q.bind(":id", static_cast<int64_t>(message.id.get()));

        execLogged(q, "MessageRepository: Update message content");
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "MessageRepository: Update message content failed:" << e.what();
    }
}

void MessageRepository::updateReactionsJson(Core::Snowflake messageId, const std::string &reactionsJson)
{
    auto &db = getDb();
    try {
        auto q = db.prepare("UPDATE messages SET reactions = :reactions WHERE id = :id");
        if (!reactionsJson.empty()) {
            std::string rct = reactionsJson;
            q.bind(":reactions", std::string_view(rct));
        } else {
            q.bindNull(":reactions");
        }
        q.bind(":id", static_cast<int64_t>(messageId));

        execLogged(q, "MessageRepository: Update reactions");
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "MessageRepository: Update reactions failed:" << e.what();
    }
}

std::string MessageRepository::getReactionsJson(Core::Snowflake messageId)
{
    auto &db = getDb();
    try {
        auto q = db.prepare("SELECT reactions FROM messages WHERE id = :id");
        q.bind(":id", static_cast<int64_t>(messageId));

        if (!q.step())
            return {};

        return q.getText(0);
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "MessageRepository: getReactionsJson failed:" << e.what();
        return {};
    }
}

std::optional<Discord::Message> MessageRepository::getMessage(Core::Snowflake messageId)
{
    auto &db = getDb();
    try {
        auto q = db.prepare(R"(
            SELECT m.id, m.channel_id, m.author_id, m.content, m.timestamp, m.edited_timestamp, m.type, m.flags, m.embeds, m.reactions,
                   u.id, u.username, u.global_name, u.avatar, u.bot,
                   m.referenced_message_id,
                   rm.id, rm.channel_id, rm.author_id, rm.content, rm.timestamp, rm.edited_timestamp, rm.type, rm.flags, rm.embeds,
                   ru.id, ru.username, ru.global_name, ru.avatar, ru.bot,
                   m.reference_type, m.reference_channel_id, m.reference_guild_id, m.snapshot, rm.reference_type, rm.snapshot,
                   m.stickers, rm.stickers,
                   m.components, rm.components
            FROM messages m
            INNER JOIN users u ON m.author_id = u.id
            LEFT JOIN messages rm ON m.referenced_message_id = rm.id
            LEFT JOIN users ru ON rm.author_id = ru.id
            WHERE m.id = :id
        )");
        q.bind(":id", static_cast<int64_t>(messageId));

        if (!q.step())
            return std::nullopt;

        std::vector<Discord::Message> single{ readMessageFromQuery(q) };
        loadAttachmentsForMessages(single, db);
        return single.front();
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "MessageRepository: getMessage failed:" << e.what();
        return std::nullopt;
    }
}

std::vector<Discord::Message> MessageRepository::getMessagesInRange(Core::Snowflake channelId, Core::Snowflake fromId, Core::Snowflake toId)
{
    auto &db = getDb();

    std::vector<Discord::Message> messages;
    try {
        auto q = db.prepare(R"(
            SELECT m.id, m.channel_id, m.author_id, m.content, m.timestamp, m.edited_timestamp, m.type, m.flags, m.embeds, m.reactions,
                   u.id, u.username, u.global_name, u.avatar, u.bot,
                   m.referenced_message_id,
                   rm.id, rm.channel_id, rm.author_id, rm.content, rm.timestamp, rm.edited_timestamp, rm.type, rm.flags, rm.embeds,
                   ru.id, ru.username, ru.global_name, ru.avatar, ru.bot,
                   m.reference_type, m.reference_channel_id, m.reference_guild_id, m.snapshot, rm.reference_type, rm.snapshot,
                   m.stickers, rm.stickers,
                   m.components, rm.components
            FROM messages m
            INNER JOIN users u ON m.author_id = u.id
            LEFT JOIN messages rm ON m.referenced_message_id = rm.id
            LEFT JOIN users ru ON rm.author_id = ru.id
            WHERE m.channel_id = :channel_id AND m.id >= :from_id AND m.id <= :to_id AND m.deleted = 0 AND m.context_only = 0
            ORDER BY m.id ASC
        )");

        q.bind(":channel_id", static_cast<int64_t>(channelId));
        q.bind(":from_id", static_cast<int64_t>(fromId));
        q.bind(":to_id", static_cast<int64_t>(toId));

        while (q.step()) {
            Discord::Message message = readMessageFromQuery(q);
            messages.push_back(message);
        }

        loadAttachmentsForMessages(messages, db);
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "MessageRepository: getMessagesInRange failed:" << e.what();
    }

    return messages;
}

template <typename Entity>
static std::vector<Entity> parseEntityArray(const std::string &json)
{
    std::vector<Entity> entities;
    auto doc = nlohmann::json::parse(json, nullptr, false);
    if (doc.is_array()) {
        for (const auto &val : doc)
            entities.push_back(Entity::fromJson(val.is_object() ? val : nlohmann::json::object()));
    }
    return entities;
}

static void readEmbeds(Discord::Message &message, const std::string &embedsJson)
{
    if (embedsJson.empty())
        return;
    message.embedsJson = embedsJson;
    message.embeds = parseEntityArray<Discord::Embed>(embedsJson);
}

static void readReactions(Discord::Message &message, const std::string &reactionsJson)
{
    if (reactionsJson.empty())
        return;
    message.reactionsJson = reactionsJson;
    message.reactions = parseEntityArray<Discord::Reaction>(reactionsJson);
}

static void readStickers(Discord::Message &message, const std::string &stickersJson)
{
    if (stickersJson.empty())
        return;
    message.stickersJson = stickersJson;
    message.stickerItems = parseEntityArray<Discord::StickerItem>(stickersJson);
}

static void readComponents(Discord::Message &message, std::string componentsJson)
{
    if (componentsJson.empty())
        return;
    message.componentsJson = std::move(componentsJson);
    message.components = parseEntityArray<Discord::MessageComponent>(message.componentsJson);
}

Discord::Message MessageRepository::readMessageFromQuery(const Platform::Statement &q)
{
    // Columns 0-8: m.id, m.channel_id, m.author_id, m.content, m.timestamp, m.edited_timestamp, m.type, m.flags, m.embeds
    // Column 9: m.reactions
    // Columns 10-14: u.id, u.username, u.global_name, u.avatar, u.bot
    // Column 15: m.referenced_message_id
    // Columns 16-24: rm.id, rm.channel_id, rm.author_id, rm.content, rm.timestamp, rm.edited_timestamp, rm.type, rm.flags, rm.embeds
    // Columns 25-29: ru.id, ru.username, ru.global_name, ru.avatar, ru.bot
    // Columns 30-35: m.reference_type, m.reference_channel_id, m.reference_guild_id, m.snapshot, rm.reference_type, rm.snapshot
    // Columns 36-37: m.stickers, rm.stickers
    // Columns 38-39: m.components, rm.components

    Discord::Message message;
    message.id = static_cast<Core::Snowflake>(q.getInt64(0));
    message.channelId = static_cast<Core::Snowflake>(q.getInt64(1));
    message.content = q.getText(3);
    message.timestamp = Platform::DateTime::parse(q.getText(4));
    if (!q.isNull(5))
        message.editedTimestamp = Platform::DateTime::parse(q.getText(5));
    message.type = static_cast<Discord::MessageType>(q.getInt64(6));
    message.flags = static_cast<Discord::MessageFlags>(static_cast<int>(q.getInt64(7)));

    if (!q.isNull(8))
        readEmbeds(message, q.getText(8));
    if (!q.isNull(9))
        readReactions(message, q.getText(9));
    if (!q.isNull(36))
        readStickers(message, q.getText(36));
    if (!q.isNull(38))
        readComponents(message, q.getText(38));

    message.author->id = static_cast<Core::Snowflake>(q.getInt64(10));
    message.author->username = q.getText(11);
    if (!q.isNull(12))
        message.author->globalName = q.getText(12);
    if (!q.isNull(13))
        message.author->avatar = q.getText(13);
    if (!q.isNull(14))
        message.author->bot = q.getInt(14) != 0;

    if (!q.isNull(33)) {
        std::string snapshotJson = q.getText(33);
        if (!snapshotJson.empty()) {
            auto snapshot = nlohmann::json::parse(snapshotJson, nullptr, false);
            message.setSnapshot(snapshot.is_object() ? snapshot : nlohmann::json::object());
        }
    }

    // Load referenced message from the self-join
    if (!q.isNull(15)) {
        Discord::MessageReference ref;
        ref.messageId = static_cast<Core::Snowflake>(q.getInt64(15));
        if (!q.isNull(30))
            ref.type = static_cast<Discord::MessageReferenceType>(q.getInt(30));
        ref.channelId = q.isNull(31)
                                ? message.channelId.get()
                                : static_cast<Core::Snowflake>(q.getInt64(31));
        if (!q.isNull(32))
            ref.guildId = static_cast<Core::Snowflake>(q.getInt64(32));
        message.messageReference = ref;

        // If the joined message row exists (rm.id is not null), reconstruct it
        if (!q.isNull(16)) {
            auto refMsg = std::make_shared<Discord::Message>();
            refMsg->id = static_cast<Core::Snowflake>(q.getInt64(16));
            refMsg->channelId = static_cast<Core::Snowflake>(q.getInt64(17));
            refMsg->content = q.getText(19);
            refMsg->timestamp = Platform::DateTime::parse(q.getText(20));
            if (!q.isNull(21))
                refMsg->editedTimestamp = Platform::DateTime::parse(q.getText(21));
            refMsg->type = static_cast<Discord::MessageType>(q.getInt64(22));
            refMsg->flags = static_cast<Discord::MessageFlags>(static_cast<int>(q.getInt64(23)));

            if (!q.isNull(24))
                readEmbeds(*refMsg, q.getText(24));
            if (!q.isNull(37))
                readStickers(*refMsg, q.getText(37));
            if (!q.isNull(39))
                readComponents(*refMsg, q.getText(39));

            if (!q.isNull(25)) {
                refMsg->author->id = static_cast<Core::Snowflake>(q.getInt64(25));
                refMsg->author->username = q.getText(26);
                if (!q.isNull(27))
                    refMsg->author->globalName = q.getText(27);
                if (!q.isNull(28))
                    refMsg->author->avatar = q.getText(28);
                if (!q.isNull(29))
                    refMsg->author->bot = q.getInt(29) != 0;
            }

            if (!q.isNull(34)) {
                Discord::MessageReference refRef;
                refRef.type = static_cast<Discord::MessageReferenceType>(q.getInt(34));
                refMsg->messageReference = refRef;
            }

            if (!q.isNull(35)) {
                std::string refSnapshotJson = q.getText(35);
                if (!refSnapshotJson.empty()) {
                    auto snapshot = nlohmann::json::parse(refSnapshotJson, nullptr, false);
                    refMsg->setSnapshot(snapshot.is_object() ? snapshot : nlohmann::json::object());
                }
            }

            message.referencedMessage = refMsg;
        }
    }

    return message;
}

void MessageRepository::loadAttachmentsForMessages(std::vector<Discord::Message> &messages,
                                                   Platform::Database &db)
{
    if (messages.empty())
        return;

    std::unordered_map<int64_t, int> messageIndexMap;
    for (int i = 0; i < messages.size(); ++i) {
        messageIndexMap.emplace(static_cast<int64_t>(messages[i].id.get()), i);
    }

    std::string placeholders;
    for (int i = 0; i < messages.size(); ++i) {
        if (i > 0)
            placeholders += ",";
        placeholders += "?";
    }

    try {
        std::string query = "SELECT id, message_id, filename, content_type, size, url, proxy_url, width, height, duration_secs, flags FROM attachments WHERE message_id IN (" + placeholders + ")";
        auto q = db.prepare(query);

        for (int i = 0; i < messages.size(); ++i) {
            q.bind(i + 1, static_cast<int64_t>(messages[i].id.get()));
        }

        while (q.step()) {
            int64_t messageId = q.getInt64(1);
            auto it = messageIndexMap.find(messageId);
            if (it == messageIndexMap.end())
                continue;
            int idx = it->second;

            Discord::Attachment att;
            att.id = static_cast<Core::Snowflake>(q.getInt64(0));
            att.filename = q.getText(2);
            if (!q.isNull(3))
                att.contentType = q.getText(3);
            att.size = static_cast<uint64_t>(q.getInt64(4));
            att.url = q.getText(5);
            att.proxyUrl = q.getText(6);
            if (!q.isNull(7))
                att.width = q.getInt(7);
            if (!q.isNull(8))
                att.height = q.getInt(8);
            if (!q.isNull(9))
                att.durationSecs = q.getDouble(9);
            if (!q.isNull(10))
                att.flags = static_cast<Discord::AttachmentFlags>(q.getInt(10));

            if (!messages[idx].attachments.hasValue())
                messages[idx].attachments = std::vector<Discord::Attachment>();
            messages[idx].attachments->push_back(att);
        }
    } catch (const Platform::SqliteError &e) {
        Core::log(Core::LogLevel::Warning, LogDB) << "MessageRepository: Load attachments failed:" << e.what();
    }
}

} // namespace Storage
} // namespace Styx
