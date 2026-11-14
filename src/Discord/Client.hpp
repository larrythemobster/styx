#pragma once

#include "Platform/Trackable.hpp"
#include "Platform/Timer.hpp"
#include "Platform/Native.hpp"
#include "AccountSecurity.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <memory>
#include <cstdint>
#include <deque>

#include <optional>

#include "Core/Result.hpp"
#include "Core/Snowflake.hpp"
#include "Core/Enums.hpp"
#include "Core/ProxyConfig.hpp"
#include "Core/PendingAttachment.hpp"

#include "Proto/FrecencySettings.hpp"
#include "Proto/PreloadedSettings.hpp"
#include "Proto/UserSettings.hpp"

#include "Gateway.hpp"
#include "MessageSearch.hpp"
#include "InviteLink.hpp"
#include "GuildRequests.hpp"
#include "UserEdits.hpp"
#include "HttpClient.hpp"
#include "ClientIdentity.hpp"
#include "ComponentInteraction.hpp"
#include "ApplicationCommands.hpp"
#include "CommandInteraction.hpp"

using Styx::Core::Snowflake;

namespace Styx {
namespace Discord {

struct PollCreateRequest;
struct GroupDmEdit;

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
    // Fetch recent mentions without changing read state. before is invalid for the first page.
    void fetchRecentMentions(Snowflake before, int limit, MessagesCallback callback);

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

    using MessageSearchCallback = std::function<void(const Core::Result<MessageSearchResult> &)>;
    // Searches a server when guildId is valid, otherwise the DM or group DM channelId. While
    // Discord is still indexing, it retries a few times before reporting indexNotReady.
    void searchMessages(Snowflake guildId, Snowflake channelId, const MessageSearchQuery &query, MessageSearchCallback callback);

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

    void createGroupDm(const std::vector<Snowflake> &recipients, ResultCallback<Channel> callback = {});
    // Adding to a DM returns a new group; adding to a group returns no channel.
    void addDmRecipient(Snowflake channelId, Snowflake userId, ResultCallback<std::optional<Channel>> callback = {});
    void removeGroupDmRecipient(Snowflake channelId, Snowflake userId, ActionCallback callback = {});
    void modifyGroupDm(Snowflake channelId, const GroupDmEdit &edit, ResultCallback<Channel> callback = {});
    void leaveGroupDm(Snowflake channelId, bool silent = false, ResultCallback<Channel> callback = {});
    // Rings all recipients of an active private-channel call.
    void ringChannelRecipients(Snowflake channelId, ActionCallback callback = {});

