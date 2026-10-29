#pragma once

#include "Platform/Trackable.hpp"
#include "Platform/Timer.hpp"
#include "Platform/Native.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <memory>
#include <cstdint>

#include <optional>

#include "Core/Result.hpp"
#include "Core/Snowflake.hpp"
#include "Core/Enums.hpp"
#include "Core/ProxyConfig.hpp"
#include "Core/PendingAttachment.hpp"

#include "Proto/FrecencySettings.hpp"
#include "Proto/UserSettings.hpp"

#include "Gateway.hpp"
#include "GuildRequests.hpp"
#include "HttpClient.hpp"
#include "ClientIdentity.hpp"

using Styx::Core::Snowflake;

namespace Styx {
namespace Discord {

class Client : public Platform::Trackable
{
public:
    enum class MessageLoadType {
        Latest,
        History,
        Future,
        Jump,
        Created,
    };

    explicit Client(const std::string &token, const std::string &gatewayUrl, const std::string &baseUrl,
                    const Core::ProxyConfig &proxy, CaptchaResolver *captchaResolver = nullptr);
    ~Client() override;

    void start();
    void stop();

    [[nodiscard]] Core::ConnectionState getState() const;
    [[nodiscard]] const Core::ProxyConfig &getProxy() const { return proxyConfig; }

    using MessagesCallback = std::function<void(const Core::Result<std::vector<Message>> &)>;
    void fetchLatestMessages(Snowflake channelId, int limit, MessagesCallback callback);
    void fetchHistory(Snowflake channelId, Snowflake beforeId, int limit, MessagesCallback callback);
    void fetchMessagesAfter(Snowflake channelId, Snowflake afterId, int limit, MessagesCallback callback);
    void fetchMessagesAround(Snowflake channelId, Snowflake messageId, int limit, MessagesCallback callback);

    using ProfileCallback = std::function<void(const Core::Result<UserProfile> &)>;
    void fetchUserProfile(Snowflake userId, Snowflake guildId, ProfileCallback callback);

    // empty until its actually fetched
    [[nodiscard]] std::string applicationIconHash(Snowflake applicationId);

    void setUserNote(Snowflake userId, const std::string &note);

    using DmChannelCallback = std::function<void(const Core::Result<ChannelCreate> &)>;
    void openDmChannel(Snowflake userId, DmChannelCallback callback);

    struct ForumThreadSearchResult
    {
        std::vector<Channel> threads;
        std::unordered_map<Snowflake, Message> firstMessages; // (thread id, starter message)
        bool hasMore = false;
        bool indexNotReady = false; // 202
        int retryAfterSeconds = 0;
    };
    using ForumThreadsCallback = std::function<void(const Core::Result<ForumThreadSearchResult> &)>;
    // "last_message_time" / "creation_time"
    void searchForumThreads(Snowflake forumId, int offset, const std::string &sortBy, ForumThreadsCallback callback);

    struct CreatedForumThread
    {
        Channel thread;
        std::optional<Message> starterMessage;
    };
    using ForumThreadCallback = std::function<void(const Core::Result<CreatedForumThread> &)>;
    void createForumThread(Snowflake forumId, const std::string &name,
                           const std::vector<Snowflake> &appliedTags, const std::string &content,
                           const std::string &nonce,
                           const std::vector<Core::PendingAttachment> &attachments,
                           ForumThreadCallback callback);

    using ForumPostDataCallback = std::function<void(const Core::Result<std::unordered_map<Snowflake, Message>> &)>;
    void fetchForumPostData(Snowflake forumId, const std::vector<Snowflake> &threadIds, ForumPostDataCallback callback);

    void joinThread(Snowflake threadId);
    void leaveThread(Snowflake threadId);

