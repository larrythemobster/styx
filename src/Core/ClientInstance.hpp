#pragma once

#include "Platform/Trackable.hpp"
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <functional>
#include <memory>
namespace Styx::Platform {
class Database;
}
#include "AccountInfo.hpp"
#include "Discord/Client.hpp"
#include "Emoji/EmojiManager.hpp"
#include "Gifs/FavoriteGifs.hpp"
#include "MessageManager.hpp"
#include "MemberListManager.hpp"
#include "Presence/PresenceManager.hpp"
#include "RelationshipManager.hpp"
#include "RoleHierarchy.hpp"
#include "UserManager.hpp"
#include "PermissionManager.hpp"
#include "Storage/AccountRepository.hpp"
#include "NotificationPolicy.hpp"

namespace Styx {
namespace Core {

#ifndef STYX_NO_VOICE
namespace Audio {
class VoiceManager;
}
#endif
#ifdef STYX_HAVE_SCREENSHARE
namespace Stream {
class StreamManager;
}
#endif

class ReadStateManager;
class ForumManager;
class CallStore;


class ClientInstance : public Platform::Trackable
{
public:
    explicit ClientInstance(const AccountInfo &info,
                            Discord::CaptchaResolver *captchaResolver = nullptr);
    ~ClientInstance() override;

    void start();
    void stop();

    [[nodiscard]] Discord::Client *discord() const;
    [[nodiscard]] MessageManager *messages() const;
    [[nodiscard]] ForumManager *forums() const;
    [[nodiscard]] UserManager *users() const;
    [[nodiscard]] PermissionManager *permissions() const;
    [[nodiscard]] ReadStateManager *readState() const;
    [[nodiscard]] MemberListManager *memberList() const;
    [[nodiscard]] RelationshipManager *relationships() const;
    [[nodiscard]] PresenceManager *presences() const;
    [[nodiscard]] EmojiManager *emojis() const;
    [[nodiscard]] FavoriteGifs *favoriteGifs() const;
    [[nodiscard]] CallStore *calls() const;
#ifndef STYX_NO_VOICE
    [[nodiscard]] Audio::VoiceManager *voice() const;
#endif
#ifdef STYX_HAVE_SCREENSHARE
    [[nodiscard]] Stream::StreamManager *streams() const;
#endif

    [[nodiscard]] std::vector<Discord::Role> getRolesForGuild(Snowflake guildId);
    [[nodiscard]] std::vector<Discord::Role> getMemberRolesSorted(Snowflake guildId, Snowflake userId);
    [[nodiscard]] std::optional<Discord::Guild> getGuild(Snowflake guildId);
    [[nodiscard]] std::optional<Discord::Channel> getChannel(Snowflake channelId);
    [[nodiscard]] std::vector<Discord::Channel> getGuildChannels(Snowflake guildId);
    [[nodiscard]] std::optional<Snowflake> findDmChannelWithUser(Snowflake userId);
    using DmChannelCallback = std::function<void(const Result<Snowflake> &)>;
    void noteChannelSelected(Snowflake channelId);
    void openDmChannel(Snowflake userId, DmChannelCallback callback);
    [[nodiscard]] int getChannelRateLimit(Snowflake channelId);

    [[nodiscard]] bool isThreadJoined(Snowflake threadId) const;

    [[nodiscard]] std::vector<Discord::Ban> observedBans(Snowflake guildId) const;
    [[nodiscard]] std::vector<Discord::Invite> createdInvites(Snowflake guildId) const;
    [[nodiscard]] std::optional<RoleHierarchy> selfRoleHierarchy(Snowflake guildId);

    void markGuildsAsRead(const std::vector<Snowflake> &guildIds);
    void markCategoryAsRead(Snowflake guildId, Snowflake categoryId);

    struct ThreadListPage
    {
        std::vector<Discord::Channel> threads;
        bool hasMore = false;
    };
    using ThreadListPageCallback = std::function<void(const Result<ThreadListPage> &)>;
    void fetchThreadList(Snowflake channelId, bool archived, int offset, ThreadListPageCallback callback);

    [[nodiscard]] ConnectionState state() const;

