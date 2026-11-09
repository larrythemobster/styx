#pragma once

#include <optional>
#include <vector>

#include "BaseRepository.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"
#include "UserRepository.hpp"
#include "Platform/Sqlite.hpp"

namespace Styx {
namespace Storage {

class MessageRepository : public BaseRepository
{
public:
    MessageRepository(Core::Snowflake accountId);

    void saveMessages(const std::vector<Discord::Message> &messages);
    void saveMessages(const std::vector<Discord::Message> &messages, Platform::Database &db);
    void markMessageDeleted(Core::Snowflake messageId);
    void updateMessageContent(const Discord::Message &message);
    void updateReactionsJson(Core::Snowflake messageId, const std::string &reactionsJson);
    std::string getReactionsJson(Core::Snowflake messageId);
    std::optional<Discord::Message> getMessage(Core::Snowflake messageId);

    std::vector<Discord::Message> getMessagesInRange(Core::Snowflake channelId, Core::Snowflake fromId, Core::Snowflake toId);

private:
    void loadAttachmentsForMessages(std::vector<Discord::Message> &messages, Platform::Database &db);
    Discord::Message readMessageFromQuery(const Platform::Statement &q);
    static void purgeStoredMessage(Platform::Database &db, Core::Snowflake messageId);

    UserRepository userRepository;
};

} // namespace Storage
} // namespace Styx
