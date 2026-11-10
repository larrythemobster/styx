#pragma once

#include "Platform/Trackable.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <condition_variable>
#include <string>
#include <vector>
#include <utility>

#include <curl/curl.h>

#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "Core/ProxyConfig.hpp"
#include "Enums.hpp"
#include "IngestThread.hpp"
#include "Inbound.hpp"
#include "Outbound.hpp"
#include "Events.hpp"
#include "ApplicationCommands.hpp"

namespace Styx {

namespace Proto {
struct PreloadedUserSettings;
}

namespace Discord {

class ClientIdentity;

class Gateway : public Platform::Trackable
{
public:
    explicit Gateway(const std::string &token, const std::string &gatewayUrl, ClientIdentity &identity,
                     const Core::ProxyConfig &proxy);
    ~Gateway();

    void start();
    void stop();
    void hardStop();

    [[nodiscard]] bool isRunning() const { return running; }
    [[nodiscard]] std::string currentSessionId() const;

    void subscribeToGuild(Core::Snowflake guildId, Core::Snowflake channelId, const std::vector<std::pair<int, int>> &ranges);
    void setMemberUpdatesSubscription(Core::Snowflake guildId, bool subscribed);
    void requestGuildMembers(Core::Snowflake guildId, const std::vector<Core::Snowflake> &userIds, bool presences = true);
    void queryGuildMembers(Core::Snowflake guildId, const std::string &query, int limit, bool presences);
    void requestForumUnreads(Core::Snowflake guildId, Core::Snowflake forumId,
                             const std::vector<std::pair<Core::Snowflake, Core::Snowflake>> &threads);
    void sendVoiceStateUpdate(Core::Snowflake guildId, Core::Snowflake channelId, bool selfMute, bool selfDeaf);
    void sendCallConnect(Core::Snowflake channelId);
    void sendStreamCreate(Core::Snowflake guildId, Core::Snowflake channelId);
    void sendStreamDelete(const std::string &streamKey);
    void sendStreamWatch(const std::string &streamKey);
    void sendStreamSetPaused(const std::string &streamKey, bool paused);

    // for the qos heartbeat
    void setActiveState(bool focused, bool rtcConnected);
    void sendUpdateTimeSpentSessionId();

    // Debug: simulate a server RECONNECT opcode
    void debugForceReconnect();

public:
    Platform::Signal<> connected;
    Platform::Signal<CloseCode , const std::string &> disconnected;
    Platform::Signal<int , int> reconnecting;

    // The network loop has exited for good; no reconnection will follow.
    Platform::Signal<> finished;