    [[nodiscard]] Snowflake voiceChannelId() const;
    [[nodiscard]] Snowflake voiceGuildId() const;
    [[nodiscard]] bool isInVoice() const;

    Snowflake accountId() const;
    const AccountInfo &accountInfo() const;

    Platform::Signal<ConnectionState> stateChanged;
    Platform::Signal<int, int> reconnecting;
    Platform::Signal<const AccountInfo &> detailsUpdated;

    Platform::Signal<const Discord::Ready &> ready;
    Platform::Signal<const Discord::GatewayGuild &> guildCreated;
    Platform::Signal<const Discord::Guild &> guildUpdated;
    Platform::Signal<Snowflake> guildRemoved;
    Platform::Signal<const Discord::ChannelCreate &> channelCreated;
    Platform::Signal<const Discord::ChannelUpdate &> channelUpdated;
    Platform::Signal<const Discord::ChannelDelete &> channelDeleted;
    Platform::Signal<const Discord::Channel &> threadCreated;
    Platform::Signal<const Discord::Channel &> threadUpdated;
    Platform::Signal<Snowflake, Snowflake, Snowflake> threadDeleted;
    Platform::Signal<Snowflake, const std::vector<Snowflake> &, const std::vector<Discord::Channel> &> threadListSynced;
    Platform::Signal<Snowflake> threadMembershipChanged;
    Platform::Signal<const Discord::GuildRoleCreate &> guildRoleCreated;
    Platform::Signal<const Discord::GuildRoleUpdate &> guildRoleUpdated;
    Platform::Signal<const Discord::GuildRoleDelete &> guildRoleDeleted;
    Platform::Signal<Snowflake, const std::vector<Snowflake> &> membersUpdated;
    Platform::Signal<Snowflake, Snowflake> memberRemoved;
    Platform::Signal<Snowflake> readStateChanged;
    Platform::Signal<Snowflake> forumBadgeChanged;
    Platform::Signal<Snowflake> forumJoinedPostsChanged;
    Platform::Signal<Snowflake> guildSettingsChanged;
    Platform::Signal<Snowflake, Snowflake> channelLastMessageUpdated;
    // A new message that should raise a desktop notification, and whether it mentions the user.
    Platform::Signal<const Discord::Message &, bool> notificationRequested;
    Platform::Signal<Snowflake, Snowflake> voiceStateChanged;
    Platform::Signal<const AccountInfo &> authenticationFailed;

private:
    void onGuildCreated(const Discord::GatewayGuild &guild);
    void onGuildUpdated(const Discord::Guild &guild);
    void onGuildDeleted(const Discord::GuildDelete &event);
    void onChannelCreated(const Discord::ChannelCreate &event);
    void onChannelUpdated(const Discord::ChannelUpdate &event);
    void onChannelDeleted(const Discord::ChannelDelete &event);
    void onThreadCreated(const Discord::ChannelCreate &event);
    void onThreadUpdated(const Discord::ChannelUpdate &event);
    void onThreadDeleted(const Discord::ThreadDelete &event);
    void onThreadListSync(const Discord::ThreadListSync &event);
    void onThreadMemberUpdate(const Discord::ThreadMemberUpdate &event);
    void onThreadMembersUpdate(const Discord::ThreadMembersUpdate &event);
    void onGuildRoleCreated(const Discord::GuildRoleCreate &event);
    void onGuildRoleUpdated(const Discord::GuildRoleUpdate &event);
    void onGuildRoleDeleted(const Discord::GuildRoleDelete &event);
    void onGuildMembersChunk(const Discord::GuildMembersChunk &chunk);
    void onGuildMemberAdded(const Discord::GuildMemberAdd &event);
    void onGuildMemberUpdate(const Discord::GuildMemberUpdate &event);
    void onGuildMemberRemoved(const Discord::GuildMemberRemove &event);
    void onGuildMemberListUpdate(const Discord::GuildMemberListUpdate &update);
    void onGuildBanAdded(const Discord::GuildBanEvent &event);
    void onGuildBanRemoved(const Discord::GuildBanEvent &event);
    void onInviteCreated(const Discord::Invite &invite);
    void onInviteRevoked(const std::string &code);
    void onMessagesReceived(const MessageRequestResult &result);
    void onMessageCreated(const Discord::Message &msg);
    void handleAckRequest(Snowflake channelId, Snowflake messageId);
    void handleBulkAckRequest(const std::vector<std::pair<Snowflake, Snowflake>> &pairs);
    void onVoiceConnected();
    void onVoiceDisconnected();
    void onCallCreated(const Discord::CallCreate &event);
    void onCallDeleted(const Discord::CallDelete &event);
    void onVoiceStateUpdated(const Discord::VoiceState &event);
    void connectSelectedCall(bool force);
    void onHeartbeatSessionChanged(const Discord::HeartbeatSession &session);
    bool isMessageMentioningMe(const Discord::Message &msg) const;
    [[nodiscard]] NotificationFacts notificationFactsFor(const Discord::Message &msg);

private:
    bool runInCacheTransaction(const char *what, const std::function<void(Platform::Database &)> &op);
    std::vector<Markdown::ChannelLinkPart> channelLinkParts(const Markdown::ChannelLinkRef &ref);