    struct ThreadListResult
    {
        std::vector<Channel> threads;
        std::vector<ThreadMember> members;
        bool hasMore = false;
        bool indexNotReady = false; // 202
        int retryAfterSeconds = 0;
    };
    using ThreadListCallback = std::function<void(const Core::Result<ThreadListResult> &)>;
    void searchThreads(Snowflake channelId, bool archived, int offset, ThreadListCallback callback);

    using RefreshedUrlsCallback = std::function<void(const Core::Result<std::unordered_map<std::string, std::string>> &)>;
    void refreshAttachmentUrls(const std::vector<std::string> &urls, RefreshedUrlsCallback callback);

    void sendMessage(Snowflake channelId, const std::string &content, const std::string &nonce,
                     Snowflake replyToMessageId = Snowflake::Invalid,
                     const std::vector<Core::PendingAttachment> &attachments = {});
    bool cancelMessageSend(const std::string &nonce);
    void editMessage(Snowflake channelId, Snowflake messageId, const std::string &content);
    void deleteMessage(Snowflake channelId, Snowflake messageId);
    void pinMessage(Snowflake channelId, Snowflake messageId);
    void unpinMessage(Snowflake channelId, Snowflake messageId);

    enum class ReactionLocation {
        HoverBar,
        InlineButton,
        ContextMenu,
        ReactionPicker,
        Message,
    };

    struct ReactionResult
    {
        bool success = false;
        int statusCode = 0;
        int errorCode = 0;
        int retryAfterSeconds = 0;
    };
    using ReactionCallback = std::function<void(const ReactionResult &)>;

    void addReaction(Snowflake channelId, Snowflake messageId, const std::string &reactionKey, bool isBurst, ReactionLocation location, ReactionCallback callback);
    void removeReaction(Snowflake channelId, Snowflake messageId, const std::string &reactionKey, bool isBurst, ReactionLocation location, std::optional<Snowflake> reactorUnlessMe, ReactionCallback callback);

    struct AckEntry
    {
        Snowflake channelId;
        Snowflake messageId;
        int readStateType = 0;
    };

    void ackMessage(Snowflake channelId, Snowflake messageId, int flags, int lastViewed);
    void ackBulk(const std::vector<AckEntry> &entries, std::function<void(bool success)> onFinished);

    void sendVoiceStateUpdate(Snowflake guildId, Snowflake channelId, bool selfMute, bool selfDeaf);
    void sendCallConnect(Snowflake channelId);
    void sendStreamCreate(Snowflake guildId, Snowflake channelId);
    void sendStreamDelete(const std::string &streamKey);
    void sendStreamWatch(const std::string &streamKey);
    void sendStreamSetPaused(const std::string &streamKey, bool paused);
    void setVoiceConnected(bool connected);
    void setAppFocused(bool focused);

    void restoreHeartbeatSession(const std::optional<HeartbeatSession> &stored);

    void leaveGuild(Snowflake guildId);

    void debugForceReconnect();

    void subscribeToGuildChannel(Snowflake guildId, Snowflake channelId,
                                 const std::vector<std::pair<int, int>> &ranges);
    void ensureSubscriptionByChannel(Snowflake channelId);
    void requestForumUnreads(Snowflake forumId, const std::vector<std::pair<Snowflake, Snowflake>> &threads);
    void requestGuildMembers(Snowflake guildId, const std::vector<Snowflake> &userIds, bool presences = true);
    void queryGuildMembers(Snowflake guildId, const std::string &query, int limit);

    [[nodiscard]] Snowflake getGuildIdForChannel(Snowflake channelId) const;
    void registerChannelGuild(Snowflake channelId, Snowflake guildId);

    [[nodiscard]] PremiumTier getGuildPremiumTier(Snowflake guildId) const;
    [[nodiscard]] int64_t getMaxUploadSize(Snowflake channelId) const;

    using FrecencyCallback = std::function<void(const Core::Result<Proto::FrecencyUserSettings> &)>;
    void fetchFrecencySettings(FrecencyCallback callback);