    Platform::Signal<> gatewayHello;
    Platform::Signal<const Ready &> gatewayReady;
    Platform::Signal<const ReadySupplemental &> gatewayReadySupplemental;
    Platform::Signal<const Message &> gatewayMessageCreate;
    Platform::Signal<const Message &> gatewayMessageUpdate;
    Platform::Signal<const MessageDelete &> gatewayMessageDelete;
    Platform::Signal<const TypingStart &> gatewayTypingStart;
    Platform::Signal<const ChannelCreate &> gatewayChannelCreate;
    Platform::Signal<const ChannelUpdate &> gatewayChannelUpdate;
    Platform::Signal<const ChannelDelete &> gatewayChannelDelete;
    Platform::Signal<const ChannelCreate &> gatewayThreadCreate;
    Platform::Signal<const ChannelUpdate &> gatewayThreadUpdate;
    Platform::Signal<const ThreadDelete &> gatewayThreadDelete;
    Platform::Signal<const ThreadListSync &> gatewayThreadListSync;
    Platform::Signal<const ThreadMemberUpdate &> gatewayThreadMemberUpdate;
    Platform::Signal<const ThreadMembersUpdate &> gatewayThreadMembersUpdate;
    Platform::Signal<const ForumUnreads &> gatewayForumUnreads;
    Platform::Signal<const GatewayGuild &> gatewayGuildCreate;
    Platform::Signal<const Guild &> gatewayGuildUpdate;
    Platform::Signal<const GuildDelete &> gatewayGuildDelete;
    Platform::Signal<const GuildMembersChunk &> gatewayGuildMembersChunk;
    Platform::Signal<const GuildMemberAdd &> gatewayGuildMemberAdd;
    Platform::Signal<const GuildMemberUpdate &> gatewayGuildMemberUpdate;
    Platform::Signal<const GuildMemberRemove &> gatewayGuildMemberRemove;
    Platform::Signal<const GuildRoleCreate &> gatewayGuildRoleCreate;
    Platform::Signal<const GuildRoleUpdate &> gatewayGuildRoleUpdate;
    Platform::Signal<const GuildRoleDelete &> gatewayGuildRoleDelete;
    Platform::Signal<const GuildEmojisUpdate &> gatewayGuildEmojisUpdate;
    Platform::Signal<const GuildStickersUpdate &> gatewayGuildStickersUpdate;
    Platform::Signal<const GuildBanEvent &> gatewayGuildBanAdd;
    Platform::Signal<const GuildBanEvent &> gatewayGuildBanRemove;
    Platform::Signal<const GuildPruneUpdate &> gatewayGuildPruneUpdate;
    Platform::Signal<const MessageAck &> gatewayMessageAck;
    Platform::Signal<const MessagePollVote &> gatewayMessagePollVoteAdd;
    Platform::Signal<const MessagePollVote &> gatewayMessagePollVoteRemove;
    Platform::Signal<const MessageReactionAdd &> gatewayMessageReactionAdd;
    Platform::Signal<const MessageReactionAddMany &> gatewayMessageReactionAddMany;
    Platform::Signal<const MessageReactionRemove &> gatewayMessageReactionRemove;
    Platform::Signal<const MessageReactionRemoveAll &> gatewayMessageReactionRemoveAll;
    Platform::Signal<const MessageReactionRemoveEmoji &> gatewayMessageReactionRemoveEmoji;
    Platform::Signal<const UserGuildSettings &> gatewayUserGuildSettingsUpdate;
    Platform::Signal<const NotificationSettings &> gatewayNotificationSettingsUpdate;
    Platform::Signal<const GuildMemberListUpdate &> gatewayGuildMemberListUpdate;
    Platform::Signal<const VoiceState &> gatewayVoiceStateUpdate;
    Platform::Signal<const VoiceServerUpdate &> gatewayVoiceServerUpdate;
    Platform::Signal<const CallCreate &> gatewayCallCreate;
    Platform::Signal<const CallUpdate &> gatewayCallUpdate;
    Platform::Signal<const CallDelete &> gatewayCallDelete;
    Platform::Signal<const StreamCreate &> gatewayStreamCreate;
    Platform::Signal<const StreamUpdate &> gatewayStreamUpdate;
    Platform::Signal<const StreamServerUpdate &> gatewayStreamServerUpdate;
    Platform::Signal<const StreamDelete &> gatewayStreamDelete;
    Platform::Signal<const Relationship &> gatewayRelationshipAdd;
    Platform::Signal<const RelationshipPartial &> gatewayRelationshipUpdate;
    Platform::Signal<const RelationshipPartial &> gatewayRelationshipRemove;
    Platform::Signal<const UserNoteUpdate &> gatewayUserNoteUpdate;
    Platform::Signal<const UserSettingsProtoUpdate &> gatewayUserSettingsProtoUpdate;
    Platform::Signal<const Presence &> gatewayPresenceUpdate;
    Platform::Signal<const std::vector<Presence> &> gatewayPresencesReplace;
    Platform::Signal<const std::vector<UserSession> &> gatewaySessionsReplace;
    Platform::Signal<const InteractionCreate &> gatewayInteractionCreate;
    Platform::Signal<const InteractionSuccess &> gatewayInteractionSuccess;
    Platform::Signal<const AutocompleteResponse &> gatewayAutocompleteResponse;
    Platform::Signal<const InteractionFailure &> gatewayInteractionFailure;

private:
    void sendPayload(const Core::OrderedJson &obj);
    void sendPayload(const std::string &data);