    void saveGuild(const Discord::GatewayGuild &guild, const std::vector<Discord::Member> *members, Snowflake myId, Platform::Database &db);
    void initGuildReadState(const Discord::GatewayGuild &guild);

    [[nodiscard]] bool isForumParent(Snowflake parentId);
    void registerThreadReadState(const Discord::Channel &thread, Snowflake guildId);
    void cacheThread(const Discord::Channel &thread, Snowflake guildId);
    void ingestThread(const Discord::Channel &thread, Snowflake guildId);
    void fetchThreadListAttempt(Snowflake channelId, bool archived, int offset, int attempt, const ThreadListPageCallback &callback);
    [[nodiscard]] std::vector<Snowflake> markableChannelIds(Snowflake guildId, Snowflake categoryId);
    void sendNextBulkAckBatch();

    AccountInfo account;

    std::unique_ptr<MessageManager> messageManager;
    std::unique_ptr<ForumManager> forumManager;
    std::unique_ptr<UserManager> userManager;
    std::unique_ptr<Discord::Client> client;
    std::unique_ptr<PermissionManager> permissionManager;
    std::unique_ptr<ReadStateManager> readStateManager;
    std::unique_ptr<MemberListManager> memberListManager;
    std::unique_ptr<RelationshipManager> relationshipManager;
    std::unique_ptr<PresenceManager> presenceManager;
    std::unique_ptr<EmojiManager> emojiManager;
    std::unique_ptr<FavoriteGifs> favoriteGifManager;
#ifndef STYX_NO_VOICE
    std::unique_ptr<Audio::VoiceManager> voiceManager;
#endif
#ifdef STYX_HAVE_SCREENSHARE
    std::unique_ptr<Stream::StreamManager> streamManager;
#endif
    std::unique_ptr<CallStore> callStore;
    Snowflake selectedChannelId;
    std::string gatewaySessionId;

    Storage::RoleRepository roleRepo;
    Storage::AccountRepository accountRepo;
    Storage::GuildRepository guildRepo;
    Storage::ChannelRepository channelRepo;
    Storage::MemberRepository memberRepo;

    std::unordered_map<Snowflake, std::vector<Discord::Role>> rolesCacheByGuild;

    Snowflake currentVoiceChannelId;
    Snowflake currentVoiceGuildId;

    // notFound members are kept in here so we dont ask for them again
    std::set<std::pair<Snowflake /*guildId*/, Snowflake /*userId*/>> pendingMemberRequests;

    std::unordered_set<Snowflake> joinedThreads;
    std::unordered_map<Snowflake, Discord::Channel> threadCache;
    std::unordered_map<Snowflake, bool> forumParentCache;

    std::unordered_map<Snowflake, std::vector<Discord::Ban>> observedBansByGuild;
    std::unordered_map<Snowflake, std::vector<Discord::Invite>> createdInvitesByGuild;

    std::vector<Discord::Client::AckEntry> queuedBulkAcks;
    bool bulkAckInFlight = false;
};

} // namespace Core
} // namespace Styx