    struct FrecencyPatchResult
    {
        bool success = false;
        bool rateLimited = false;
        bool outOfDate = false;
        std::string error;
        std::optional<Proto::FrecencyUserSettings> settings; // server state after the patch
    };
    using FrecencyPatchCallback = std::function<void(const FrecencyPatchResult &)>;
    void patchFrecencySettings(const std::string &partialProto,
                               std::optional<uint32_t> requiredDataVersion,
                               FrecencyPatchCallback callback);

    [[nodiscard]] const Proto::PreloadedUserSettings &getSettings() const;
    [[nodiscard]] const User &getMe() const;
    [[nodiscard]] bool isPremium() const;

    template <typename T>
    using ResultCallback = std::function<void(const Core::Result<T> &)>;
    using ActionCallback = ResultCallback<void>;

    void fetchReactors(Snowflake channelId, Snowflake messageId, const std::string &reactionKey, bool isBurst, int limit, std::optional<Snowflake> after, ResultCallback<std::vector<User>> callback);

    void fetchGuildProfile(Snowflake guildId, ResultCallback<GuildProfileEdit> callback);
    void modifyGuildProfile(Snowflake guildId, const GuildProfileEdit &edit, ResultCallback<GuildProfileEdit> callback);
    void modifyGuild(Snowflake guildId, const GuildEdit &edit, const std::map<std::string, std::string> &originalMd5s, ActionCallback callback);
    void deleteGuild(Snowflake guildId, ActionCallback callback);

    void createRole(Snowflake guildId, ResultCallback<Role> callback);
    void modifyRole(Snowflake guildId, Snowflake roleId, const RoleEdit &edit, ActionCallback callback);
    void modifyRolePositions(Snowflake guildId, const std::vector<std::pair<Snowflake, int>> &positions, ActionCallback callback);
    void deleteRole(Snowflake guildId, Snowflake roleId, ActionCallback callback);
    void fetchRoleMemberCounts(Snowflake guildId, ResultCallback<std::unordered_map<Snowflake, int>> callback);
    void fetchRoleMemberIds(Snowflake guildId, Snowflake roleId, ResultCallback<std::vector<Snowflake>> callback);
    void addRoleMembers(Snowflake guildId, Snowflake roleId, const std::vector<Snowflake> &userIds, ResultCallback<std::vector<Snowflake>> callback);

    struct MemberSearchPage
    {
        std::vector<MemberSearchResult> members;
        int totalResultCount = 0;
        std::optional<int> indexingRetryAfterSeconds;
    };
    void searchGuildMembers(Snowflake guildId, const MemberSearchQuery &query, ResultCallback<MemberSearchPage> callback);
    void setMemberRoles(Snowflake guildId, Snowflake userId, const std::vector<Snowflake> &roleIds,
                        const std::vector<Snowflake> &added, const std::vector<Snowflake> &removed, ActionCallback callback);
    void setMemberNickname(Snowflake guildId, Snowflake userId, const std::string &nick, ActionCallback callback);
    void setMemberTimeout(Snowflake guildId, Snowflake userId, const Platform::DateTime &until, const std::optional<std::string> &reason, ActionCallback callback);
    void kickMember(Snowflake guildId, Snowflake userId, const std::string &reason, ActionCallback callback);
    void banMember(Snowflake guildId, Snowflake userId, int deleteMessageSeconds, const std::string &reason, ActionCallback callback);
    void unbanMember(Snowflake guildId, Snowflake userId, ActionCallback callback);
    void requestPruneCount(Snowflake guildId, int days, const std::vector<Snowflake> &includeRoles, ActionCallback callback);
    void pruneMembers(Snowflake guildId, int days, const std::vector<Snowflake> &includeRoles, ActionCallback callback);
    void setMemberUpdatesSubscription(Snowflake guildId, bool subscribed);

    void fetchBans(Snowflake guildId, Snowflake after, ResultCallback<std::vector<Ban>> callback);
    void searchBans(Snowflake guildId, const std::string &query, const std::vector<Snowflake> &userIds, ResultCallback<std::vector<Ban>> callback);