    // Payloads are delivered on the main dispatcher.
    void onPayloadReceived(const nlohmann::json &root);
    void handleDispatch(const Inbound &data);
    void handleReady(const Inbound &data);
    void handleReadySupplemental(const Inbound &data);
    void handleMessageCreate(const Inbound &data);
    void handleMessageUpdate(const Inbound &data);
    void handleMessageDelete(const Inbound &data);
    void handleTypingStart(const Inbound &data);
    void handleChannelCreate(const Inbound &data);
    void handleChannelUpdate(const Inbound &data);
    void handleChannelDelete(const Inbound &data);
    void handleThreadCreate(const Inbound &data);
    void handleThreadUpdate(const Inbound &data);
    void handleThreadDelete(const Inbound &data);
    void handleThreadListSync(const Inbound &data);
    void handleThreadMemberUpdate(const Inbound &data);
    void handleThreadMembersUpdate(const Inbound &data);
    void handleForumUnreads(const Inbound &data);
    void handleGuildCreate(const Inbound &data);
    void handleGuildUpdate(const Inbound &data);
    void handleGuildDelete(const Inbound &data);
    void handleGuildMembersChunk(const Inbound &data);
    void handleGuildMemberAdd(const Inbound &data);
    void handleGuildMemberUpdate(const Inbound &data);
    void handleGuildMemberRemove(const Inbound &data);
    void handleGuildRoleCreate(const Inbound &data);
    void handleGuildRoleUpdate(const Inbound &data);
    void handleGuildRoleDelete(const Inbound &data);
    void handleGuildEmojisUpdate(const Inbound &data);
    void handleGuildStickersUpdate(const Inbound &data);
    void handleGuildBanAdd(const Inbound &data);
    void handleGuildBanRemove(const Inbound &data);
    void handleGuildPruneUpdate(const Inbound &data);
    void handleMessageAck(const Inbound &data);
    void handleMessageReactionAdd(const Inbound &data);
    void handleMessageReactionAddMany(const Inbound &data);
    void handleMessageReactionRemove(const Inbound &data);
    void handleMessageReactionRemoveAll(const Inbound &data);
    void handleMessageReactionRemoveEmoji(const Inbound &data);
    void handleUserGuildSettingsUpdate(const Inbound &data);
    void handleNotificationSettingsUpdate(const Inbound &data);
    void handleGuildMemberListUpdate(const Inbound &data);
    void handleVoiceStateUpdate(const Inbound &data);
    void handleVoiceStateUpdateBatch(const Inbound &data);
    void handleVoiceServerUpdate(const Inbound &data);
    void handleCallCreate(const Inbound &data);
    void handleCallUpdate(const Inbound &data);
    void handleCallDelete(const Inbound &data);
    void handleRelationshipAdd(const Inbound &data);
    void handleRelationshipUpdate(const Inbound &data);
    void handleRelationshipRemove(const Inbound &data);
    void handleUserNoteUpdate(const Inbound &data);
    void handleUserSettingsProtoUpdate(const Inbound &data);
    void handlePresenceUpdate(const Inbound &data);
    void handlePresencesReplace(const Inbound &data);
    void handleSessionsReplace(const Inbound &data);
    void handleHello(const Inbound &data);
    void identify();
    void resume();
    bool isFatalCloseCode(CloseCode code) const;

    void networkLoop();
    void runConnection();
    void heartbeatLoop();
    void sendHeartbeat();
    QoSPayload consumeQoSPayload();

    // join network and heartbeat threads, destroy ingest thread
    void teardown();

private:
    std::string token;
    std::string gatewayUrl;
    ClientIdentity &identity;
    Core::ProxyConfig proxy;

    std::atomic<bool> running;

    std::mutex curlMutex;
    CURL *curl = nullptr;

    std::unique_ptr<IngestThread> ingest;
    std::shared_ptr<std::atomic<uint64_t>> ingestEpoch = std::make_shared<std::atomic<uint64_t>>(0);

    std::atomic<bool> wantToClose{ false };
    std::thread networkThread;
    std::chrono::steady_clock::time_point closeTime;
    static constexpr std::chrono::milliseconds closeTimeout = std::chrono::milliseconds(1000);

    std::atomic<int> lastReceivedSequence = 0;

    std::atomic<int> heartbeatInterval = 0;
    std::mutex heartbeatMutex;
    std::condition_variable heartbeatCv;
    std::thread heartbeatThread;
    std::mutex heartbeatThreadMutex;

    std::mutex qosMutex;
    std::optional<QoSPayload> currentQoS;
    std::optional<QoSPayload> upcomingQoS;
    std::atomic<bool> sessionEstablished{ false };

    mutable std::mutex sessionMutex;
    std::string sessionId;
    std::string resumeGatewayUrl;

    std::atomic<bool> heartbeatAckReceived{ true };
    std::atomic<bool> shouldReconnect{ false };
    std::atomic<bool> canResume{ false };
    std::atomic<bool> isResuming{ false };
    std::atomic<int> reconnectAttempts{ 0 };
    static constexpr int maxReconnectAttempts = 5;
};

} // namespace Discord
} // namespace Styx
