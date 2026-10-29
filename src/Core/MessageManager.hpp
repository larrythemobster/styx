#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <cstdint>
#include <list>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "Platform/Trackable.hpp"

#include "MessageReactors.hpp"
#include "MessageSegments.hpp"
#include "PendingAttachment.hpp"
#include "Snowflake.hpp"
#include "Storage/MessageRepository.hpp"
#include "Discord/Entities.hpp"
#include "Discord/Events.hpp"
#include "Discord/Client.hpp"
#include "Markdown/Parser.hpp"

namespace Styx {
namespace Core {

class UserManager;
class EmojiManager;

struct MessageRequestResult
{
    // not using Result cuz i want to see the type and id even if it failed
    bool success;
    Discord::Client::MessageLoadType type;
    Snowflake channelId;
    std::vector<Discord::Message> messages;
    bool reachedLatest = false;
    Snowflake anchorId = Snowflake::Invalid;
};

class MessageManager : public Platform::Trackable
{
public:
    explicit MessageManager(Snowflake accountId, Discord::Client *client,
                            UserManager *userManager);
    ~MessageManager() override;

    void setChannelResolver(std::function<std::string(Snowflake)> resolver);
    void setChannelLinkResolver(Markdown::ChannelLinkResolverFn resolver);
    void setEmojiManager(EmojiManager *manager);

    [[nodiscard]] std::string channelTopicHtml(const std::string &topic, Snowflake channelId) const;
    [[nodiscard]] std::string profileBioHtml(const std::string &bio) const;

    void requestLoadChannel(Snowflake channelId);
    void requestLoadHistory(Snowflake channelId, Snowflake beforeId);
    void requestLoadFuture(Snowflake channelId, Snowflake afterId);
    void requestLoadAround(Snowflake channelId, Snowflake messageId);
    void sendMessage(Snowflake channelId, const std::string &content,
                     Snowflake replyToMessageId = Snowflake::Invalid,
                     const std::vector<PendingAttachment> &attachments = {});
    void cancelSend(Snowflake channelId, const std::string &nonce);

    using ReactionLocation = Discord::Client::ReactionLocation;
    void addReaction(Snowflake channelId, Snowflake messageId, const Discord::Emoji &emoji, bool isBurst, ReactionLocation location);
    void removeReaction(Snowflake channelId, Snowflake messageId, const Discord::Emoji &emoji, bool isBurst, ReactionLocation location);
    void removeReactionOf(Snowflake reactorId, Snowflake channelId, const ReactionRef &reaction);

    [[nodiscard]] MessageReactors *reactors() const { return reactorStore.get(); }
    [[nodiscard]] std::vector<Discord::Reaction> reactionsOf(Snowflake messageId);

    enum class ReactionRejection {
        TooManyReactions,
        AlreadyReactedWithOtherType,
    };

    Platform::Signal<ReactionRejection> reactionRejected;
    Platform::Signal<Snowflake> reactionsChanged;
    Platform::Signal<const MessageRequestResult &> messagesReceived;
    Platform::Signal<const std::string &> messageErrored;
    Platform::Signal<Snowflake, Snowflake> messageDeleted;
    Platform::Signal<Snowflake, const std::vector<Snowflake> &> membersLearnedFromMessage;
    Platform::Signal<const std::string &, int, std::int64_t, std::int64_t> attachmentUploadProgress;

    void onMessageCreated(const Discord::Message &message);
    void onMessageUpdated(const Discord::Message &message);
    void onMessageDeleted(const Discord::MessageDelete &event);
    void onMessageSendFailed(const std::string &nonce, const std::string &error);
    void onReactionAdd(const Discord::MessageReactionAdd &event);
    void onReactionAddMany(const Discord::MessageReactionAddMany &event);
    void onReactionRemove(const Discord::MessageReactionRemove &event);
    void onReactionRemoveAll(const Discord::MessageReactionRemoveAll &event);
    void onReactionRemoveEmoji(const Discord::MessageReactionRemoveEmoji &event);

private:
    using LoadType = Discord::Client::MessageLoadType;
    static constexpr int PageSize = 30;
    static constexpr int JumpWindow = 30;

    using PageFetcher = std::function<void(Discord::Client::MessagesCallback)>;
    void fetchPage(LoadType type, Snowflake channelId, Snowflake anchorId, const PageFetcher &fetch);
    void onApiMessagesReceived(const std::vector<Discord::Message> &messages, LoadType type,
                               Snowflake channelId, Snowflake anchorId = Snowflake::Invalid);
    [[nodiscard]] std::optional<std::vector<Discord::Message>> cachedSlice(Snowflake channelId,
                                                                     const MessageSegments::Run &run,
                                                                     int from, int to);
    void emitReactionUpdate(Discord::Message &msg);

    struct ReactionChange
    {
        Snowflake channelId;
        Snowflake messageId;
        Discord::Emoji emoji;
        bool isBurst;
        ReactionLocation location;
        std::optional<Snowflake> reactorUnlessMe;
    };
    enum class ReactionOp {
        Add,
        Remove,
    };
    enum class ReactionAttempt {
        First,
        Retry,
    };
    void updateReactions(Snowflake messageId, const std::function<bool(std::vector<Discord::Reaction> &)> &mutate);
    void applyOptimistically(ReactionOp op, const ReactionChange &change);
    void sendReactionChange(ReactionOp op, const ReactionChange &change, ReactionAttempt attempt);
    void onReactionChangeFailed(ReactionOp op, const ReactionChange &change, ReactionAttempt attempt, const Discord::Client::ReactionResult &result);
    void cacheGatewayMembers(const Discord::Message &msg);
    void parseMessageContent(Discord::Message &msg);
    void cacheParsedContent(Discord::Message &msg, const std::string &content, Snowflake channelId) const;

    Storage::MessageRepository repo;

    Discord::Client *client;
    UserManager *userManager;
    std::unique_ptr<MessageReactors> reactorStore;
    EmojiManager *emojiManager = nullptr;
    std::unique_ptr<Markdown::Parser> parser;

    struct MessageCache {
        static constexpr std::size_t Capacity = 1000;
        using Entry = std::pair<Snowflake, Discord::Message>;
        std::list<Entry> entries;
        std::unordered_map<Snowflake, std::list<Entry>::iterator> index;
        Discord::Message *object(Snowflake id);
        void insert(Snowflake id, const Discord::Message &message);
        void remove(Snowflake id);
    } messageCache;
    std::unordered_map<Snowflake, MessageSegments> segments;
    std::unordered_set<Snowflake> fetchedChannels;
    std::unordered_map<Snowflake, Snowflake> channelStartId;
    std::set<std::pair<Snowflake, LoadType>> pagesInFlight;
};

} // namespace Core
} // namespace Styx