    void fetchGuildInvites(Snowflake guildId, ResultCallback<std::vector<Invite>> callback);
    void createInvite(Snowflake channelId, int maxAgeSeconds, int maxUses, bool temporary, ResultCallback<Invite> callback);
    void revokeInvite(const std::string &code, ActionCallback callback);
    void setIncidentActions(Snowflake guildId, const Platform::DateTime &invitesDisabledUntil, const Platform::DateTime &dmsDisabledUntil,
                            std::optional<int> lockdownHours, ActionCallback callback);

    void fetchGuildEmojis(Snowflake guildId, ResultCallback<std::vector<Emoji>> callback);
    void createGuildEmoji(Snowflake guildId, const std::string &name, const std::string &imageDataUri, const std::string &originalMd5, ResultCallback<Emoji> callback);
    void renameGuildEmoji(Snowflake guildId, Snowflake emojiId, const std::string &name, ActionCallback callback);
    void deleteGuildEmoji(Snowflake guildId, Snowflake emojiId, ActionCallback callback);

    void fetchGuildStickers(Snowflake guildId, ResultCallback<std::vector<Sticker>> callback);
    void createGuildSticker(Snowflake guildId, const std::string &name, const std::string &tags, const std::string &description,
                            const FileUpload &file, const std::string &originalMd5, ResultCallback<Sticker> callback);
    void modifyGuildSticker(Snowflake guildId, Snowflake stickerId, const std::string &name, const std::string &tags,
                            const std::string &description, ActionCallback callback);
    void deleteGuildSticker(Snowflake guildId, Snowflake stickerId, ActionCallback callback);

    struct AuditLogQuery
    {
        static constexpr int PageSize = 50;
        Snowflake before;
        Snowflake userId;
        std::optional<AuditLogAction> actionType;
    };
    void fetchAuditLog(Snowflake guildId, const AuditLogQuery &query, ResultCallback<AuditLog> callback);