    // REST admission only; interactionSucceeded/interactionFailed report the application's result.
    void createComponentInteraction(const ComponentInteractionRequest &request, ActionCallback callback = {});
    // Same admission rules, for a slash command (interaction type 2).
    void createApplicationCommandInteraction(const ApplicationCommandRequest &request, ActionCallback callback = {});
    // Replaces your selected poll answers; an empty list removes all your votes.
    void submitPollVote(Snowflake channelId, Snowflake messageId, const std::vector<int> &answerIds, ActionCallback callback = {});
    // Returns the updated message. Discord permits only the poll's author to end it.
    void endPoll(Snowflake channelId, Snowflake messageId, ResultCallback<Message> callback = {});
    // The caller supplies a nonce for Discord's message deduplication.
    void createPoll(Snowflake channelId, const PollCreateRequest &poll, const std::string &nonce,
                    ResultCallback<Message> callback = {});
    // Slash command indexes: a server's apps, a DM's or group DM's, and the user's own.
    void fetchGuildCommandIndex(Snowflake guildId, ResultCallback<ApplicationCommandIndex> callback);
    void fetchChannelCommandIndex(Snowflake channelId, ResultCallback<ApplicationCommandIndex> callback);
    void fetchUserCommandIndex(ResultCallback<ApplicationCommandIndex> callback);
    void modifyCurrentUser(const CurrentUserEdit &edit, const std::map<std::string, std::string> &originalMd5s, ResultCallback<User> callback = {});
    // Actions Discord guards with a confirmation pass its challenge to the callback instead of a result;
    // confirm it with verifyMfa and call again with the returned authorization.
    template <typename T>
    using GuardedCallback = std::function<void(const Core::Result<T> &, const std::optional<MfaRequirement> &)>;
    // Changes the account password. Discord revokes every token; the callback gets the replacement.
    void changePassword(const std::string &currentPassword, const std::string &newPassword, const std::string &mfaAuthorization,
                        GuardedCallback<std::string> callback);
    // Authenticator app (TOTP) setup: check the password first, then enable with the secret and a code.
    void checkTotpPassword(const std::string &password, const std::string &mfaAuthorization, GuardedCallback<void> callback);
    void enableTotp(const std::string &password, const std::string &secret, const std::string &code,
                    const std::string &mfaAuthorization, ResultCallback<TotpEnabled> callback);
    void disableTotp(const std::string &mfaAuthorization, GuardedCallback<std::string> callback);
    // Confirms a challenge; the result is sent as X-Discord-MFA-Authorization (valid for five minutes).
    void verifyMfa(const std::string &ticket, const MfaProof &proof, ResultCallback<std::string> callback);
    void modifyCurrentUserProfile(const UserProfileEdit &edit, const std::map<std::string, std::string> &originalMd5s, ResultCallback<UserProfileData> callback = {});
    void modifyUserGuildSettings(Snowflake guildId, const UserGuildSettingsEdit &edit, ActionCallback callback = {});
    void modifyCurrentMember(Snowflake guildId, const CurrentMemberEdit &edit,
                             const std::map<std::string, std::string> &originalMd5s, ActionCallback callback = {});
    void modifyPreloadedSettings(Proto::PreloadedSettingsEdit edit, ActionCallback callback = {});
    void setCustomStatus(const std::optional<Proto::CustomStatus> &customStatus, ActionCallback callback = {});

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
    // Looks an invite up with member counts and expiry; fails with ApiError::UnknownInvite for
    // a bad or expired code.
    void resolveInvite(const std::string &code, ResultCallback<InvitePreview> callback);
    // Joins the invite's server. location is the X-Context-Properties location the official client
    // sends for where the invite was opened, e.g. "Markdown Link" or "Join Guild". The result is the
    // server's id.
    void acceptInvite(const std::string &code, const std::string &location, ResultCallback<Snowflake> callback);
    // Creates a server owned by this account; iconDataUri may be empty. The result is its id.
    void createGuild(const std::string &name, const std::string &iconDataUri, ResultCallback<Snowflake> callback);
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
    // Accepts an incoming friend request.
    void acceptFriendRequest(Snowflake userId, ActionCallback callback);
    // Ignores an incoming request, cancels an outgoing one, removes a friend or unblocks.
    void removeRelationship(Snowflake userId, ActionCallback callback);
    void blockUser(Snowflake userId, ActionCallback callback);

public:
    Platform::Signal<Core::ConnectionState> stateChanged;
    Platform::Signal<const Ready &> ready;
    Platform::Signal<const ReadySupplemental &> readySupplemental;
    Platform::Signal<const User &> currentUserUpdated;
    Platform::Signal<const Message &> messageCreated;
    Platform::Signal<const Message &> messageUpdated;
    Platform::Signal<const MessageDelete &> messageDeleted;
    Platform::Signal<const TypingStart &> typingStart;
    Platform::Signal<const ChannelCreate &> channelCreated;
    Platform::Signal<const ChannelUpdate &> channelUpdated;
    Platform::Signal<const ChannelDelete &> channelDeleted;
    Platform::Signal<const ChannelRecipient &> channelRecipientAdded;
    Platform::Signal<const ChannelRecipient &> channelRecipientRemoved;
    Platform::Signal<const ChannelCreate &> threadCreated;
    Platform::Signal<const ChannelUpdate &> threadUpdated;
    Platform::Signal<const ThreadDelete &> threadDeleted;
    Platform::Signal<const ThreadListSync &> threadListSync;
    Platform::Signal<const ThreadMemberUpdate &> threadMemberUpdated;
    Platform::Signal<const ThreadMembersUpdate &> threadMembersUpdated;
    Platform::Signal<const ForumUnreads &> forumUnreads;
    Platform::Signal<const GatewayGuild &> guildCreated;
    Platform::Signal<const Guild &> guildUpdated;
    Platform::Signal<const GuildDelete &> guildDeleted;
    Platform::Signal<const GuildMembersChunk &> guildMembersChunk;
    Platform::Signal<const GuildMemberAdd &> guildMemberAdded;
    Platform::Signal<const GuildMemberUpdate &> guildMemberUpdated;
    Platform::Signal<const GuildMemberRemove &> guildMemberRemoved;
    Platform::Signal<const GuildRoleCreate &> guildRoleCreated;
    Platform::Signal<const GuildRoleUpdate &> guildRoleUpdated;
    Platform::Signal<const GuildRoleDelete &> guildRoleDeleted;
    Platform::Signal<Snowflake, Snowflake, int> roleMemberCountChanged;
    Platform::Signal<const GuildEmojisUpdate &> guildEmojisUpdated;
    Platform::Signal<const GuildStickersUpdate &> guildStickersUpdated;
    Platform::Signal<const GuildBanEvent &> guildBanAdded;
    Platform::Signal<const GuildBanEvent &> guildBanRemoved;
    Platform::Signal<const Invite &> inviteCreated;
    Platform::Signal<const std::string &> inviteRevoked;
    Platform::Signal<const GuildPruneUpdate &> guildPruneUpdated;
    Platform::Signal<const MessageAck &> messageAcked;
    Platform::Signal<const MessagePollVote &> messagePollVoteAdded;
    Platform::Signal<const MessagePollVote &> messagePollVoteRemoved;
    Platform::Signal<const MessageReactionAdd &> messageReactionAdd;
    Platform::Signal<const MessageReactionAddMany &> messageReactionAddMany;
    Platform::Signal<const MessageReactionRemove &> messageReactionRemove;
    Platform::Signal<const MessageReactionRemoveAll &> messageReactionRemoveAll;
    Platform::Signal<const MessageReactionRemoveEmoji &> messageReactionRemoveEmoji;
    Platform::Signal<const UserGuildSettings &> userGuildSettingsUpdated;
    Platform::Signal<const NotificationSettings &> notificationSettingsUpdated;
    Platform::Signal<const GuildMemberListUpdate &> guildMemberListUpdate;
    Platform::Signal<const VoiceState &> voiceStateUpdated;
    Platform::Signal<const VoiceServerUpdate &> voiceServerUpdated;
    Platform::Signal<const CallCreate &> callCreated;
    Platform::Signal<const CallUpdate &> callUpdated;
    Platform::Signal<const CallDelete &> callDeleted;
    Platform::Signal<const StreamCreate &> streamCreated;
    Platform::Signal<const StreamUpdate &> streamUpdated;
    Platform::Signal<const StreamServerUpdate &> streamServerUpdated;
    Platform::Signal<const StreamDelete &> streamDeleted;
    Platform::Signal<const InteractionCreate &> interactionCreated;
    Platform::Signal<const InteractionSuccess &> interactionSucceeded;
    Platform::Signal<const AutocompleteResponse &> autocompleteReceived;
    Platform::Signal<const InteractionFailure &> interactionFailed;
    Platform::Signal<const Relationship &> relationshipAdded;
    Platform::Signal<const RelationshipPartial &> relationshipUpdated;
    Platform::Signal<const RelationshipPartial &> relationshipRemoved;
    Platform::Signal<const UserNoteUpdate &> userNoteUpdated;
    Platform::Signal<const UserSettingsProtoUpdate &> userSettingsProtoUpdated;
    Platform::Signal<const Proto::FrecencyUserSettings &> frecencySettingsReceived;
    Platform::Signal<> settingsChanged;
    Platform::Signal<Snowflake> applicationIconResolved;
    Platform::Signal<const Presence &> presenceUpdated;
    Platform::Signal<const std::vector<Presence> &> presencesReplaced;
    Platform::Signal<const std::vector<UserSession> &> sessionsReplaced;
    Platform::Signal<const std::string &, const std::string &> messageSendFailed;
    Platform::Signal<Core::Snowflake, const std::string &> guildLeaveFailed;
    Platform::Signal<const std::string &, int, int64_t, int64_t> attachmentUploadProgress;

    Platform::Signal<int, int> reconnecting;
    Platform::Signal<const std::string &> errorOccurred;
    Platform::Signal<> authenticationFailed;
    Platform::Signal<const HeartbeatSession &> heartbeatSessionChanged;

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
    void runMessageSearch(std::string endpoint, std::vector<std::pair<std::string, std::string>> query, int attemptsLeft, MessageSearchCallback callback);
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

    void startNextPreloadedEdit();
    void finishPreloadedEdit(Core::Result<void> result);
    void applyPreloadedSnapshot(Proto::PreloadedSettingsSnapshot snapshot, bool partial);

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

    struct PendingPreloadedEdit {
        Proto::PreloadedSettingsEdit edit;
        ActionCallback callback;
    };
    std::deque<PendingPreloadedEdit> pendingPreloadedEdits;
    bool preloadedEditInFlight = false;
    Proto::PreloadedSettingsSnapshot preloadedSnapshot;
    std::optional<Proto::GuildFolders> legacyGuildFolders;

    Proto::PreloadedUserSettings settings;
    User me;

    bool appFocused = true;
    bool voiceConnected = false;
    Platform::Timer heartbeatSessionTimer;
};


} // namespace Discord
} // namespace Styx