    void sendFriendRequest(const std::string &username, ActionCallback callback);

public:
    Platform::Signal<Core::ConnectionState> stateChanged;
    Platform::Signal<const Ready &data> ready;
    Platform::Signal<const ReadySupplemental &data> readySupplemental;
    Platform::Signal<const Message &msg> messageCreated;
    Platform::Signal<const Message &msg> messageUpdated;
    Platform::Signal<const MessageDelete &event> messageDeleted;
    Platform::Signal<const TypingStart &event> typingStart;
    Platform::Signal<const ChannelCreate &event> channelCreated;
    Platform::Signal<const ChannelUpdate &event> channelUpdated;
    Platform::Signal<const ChannelDelete &event> channelDeleted;
    Platform::Signal<const ChannelCreate &event> threadCreated;
    Platform::Signal<const ChannelUpdate &event> threadUpdated;
    Platform::Signal<const ThreadDelete &event> threadDeleted;
    Platform::Signal<const ThreadListSync &event> threadListSync;
    Platform::Signal<const ThreadMemberUpdate &event> threadMemberUpdated;
    Platform::Signal<const ThreadMembersUpdate &event> threadMembersUpdated;
    Platform::Signal<const ForumUnreads &event> forumUnreads;
    Platform::Signal<const GatewayGuild &guild> guildCreated;
    Platform::Signal<const Guild &guild> guildUpdated;
    Platform::Signal<const GuildDelete &event> guildDeleted;
    Platform::Signal<const GuildMembersChunk &chunk> guildMembersChunk;
    Platform::Signal<const GuildMemberAdd &event> guildMemberAdded;
    Platform::Signal<const GuildMemberUpdate &event> guildMemberUpdated;
    Platform::Signal<const GuildMemberRemove &event> guildMemberRemoved;
    Platform::Signal<const GuildRoleCreate &event> guildRoleCreated;
    Platform::Signal<const GuildRoleUpdate &event> guildRoleUpdated;
    Platform::Signal<const GuildRoleDelete &event> guildRoleDeleted;
    Platform::Signal<Snowflake, Snowflake, int> roleMemberCountChanged;
    Platform::Signal<const GuildEmojisUpdate &event> guildEmojisUpdated;
    Platform::Signal<const GuildStickersUpdate &event> guildStickersUpdated;
    Platform::Signal<const GuildBanEvent &event> guildBanAdded;
    Platform::Signal<const GuildBanEvent &event> guildBanRemoved;
    Platform::Signal<const Invite &invite> inviteCreated;
    Platform::Signal<const std::string &code> inviteRevoked;
    Platform::Signal<const GuildPruneUpdate &event> guildPruneUpdated;
    Platform::Signal<const MessageAck &event> messageAcked;
    Platform::Signal<const MessageReactionAdd &event> messageReactionAdd;
    Platform::Signal<const MessageReactionAddMany &event> messageReactionAddMany;
    Platform::Signal<const MessageReactionRemove &event> messageReactionRemove;
    Platform::Signal<const MessageReactionRemoveAll &event> messageReactionRemoveAll;
    Platform::Signal<const MessageReactionRemoveEmoji &event> messageReactionRemoveEmoji;
    Platform::Signal<const UserGuildSettings &settings> userGuildSettingsUpdated;
    Platform::Signal<const NotificationSettings &settings> notificationSettingsUpdated;
    Platform::Signal<const GuildMemberListUpdate &event> guildMemberListUpdate;
    Platform::Signal<const VoiceState &event> voiceStateUpdated;
    Platform::Signal<const VoiceServerUpdate &event> voiceServerUpdated;
    Platform::Signal<const CallCreate &event> callCreated;
    Platform::Signal<const CallUpdate &event> callUpdated;
    Platform::Signal<const CallDelete &event> callDeleted;
    Platform::Signal<const StreamCreate &event> streamCreated;
    Platform::Signal<const StreamUpdate &event> streamUpdated;
    Platform::Signal<const StreamServerUpdate &event> streamServerUpdated;
    Platform::Signal<const StreamDelete &event> streamDeleted;
    Platform::Signal<const Relationship &event> relationshipAdded;
    Platform::Signal<const RelationshipPartial &event> relationshipUpdated;
    Platform::Signal<const RelationshipPartial &event> relationshipRemoved;
    Platform::Signal<const UserNoteUpdate &event> userNoteUpdated;
    Platform::Signal<const UserSettingsProtoUpdate &event> userSettingsProtoUpdated;
    Platform::Signal<const Proto::FrecencyUserSettings &settings> frecencySettingsReceived;
    Platform::Signal<> settingsChanged;
    Platform::Signal<Snowflake> applicationIconResolved;
    Platform::Signal<const Presence &event> presenceUpdated;
    Platform::Signal<const std::vector<Presence> &presences> presencesReplaced;
    Platform::Signal<const std::vector<UserSession> &sessions> sessionsReplaced;
    Platform::Signal<const std::string &nonce, const std::string &error> messageSendFailed;
    Platform::Signal<Core::Snowflake, const std::string &error> guildLeaveFailed;
    Platform::Signal<const std::string &nonce, int, int64_t, int64_t> attachmentUploadProgress;

    Platform::Signal<int, int> reconnecting;
    Platform::Signal<const std::string &errorStr> errorOccurred;
    Platform::Signal<> authenticationFailed;
    Platform::Signal<const HeartbeatSession &session> heartbeatSessionChanged;

private:
    void onConnected();
    void onHeartbeatSessionTimer();
    void onDisconnected(CloseCode code, const std::string &reason);

    void onGatewayReady(const Ready &data);
    void onGatewayReadySupplemental(const ReadySupplemental &data);
    void onGatewayMessageCreate(const Message &msg);
    void onGatewayMessageUpdate(const Message &msg);
    void onGatewayMessageDelete(const MessageDelete &event);
    void onGatewayChannelCreate(const ChannelCreate &event);
    void onGatewayChannelUpdate(const ChannelUpdate &event);
    void onGatewayChannelDelete(const ChannelDelete &event);
    void onGatewayThreadCreate(const ChannelCreate &event);
    void onGatewayThreadUpdate(const ChannelUpdate &event);
    void onGatewayThreadDelete(const ThreadDelete &event);
    void onGatewayThreadListSync(const ThreadListSync &event);
    void onGatewayGuildCreate(const GatewayGuild &guild);
    void onGatewayGuildUpdate(const Guild &guild);
    void onGatewayGuildDelete(const GuildDelete &event);
    void onGatewayGuildRoleCreate(const GuildRoleCreate &event);
    void onGatewayGuildRoleUpdate(const GuildRoleUpdate &event);
    void onGatewayGuildRoleDelete(const GuildRoleDelete &event);
    void onGatewayUserSettingsProtoUpdate(const UserSettingsProtoUpdate &event);

private:
    void fetchMessages(Snowflake channelId, std::vector<std::pair<std::string, std::string>> query, int limit, MessagesCallback callback);
    void indexGuildMappings(const GatewayGuild &guild);
    void applyDiscordLocale();
    void updateActiveState();
    void syncHeartbeatSession();
    void removeGuildMappings(Snowflake guildId);

    struct UploadState
    {
        Snowflake channelId;
        std::string nonce;
        std::vector<Core::PendingAttachment> attachments;
        std::vector<std::string> uploadFilenames;
        std::vector<bool> uploaded;
        int remaining = 0;
        bool failed = false;
        std::shared_ptr<std::atomic<bool>> cancelFlag;

        std::function<void(const nlohmann::json &attachmentsJson)> onUploaded;
        std::function<void(const std::string &error)> onFailed;
    };

    void setState(Core::ConnectionState state);
    void uploadAttachmentsAndSend(const std::shared_ptr<UploadState> &state);
    void finishUpload(const std::shared_ptr<UploadState> &state);
    void failUpload(const std::shared_ptr<UploadState> &state, const std::string &error);
    void postForumThread(Snowflake forumId, const std::string &name,
                         const std::vector<Snowflake> &appliedTags,
                         const nlohmann::json &message,
                         ForumThreadCallback callback);
    void cleanupUploadedSlots(const std::shared_ptr<UploadState> &state);
    void settleUpload(const std::shared_ptr<UploadState> &state);

    HttpCallback applyThen(std::function<void(const nlohmann::json &)> apply, HttpCallback finish);
    HttpCallback memberUpdateHandler(Snowflake guildId, const char *what, ActionCallback callback);
    HttpCallback memberRemovalHandler(Snowflake guildId, Snowflake userId, const char *what, ActionCallback callback);

private:
    Core::ConnectionState state = Core::ConnectionState::Disconnected;

    std::string baseUrl;
    Core::ProxyConfig proxyConfig;
    std::string token;

    ClientIdentity identity;
    HttpClient *httpClient;
    Gateway *gateway;

    std::unordered_map<Snowflake, Snowflake> channelToGuild; // todo prob move this somewhere or just a cache
    std::unordered_map<Snowflake, PremiumTier> guildPremiumTiers;
    std::unordered_set<Snowflake> subscribedGuilds;
    std::unordered_map<std::string, std::shared_ptr<UploadState>> activeUploads; // by nonce

    std::unordered_map<Snowflake, std::string> applicationIcons;
    std::unordered_set<Snowflake> applicationIconRequests;

    Proto::PreloadedUserSettings settings;
    User me;

    bool appFocused = true;
    bool voiceConnected = false;
    Platform::Timer heartbeatSessionTimer;
};


} // namespace Discord
} // namespace Styx