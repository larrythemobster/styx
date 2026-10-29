#include "Gateway.hpp"

#include "Enums.hpp"
#include "Objects.hpp"
#include "Outbound.hpp"
#include "Inbound.hpp"
#include "Events.hpp"
#include "CurlUtils.hpp"
#include "ClientIdentity.hpp"

#include "Core/Logging.hpp"
#include "Proto/ProtoReader.hpp"
#include "Proto/UserSettings.hpp"

#include "Platform/Dispatcher.hpp"

#include <algorithm>
#include <cstdlib>

namespace Styx {
namespace Discord {

static size_t write_cb(char *b, size_t size, size_t nmemb, void *userdata)
{
    return size * nmemb;
}

// Lets curl_easy_perform be interrupted mid-connect; without it a stop() request
// is not seen until the handshake finishes or times out.
static int abort_when_closing(void *userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return static_cast<std::atomic<bool> *>(userdata)->load() ? 1 : 0;
}

Gateway::Gateway(const std::string &token, const std::string &gatewayUrl, ClientIdentity &identity,
                 const Core::ProxyConfig &proxy)
    : token(token), gatewayUrl(gatewayUrl), identity(identity), proxy(proxy), running(false)
{
}

Gateway::~Gateway()
{
    hardStop();
}

void Gateway::start()
{
    if (running) {
        Core::log(Core::LogLevel::Warning, LogDiscord) << "Attempt to start already running gateway";
        return;
    }

    teardown();

    wantToClose = false;
    shouldReconnect = false;
    reconnectAttempts = 0;
    isResuming = false;

    ingest = std::make_unique<IngestThread>();
    auto epoch = ingestEpoch;
    const auto currentEpoch = ++*epoch;
    ingest->payloadReceived.connect(this, [this, epoch, currentEpoch](nlohmann::json payload, uint64_t generation) {
        Platform::postToMain([this, epoch, currentEpoch, generation, payload = std::move(payload)] {
            if (epoch->load() == currentEpoch && ingest && generation == ingest->currentGeneration())
                onPayloadReceived(payload);
        });
    });

    ingest->start();

    running = true;
    networkThread = std::thread(&Gateway::networkLoop, this);
}

void Gateway::stop()
{
    closeTime = std::chrono::steady_clock::now();
    wantToClose = true;
}

void Gateway::hardStop()
{
    wantToClose = true;
    shouldReconnect = false;
    teardown();
}

void Gateway::teardown()
{
    ++*ingestEpoch;
    running = false;

    heartbeatCv.notify_all();
    if (networkThread.joinable())
        networkThread.join();
    {
        std::lock_guard lock(heartbeatThreadMutex);
        if (heartbeatThread.joinable())
            heartbeatThread.join();
    }

    ingest.reset();
}

void Gateway::subscribeToGuild(Core::Snowflake guildId, Core::Snowflake channelId, const std::vector<std::pair<int, int>> &ranges)
{
    GuildSubscriptionsBulk data;
    GuildSubscriptionsBulk::SubscriptionData guild;
    guild.typing = true;
    guild.activities = true;
    guild.threads = true;
    guild.channels[channelId] = ranges;
    data.subscriptions.get()[guildId] = guild;


    sendPayload(data.toJson());
}

void Gateway::setMemberUpdatesSubscription(Core::Snowflake guildId, bool subscribed)
{
    GuildSubscriptionsBulk data;
    GuildSubscriptionsBulk::SubscriptionData guild;
    guild.memberUpdates = subscribed;
    data.subscriptions.get()[guildId] = guild;

    sendPayload(data.toJson());
}

void Gateway::sendPayload(const Core::OrderedJson &obj)
{
    sendPayload(obj.toBytes());
}

void Gateway::sendPayload(const std::string &data)
{
    CurlUtils::wsSend(curl, curlMutex, data.data(), data.size(), CURLWS_TEXT, "gateway");
}

void Gateway::onPayloadReceived(const nlohmann::json &root)
{
    Core::log(Core::LogLevel::Debug, LogDiscord) << "Received payload" << root.dump();

    Inbound msg = Inbound::fromJson(root);

    if (msg.s.has_value())
        lastReceivedSequence = msg.s.value();

    switch (msg.opcode) {
    case OpCode::DISPATCH:
        handleDispatch(msg);
        break;
    case OpCode::HEARTBEAT:
        sendHeartbeat();
        break;
    case OpCode::HELLO:
        handleHello(msg);
        break;
    case OpCode::HEARTBEAT_ACK:
        heartbeatAckReceived = true;
        break;
    case OpCode::RECONNECT:
        Core::log(Core::LogLevel::Info, LogDiscord) << "Server requested reconnect";
        shouldReconnect = true;
        break;
    case OpCode::INVALID_SESSION: {
        bool resumable = msg.data.is_boolean() && msg.data.get<bool>();
        Core::log(Core::LogLevel::Info, LogDiscord) << "Invalid session, resumable:" << resumable;
        if (!resumable) {
            canResume = false;
            sessionId.clear();
        }
        shouldReconnect = true;
        break;
    }
    default:
        break;
    }
}

void Gateway::handleDispatch(const Inbound &data)
{
    std::string t = data.t.value_or("");
    Core::log(Core::LogLevel::Debug, LogDiscord) << "Received dispatch event" << t;

    GatewayEvent event = parseGatewayEvent(t);

    switch (event) {
    case GatewayEvent::READY:
        handleReady(data);
        break;
    case GatewayEvent::READY_SUPPLEMENTAL:
        handleReadySupplemental(data);
        break;
    case GatewayEvent::MESSAGE_CREATE:
        handleMessageCreate(data);
        break;
    case GatewayEvent::MESSAGE_UPDATE:
        handleMessageUpdate(data);
        break;
    case GatewayEvent::MESSAGE_DELETE:
        handleMessageDelete(data);
        break;
    case GatewayEvent::TYPING_START:
        handleTypingStart(data);
        break;
    case GatewayEvent::CHANNEL_CREATE:
        handleChannelCreate(data);
        break;
    case GatewayEvent::CHANNEL_UPDATE:
        handleChannelUpdate(data);
        break;
    case GatewayEvent::CHANNEL_DELETE:
        handleChannelDelete(data);
        break;
    case GatewayEvent::THREAD_CREATE:
        handleThreadCreate(data);
        break;
    case GatewayEvent::THREAD_UPDATE:
        handleThreadUpdate(data);
        break;
    case GatewayEvent::THREAD_DELETE:
        handleThreadDelete(data);
        break;
    case GatewayEvent::THREAD_LIST_SYNC:
        handleThreadListSync(data);
        break;
    case GatewayEvent::THREAD_MEMBER_UPDATE:
        handleThreadMemberUpdate(data);
        break;
    case GatewayEvent::THREAD_MEMBERS_UPDATE:
        handleThreadMembersUpdate(data);
        break;
    case GatewayEvent::FORUM_UNREADS:
        handleForumUnreads(data);
        break;
    case GatewayEvent::GUILD_CREATE:
        handleGuildCreate(data);
        break;
    case GatewayEvent::GUILD_UPDATE:
        handleGuildUpdate(data);
        break;
    case GatewayEvent::GUILD_DELETE:
        handleGuildDelete(data);
        break;
    case GatewayEvent::GUILD_MEMBERS_CHUNK:
        handleGuildMembersChunk(data);
        break;
    case GatewayEvent::GUILD_MEMBER_ADD:
        handleGuildMemberAdd(data);
        break;
    case GatewayEvent::GUILD_MEMBER_UPDATE:
        handleGuildMemberUpdate(data);
        break;
    case GatewayEvent::GUILD_MEMBER_REMOVE:
        handleGuildMemberRemove(data);
        break;
    case GatewayEvent::GUILD_ROLE_CREATE:
        handleGuildRoleCreate(data);
        break;
    case GatewayEvent::GUILD_ROLE_UPDATE:
        handleGuildRoleUpdate(data);
        break;
    case GatewayEvent::GUILD_ROLE_DELETE:
        handleGuildRoleDelete(data);
        break;
    case GatewayEvent::GUILD_EMOJIS_UPDATE:
        handleGuildEmojisUpdate(data);
        break;
    case GatewayEvent::GUILD_STICKERS_UPDATE:
        handleGuildStickersUpdate(data);
        break;
    case GatewayEvent::GUILD_BAN_ADD:
        handleGuildBanAdd(data);
        break;
    case GatewayEvent::GUILD_BAN_REMOVE:
        handleGuildBanRemove(data);
        break;
    case GatewayEvent::GUILD_PRUNE_UPDATE:
        handleGuildPruneUpdate(data);
        break;
    case GatewayEvent::MESSAGE_ACK:
        handleMessageAck(data);
        break;
    case GatewayEvent::MESSAGE_REACTION_ADD:
        handleMessageReactionAdd(data);
        break;
    case GatewayEvent::MESSAGE_REACTION_ADD_MANY:
        handleMessageReactionAddMany(data);
        break;
    case GatewayEvent::MESSAGE_REACTION_REMOVE:
        handleMessageReactionRemove(data);
        break;
    case GatewayEvent::MESSAGE_REACTION_REMOVE_ALL:
        handleMessageReactionRemoveAll(data);
        break;
    case GatewayEvent::MESSAGE_REACTION_REMOVE_EMOJI:
        handleMessageReactionRemoveEmoji(data);
        break;
    case GatewayEvent::USER_GUILD_SETTINGS_UPDATE:
        handleUserGuildSettingsUpdate(data);
        break;
    case GatewayEvent::NOTIFICATION_SETTINGS_UPDATE:
        handleNotificationSettingsUpdate(data);
        break;
    case GatewayEvent::GUILD_MEMBER_LIST_UPDATE:
        handleGuildMemberListUpdate(data);
        break;
    case GatewayEvent::VOICE_STATE_UPDATE:
        handleVoiceStateUpdate(data);
        break;
    case GatewayEvent::VOICE_STATE_UPDATE_BATCH:
        handleVoiceStateUpdateBatch(data);
        break;
    case GatewayEvent::VOICE_SERVER_UPDATE:
        handleVoiceServerUpdate(data);
        break;
    case GatewayEvent::CALL_CREATE:
        handleCallCreate(data);
        break;
    case GatewayEvent::CALL_UPDATE:
        handleCallUpdate(data);
        break;
    case GatewayEvent::CALL_DELETE:
        handleCallDelete(data);
        break;
    case GatewayEvent::STREAM_CREATE:
        gatewayStreamCreate.publish(data.getData<StreamCreate>());
        break;
    case GatewayEvent::STREAM_UPDATE:
        gatewayStreamUpdate.publish(data.getData<StreamUpdate>());
        break;
    case GatewayEvent::STREAM_SERVER_UPDATE:
        gatewayStreamServerUpdate.publish(data.getData<StreamServerUpdate>());
        break;
    case GatewayEvent::STREAM_DELETE:
        gatewayStreamDelete.publish(data.getData<StreamDelete>());
        break;
    case GatewayEvent::RELATIONSHIP_ADD:
        handleRelationshipAdd(data);
        break;
    case GatewayEvent::RELATIONSHIP_UPDATE:
        handleRelationshipUpdate(data);
        break;
    case GatewayEvent::RELATIONSHIP_REMOVE:
        handleRelationshipRemove(data);
        break;
    case GatewayEvent::USER_NOTE_UPDATE:
        handleUserNoteUpdate(data);
        break;
    case GatewayEvent::USER_SETTINGS_PROTO_UPDATE:
        handleUserSettingsProtoUpdate(data);
        break;
    case GatewayEvent::PRESENCE_UPDATE:
        handlePresenceUpdate(data);
        break;
    case GatewayEvent::PRESENCES_REPLACE:
        handlePresencesReplace(data);
        break;
    case GatewayEvent::SESSIONS_REPLACE:
        handleSessionsReplace(data);
        break;
    case GatewayEvent::UNKNOWN:
        Core::log(Core::LogLevel::Info, LogDiscord) << "Unknown gateway event: " << t;
        break;
    default:
        Core::log(Core::LogLevel::Info, LogDiscord) << "Parsed but unhandled gateway event: " << t;
    }
}

void Gateway::handleReady(const Inbound &data)
{
    Core::log(Core::LogLevel::Debug, LogDiscord) << "Received ready event";

    Ready msg = data.getData<Ready>();

    {
        std::lock_guard lock(sessionMutex);
        if (msg.sessionId.hasValue())
            sessionId = msg.sessionId.get();
        if (msg.resumeGatewayUrl.hasValue())
            resumeGatewayUrl = msg.resumeGatewayUrl.get();
        canResume = !sessionId.empty();
    }
    reconnectAttempts = 0;
    sessionEstablished = true;

    gatewayReady.publish(msg);

    sendUpdateTimeSpentSessionId();
}

void Gateway::handleReadySupplemental(const Inbound &data)
{
    Core::log(Core::LogLevel::Debug, LogDiscord) << "Received ready supplemental event";

    ReadySupplemental msg = data.getData<ReadySupplemental>();

    gatewayReadySupplemental.publish(msg);
}

void Gateway::handleMessageCreate(const Inbound &data)
{
    Message msg = data.getData<Message>();

    gatewayMessageCreate.publish(msg);
}

void Gateway::handleMessageUpdate(const Inbound &data)
{
    Message msg = data.getData<Message>();

    gatewayMessageUpdate.publish(msg);
}

void Gateway::handleMessageDelete(const Inbound &data)
{
    MessageDelete event = data.getData<MessageDelete>();

    gatewayMessageDelete.publish(event);
}

void Gateway::handleTypingStart(const Inbound &data)
{
    TypingStart event = data.getData<TypingStart>();

    gatewayTypingStart.publish(event);
}

void Gateway::handleChannelCreate(const Inbound &data)
{
    ChannelCreate event = data.getData<ChannelCreate>();

    gatewayChannelCreate.publish(event);
}

void Gateway::handleChannelUpdate(const Inbound &data)
{
    ChannelUpdate event = data.getData<ChannelUpdate>();

    gatewayChannelUpdate.publish(event);
}

void Gateway::handleChannelDelete(const Inbound &data)
{
    ChannelDelete event = data.getData<ChannelDelete>();

    gatewayChannelDelete.publish(event);
}

void Gateway::handleThreadCreate(const Inbound &data)
{
    ChannelCreate event = data.getData<ChannelCreate>();

    gatewayThreadCreate.publish(event);
}

void Gateway::handleThreadUpdate(const Inbound &data)
{
    ChannelUpdate event = data.getData<ChannelUpdate>();

    gatewayThreadUpdate.publish(event);
}

void Gateway::handleThreadDelete(const Inbound &data)
{
    ThreadDelete event = data.getData<ThreadDelete>();

    gatewayThreadDelete.publish(event);
}

void Gateway::handleThreadListSync(const Inbound &data)
{
    ThreadListSync event = data.getData<ThreadListSync>();

    gatewayThreadListSync.publish(event);
}

void Gateway::handleThreadMemberUpdate(const Inbound &data)
{
    ThreadMemberUpdate event = data.getData<ThreadMemberUpdate>();

    gatewayThreadMemberUpdate.publish(event);
}

void Gateway::handleThreadMembersUpdate(const Inbound &data)
{
    ThreadMembersUpdate event = data.getData<ThreadMembersUpdate>();

    gatewayThreadMembersUpdate.publish(event);
}

void Gateway::handleForumUnreads(const Inbound &data)
{
    ForumUnreads event = data.getData<ForumUnreads>();

    gatewayForumUnreads.publish(event);
}

void Gateway::requestForumUnreads(Core::Snowflake guildId, Core::Snowflake forumId,
                                  const std::vector<std::pair<Core::Snowflake, Core::Snowflake>> &threads)
{
    RequestForumUnreads request;
    request.guildId = guildId;
    request.channelId = forumId;
    request.threads = threads;

    sendPayload(request.toJson());
}

void Gateway::handleGuildCreate(const Inbound &data)
{
    GatewayGuild guild = data.getData<GatewayGuild>();

    gatewayGuildCreate.publish(guild);
}

void Gateway::handleGuildUpdate(const Inbound &data)
{
    Guild guild = data.getData<Guild>();

    gatewayGuildUpdate.publish(guild);
}

void Gateway::handleGuildMembersChunk(const Inbound &data)
{
    GuildMembersChunk chunk = data.getData<GuildMembersChunk>();

    gatewayGuildMembersChunk.publish(chunk);
}

void Gateway::handleGuildMemberAdd(const Inbound &data)
{
    GuildMemberAdd event = data.getData<GuildMemberAdd>();

    gatewayGuildMemberAdd.publish(event);
}

void Gateway::handleGuildMemberUpdate(const Inbound &data)
{
    GuildMemberUpdate event = data.getData<GuildMemberUpdate>();

    gatewayGuildMemberUpdate.publish(event);
}

void Gateway::handleGuildMemberRemove(const Inbound &data)
{
    GuildMemberRemove event = data.getData<GuildMemberRemove>();

    gatewayGuildMemberRemove.publish(event);
}

void Gateway::handleGuildRoleCreate(const Inbound &data)
{
    GuildRoleCreate event = data.getData<GuildRoleCreate>();

    gatewayGuildRoleCreate.publish(event);
}

void Gateway::handleGuildRoleUpdate(const Inbound &data)
{
    GuildRoleUpdate event = data.getData<GuildRoleUpdate>();

    gatewayGuildRoleUpdate.publish(event);
}

void Gateway::handleGuildRoleDelete(const Inbound &data)
{
    GuildRoleDelete event = data.getData<GuildRoleDelete>();

    gatewayGuildRoleDelete.publish(event);
}

void Gateway::handleGuildEmojisUpdate(const Inbound &data)
{
    GuildEmojisUpdate event = data.getData<GuildEmojisUpdate>();

    gatewayGuildEmojisUpdate.publish(event);
}

void Gateway::handleGuildStickersUpdate(const Inbound &data)
{
    GuildStickersUpdate event = data.getData<GuildStickersUpdate>();

    gatewayGuildStickersUpdate.publish(event);
}

void Gateway::handleGuildBanAdd(const Inbound &data)
{
    GuildBanEvent event = data.getData<GuildBanEvent>();

    gatewayGuildBanAdd.publish(event);
}

void Gateway::handleGuildBanRemove(const Inbound &data)
{
    GuildBanEvent event = data.getData<GuildBanEvent>();

    gatewayGuildBanRemove.publish(event);
}

void Gateway::handleGuildPruneUpdate(const Inbound &data)
{
    GuildPruneUpdate event = data.getData<GuildPruneUpdate>();

    gatewayGuildPruneUpdate.publish(event);
}

void Gateway::handleGuildDelete(const Inbound &data)
{
    GuildDelete event = data.getData<GuildDelete>();

    gatewayGuildDelete.publish(event);
}

void Gateway::handleMessageAck(const Inbound &data)
{
    MessageAck event = data.getData<MessageAck>();

    gatewayMessageAck.publish(event);
}

void Gateway::handleMessageReactionAdd(const Inbound &data)
{
    MessageReactionAdd event = data.getData<MessageReactionAdd>();

    gatewayMessageReactionAdd.publish(event);
}

void Gateway::handleMessageReactionAddMany(const Inbound &data)
{
    MessageReactionAddMany event = data.getData<MessageReactionAddMany>();

    gatewayMessageReactionAddMany.publish(event);
}

void Gateway::handleMessageReactionRemove(const Inbound &data)
{
    MessageReactionRemove event = data.getData<MessageReactionRemove>();

    gatewayMessageReactionRemove.publish(event);
}

void Gateway::handleMessageReactionRemoveAll(const Inbound &data)
{
    MessageReactionRemoveAll event = data.getData<MessageReactionRemoveAll>();

    gatewayMessageReactionRemoveAll.publish(event);
}

void Gateway::handleMessageReactionRemoveEmoji(const Inbound &data)
{
    MessageReactionRemoveEmoji event = data.getData<MessageReactionRemoveEmoji>();

    gatewayMessageReactionRemoveEmoji.publish(event);
}

void Gateway::handleUserGuildSettingsUpdate(const Inbound &data)
{
    UserGuildSettings settings = data.getData<UserGuildSettings>();

    gatewayUserGuildSettingsUpdate.publish(settings);
}

void Gateway::handleNotificationSettingsUpdate(const Inbound &data)
{
    NotificationSettings settings = data.getData<NotificationSettings>();

    gatewayNotificationSettingsUpdate.publish(settings);
}

void Gateway::handleGuildMemberListUpdate(const Inbound &data)
{
    GuildMemberListUpdate update = data.getData<GuildMemberListUpdate>();

    gatewayGuildMemberListUpdate.publish(update);
}

void Gateway::handleVoiceStateUpdate(const Inbound &data)
{
    VoiceState event = data.getData<VoiceState>();

    gatewayVoiceStateUpdate.publish(event);
}

void Gateway::handleVoiceStateUpdateBatch(const Inbound &data)
{
    VoiceStateUpdateBatch batch = data.getData<VoiceStateUpdateBatch>();
    if (!batch.voiceStates.hasValue())
        return;

    for (const VoiceState &state : batch.voiceStates.get())
        gatewayVoiceStateUpdate.publish(state);
}

void Gateway::handleVoiceServerUpdate(const Inbound &data)
{
    VoiceServerUpdate event = data.getData<VoiceServerUpdate>();

    gatewayVoiceServerUpdate.publish(event);
}

void Gateway::handleCallCreate(const Inbound &data)
{
    gatewayCallCreate.publish(data.getData<CallCreate>());
}

void Gateway::handleCallUpdate(const Inbound &data)
{
    gatewayCallUpdate.publish(data.getData<CallUpdate>());
}

void Gateway::handleCallDelete(const Inbound &data)
{
    gatewayCallDelete.publish(data.getData<CallDelete>());
}

void Gateway::handleRelationshipAdd(const Inbound &data)
{
    Relationship event = data.getData<Relationship>();
    gatewayRelationshipAdd.publish(event);
}

void Gateway::handleRelationshipUpdate(const Inbound &data)
{
    RelationshipPartial event = data.getData<RelationshipPartial>();
    gatewayRelationshipUpdate.publish(event);
}

void Gateway::handleRelationshipRemove(const Inbound &data)
{
    RelationshipPartial event = data.getData<RelationshipPartial>();
    gatewayRelationshipRemove.publish(event);
}

void Gateway::handleUserNoteUpdate(const Inbound &data)
{
    UserNoteUpdate event = data.getData<UserNoteUpdate>();
    gatewayUserNoteUpdate.publish(event);
}

void Gateway::handleUserSettingsProtoUpdate(const Inbound &data)
{
    UserSettingsProtoUpdate event = data.getData<UserSettingsProtoUpdate>();

    gatewayUserSettingsProtoUpdate.publish(event);
}

void Gateway::handlePresenceUpdate(const Inbound &data)
{
    Presence event = data.getData<Presence>();
    gatewayPresenceUpdate.publish(event);
}

void Gateway::handlePresencesReplace(const Inbound &data)
{
    const auto &array = data.data;
    if (!array.is_array()) return;

    std::vector<Presence> presences;
    presences.reserve(array.size());
    for (const auto &value : array)
        presences.push_back(Presence::fromJson(value));

    Core::log(Core::LogLevel::Debug, LogDiscord) << "PRESENCES_REPLACE:" << presences.size() << "presences";
    gatewayPresencesReplace.publish(presences);
}

void Gateway::handleSessionsReplace(const Inbound &data)
{
    const auto &array = data.data;
    if (!array.is_array()) return;

    std::vector<UserSession> sessions;
    sessions.reserve(array.size());
    for (const auto &value : array)
        sessions.push_back(UserSession::fromJson(value));

    Core::log(Core::LogLevel::Debug, LogDiscord) << "SESSIONS_REPLACE:" << sessions.size() << "sessions";
    gatewaySessionsReplace.publish(sessions);
}

void Gateway::requestGuildMembers(Core::Snowflake guildId, const std::vector<Core::Snowflake> &userIds, bool presences)
{
    for (size_t start = 0; start < userIds.size(); start += 100) {
        RequestGuildMembers request;
        request.guildId = guildId;
        request.userIds = std::vector<Core::Snowflake>(userIds.begin() + start, userIds.begin() + std::min(start + size_t{100}, userIds.size()));
        request.presences = presences;
        sendPayload(request.toJson());
    }
}

void Gateway::queryGuildMembers(Core::Snowflake guildId, const std::string &query, int limit, bool presences)
{
    RequestGuildMembers request;
    request.guildId = guildId;
    request.query = query;
    request.limit = limit;
    request.presences = presences;

    sendPayload(request.toJson());
}

void Gateway::sendVoiceStateUpdate(Core::Snowflake guildId, Core::Snowflake channelId, bool selfMute, bool selfDeaf)
{
    UpdateVoiceState msg;
    if (guildId.isValid())
        msg.guildId = guildId;
    else
        msg.guildId = nullptr;
    if (channelId.isValid())
        msg.channelId = channelId;
    else
        msg.channelId = nullptr;
    msg.selfMute = selfMute;
    msg.selfDeaf = selfDeaf;

    sendPayload(msg.toJson());
}

void Gateway::sendCallConnect(Core::Snowflake channelId)
{
    ConnectCall msg;
    msg.channelId = channelId;
    sendPayload(msg.toJson());
}

void Gateway::sendStreamCreate(Core::Snowflake guildId, Core::Snowflake channelId)
{
    StreamCreateRequest msg;
    msg.type = guildId.isValid() ? "guild" : "call";
    if (guildId.isValid())
        msg.guildId = guildId;
    else
        msg.guildId = nullptr;
    msg.channelId = channelId;
    sendPayload(msg.toJson());
}

void Gateway::sendStreamDelete(const std::string &streamKey)
{
    StreamDeleteRequest msg;
    msg.streamKey = streamKey;
    sendPayload(msg.toJson());
}

void Gateway::sendStreamWatch(const std::string &streamKey)
{
    StreamWatchRequest msg;
    msg.streamKey = streamKey;
    sendPayload(msg.toJson());
}

void Gateway::sendStreamSetPaused(const std::string &streamKey, bool paused)
{
    StreamSetPausedRequest msg;
    msg.streamKey = streamKey;
    msg.paused = paused;
    sendPayload(msg.toJson());
}

void Gateway::handleHello(const Inbound &data)
{
    Core::log(Core::LogLevel::Debug, LogDiscord) << "Received hello";

    Hello msg = data.getData<Hello>();

    heartbeatInterval = msg.heartbeatInterval;
    heartbeatAckReceived = true;

    if (isResuming && canResume)
        resume();
    else
        identify();

    reconnectAttempts = 0;
    isResuming = false;

    {
        std::lock_guard lock(heartbeatThreadMutex);
        if (running && !heartbeatThread.joinable())
            heartbeatThread = std::thread(&Gateway::heartbeatLoop, this);
    }

    gatewayHello.publish();
}

void Gateway::identify()
{
    ClientPropertiesBuildParams params;
    params.includeClientHeartbeatSessionId = true;
    params.isFastConnect = false;
    params.gatewayConnectReasons = "AppSkeleton";
    ClientProperties properties = identity.buildClientProperties(params);

    UpdatePresence presence;
    presence.status = "unknown";
    presence.since = 0;
    presence.afk = false;

    ClientState clientState;

    Identify identify;
    identify.token = token;
    identify.capabilities = CURRENT_CAPABILITIES;
    identify.compress = false;
    identify.properties = properties;
    identify.presence = presence;
    identify.clientState = clientState;

    sendPayload(identify.toJson());
}

void Gateway::networkLoop()
{
    runConnection();

    ++*ingestEpoch;
    running = false;
    heartbeatCv.notify_all();
    {
        std::lock_guard lock(heartbeatThreadMutex);
        if (heartbeatThread.joinable())
            heartbeatThread.join();
    }

    finished.publish();
}

void Gateway::runConnection()
{
    do {
        shouldReconnect = false;

        // Choose URL: use resumeGatewayUrl if resuming, else gatewayUrl.
        // resumeGatewayUrl from READY is a bare host (e.g. wss://gateway-us-east1-b.discord.gg)
        // without query parameters — append them from the original gatewayUrl.
        std::string connectUrl = gatewayUrl;
        {
            std::lock_guard lock(sessionMutex);
            if (isResuming && canResume && !resumeGatewayUrl.empty()) {
                connectUrl = resumeGatewayUrl;
                auto query = gatewayUrl.find('?');
                if (query != std::string::npos) {
                    auto existingQuery = connectUrl.find('?');
                    if (existingQuery != std::string::npos)
                        connectUrl.erase(existingQuery);
                    connectUrl += gatewayUrl.substr(query);
                }
            }
        }

        curl = curl_easy_init();
        if (!curl) {
            Core::log(Core::LogLevel::Error, LogDiscord) << "Failed to initialize curl";
            running = false;
            disconnected.publish(CloseCode::INTERNAL, "Failed to initialize curl");
            return;
        }

        curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
        printf("SSL backend: %s\n", info->ssl_version);

        curl_easy_setopt(curl, CURLOPT_URL, connectUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abort_when_closing);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &wantToClose);
        CurlUtils::applyCommonOptions(curl);
        CurlUtils::applyProxy(curl, proxy);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            if (wantToClose) {
                {
                    std::lock_guard lock(curlMutex);
                    curl_easy_cleanup(curl);
                    curl = nullptr;
                }
                running = false;
                disconnected.publish(CloseCode::CONNECTION_REQUEST_CANCELED,
                                  "Connection cancelled");
                return;
            }

            Core::log(Core::LogLevel::Warning, LogDiscord) << "Failed to connect to gateway:" << curl_easy_strerror(res);

            // On connect failure during reconnect, retry with backoff
            if (reconnectAttempts > 0 && reconnectAttempts < maxReconnectAttempts) {
                {
                    std::lock_guard lock(curlMutex);
                    curl_easy_cleanup(curl);
                    curl = nullptr;
                }

                reconnectAttempts++;
                int delay = 1000 + (std::rand() % 4000);
                Core::log(Core::LogLevel::Info, LogDiscord) << "Reconnect attempt" << reconnectAttempts.load()
                                   << "in" << delay << "ms";
                for (int waited = 0; waited < delay && !wantToClose; waited += 100)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));

                shouldReconnect = true;
                continue;
            }

            {
                std::lock_guard lock(curlMutex);
                curl_easy_cleanup(curl);
                curl = nullptr;
            }
            running = false;
            disconnected.publish(CloseCode::INTERNAL,
                              std::string("Failed to connect to gateway: ") + curl_easy_strerror(res));
            return;
        }

        connected.publish();

        char chunk[8192];
        size_t rlen = 0;
        const curl_ws_frame *meta = nullptr;

        bool closeSent = false;
        while (running) {
            if (shouldReconnect)
                break;

            {
                std::lock_guard lock(curlMutex);

                if (wantToClose) {
                    if (!closeSent) {
                        closeSent = true;
                        uint8_t close_payload[2] = { 0x03, 0xE8 };
                        size_t bytesSent = 0;
                        curl_ws_send(curl, close_payload, sizeof(close_payload), &bytesSent, 0,
                                     CURLWS_CLOSE);
                    }

                    auto now = std::chrono::steady_clock::now();
                    if (now - closeTime > closeTimeout) {
                        running = false;
                        Core::log(Core::LogLevel::Debug, LogDiscord) << "Gateway close timeout";
                        break;
                    }
                }

                rlen = 0;
                meta = nullptr;
                res = curl_ws_recv(curl, chunk, sizeof(chunk), &rlen, &meta);
            }

            if (res == CURLE_AGAIN) {
                CurlUtils::wsRecvWait(curl, curlMutex);

                if (shouldReconnect)
                    break;

                continue;
            }

            if (res != CURLE_OK) {
                Core::log(Core::LogLevel::Warning, LogDiscord) << "curl_ws_recv failed:" << curl_easy_strerror(res);
                if (!wantToClose && canResume)
                    shouldReconnect = true;
                break;
            }

            if (!meta)
                continue;

            if (meta->flags & CURLWS_CLOSE) {
                int closeCode = 1000;
                std::string closeReason;

                if (rlen >= 2) {
                    closeCode = (uint8_t(chunk[0]) << 8) | uint8_t(chunk[1]);
                    if (rlen > 2)
                        closeReason = std::string(chunk + 2, rlen - 2);
                }

                Core::log(Core::LogLevel::Info, LogDiscord) << "Connection closed with code" << closeCode
                                   << "reason:" << closeReason;
                CloseCode cc = static_cast<CloseCode>(closeCode);
                disconnected.publish(cc, closeReason);
                if (!wantToClose && !isFatalCloseCode(cc)) {
                    if (cc == CloseCode::INVALID_SEQ || cc == CloseCode::SESSION_TIMED_OUT ||
                        cc == CloseCode::SESSION_NO_LONGER_VALID)
                        canResume = false;
                    shouldReconnect = true;
                }
                break;
            }

            if (meta->flags & (CURLWS_PING | CURLWS_PONG))
                continue;

            ingest->push(std::string(chunk, rlen));
        }

        // Clean up current connection
        sessionEstablished = false;
        {
            std::lock_guard lock(curlMutex);
            curl_easy_cleanup(curl);
            curl = nullptr;
        }

        // If shouldReconnect was set (by RECONNECT/INVALID_SESSION opcode handlers
        // or by zombie detection), prepare for reconnection
        if (shouldReconnect && running && reconnectAttempts < maxReconnectAttempts) {
            reconnectAttempts++;
            isResuming = canResume.load();

            // Join the heartbeat thread if it exited (e.g. zombie detection broke the loop)
            // so handleHello can start a fresh one on reconnect
            {
                std::lock_guard lock(heartbeatThreadMutex);
                if (heartbeatThread.joinable()) {
                    heartbeatCv.notify_all();
                    heartbeatThread.join();
                }
            }

            // Reset the IngestThread's zlib stream — the new connection starts a fresh
            // zlib context, so the old stream state would corrupt decompression
            ingest->reset();

            int delay = 1000 + (std::rand() % 4000);
            Core::log(Core::LogLevel::Info, LogDiscord) << "Reconnecting in" << delay << "ms (attempt"
                               << reconnectAttempts.load() << ")";
            reconnecting.publish(reconnectAttempts.load(), maxReconnectAttempts);
            for (int waited = 0; waited < delay && !wantToClose; waited += 100)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        else if (reconnectAttempts >= maxReconnectAttempts) {
            shouldReconnect = false;
        }

    } while (shouldReconnect && running && !wantToClose && reconnectAttempts <= maxReconnectAttempts);
}

void Gateway::heartbeatLoop()
{
    Core::log(Core::LogLevel::Debug, LogDiscord) << "Heartbeat loop started, interval:" << heartbeatInterval.load();

    while (running) {
        if (!heartbeatAckReceived) {
            Core::log(Core::LogLevel::Warning, LogDiscord) << "No heartbeat ACK received — zombie connection detected";
            shouldReconnect = true;
            break;
        }
        heartbeatAckReceived = false;

        sendHeartbeat();

        {
            std::unique_lock lock(heartbeatMutex);
            bool stop = heartbeatCv.wait_for(lock, std::chrono::milliseconds(heartbeatInterval.load()),
                                             [this] { return !running || shouldReconnect.load(); });

            if (stop)
                break;
        }
    }
}

void Gateway::sendHeartbeat()
{
    QoSHeartbeat heartbeat;
    heartbeat.seq = lastReceivedSequence.load();
    heartbeat.qos = consumeQoSPayload();
    sendPayload(heartbeat.toJson());
}

void Gateway::sendUpdateTimeSpentSessionId()
{
    if (!sessionEstablished)
        return;

    auto session = identity.heartbeatSession();

    if (!session)
        return;

    UpdateTimeSpentSessionId payload;
    payload.initializationTimestamp = session->createdAtMs;
    payload.sessionId = session->id;
    payload.clientLaunchId = identity.clientLaunchId();
    sendPayload(payload.toJson());
    sendHeartbeat();
}

static QoSPayload makeQoSPayload(const std::vector<std::string> &reasons)
{
    QoSPayload payload;
    payload.ver = 30;
    payload.active = !reasons.empty();
    payload.reasons = reasons;
    return payload;
}

QoSPayload Gateway::consumeQoSPayload()
{
    std::lock_guard lock(qosMutex);
    QoSPayload payload = currentQoS.value_or(makeQoSPayload({}));
    if (upcomingQoS.has_value())
        currentQoS = *upcomingQoS;
    upcomingQoS.reset();
    return payload;
}

void Gateway::setActiveState(bool focused, bool rtcConnected)
{
    std::vector<std::string> reasons;
    if (focused)
        reasons.push_back("foregrounded");
    if (rtcConnected)
        reasons.push_back("rtc_connected");
    QoSPayload state = makeQoSPayload(reasons);

    // this is awkward but it mimics what discord does
    bool sendNow = false;
    {
        std::lock_guard lock(qosMutex);
        bool wasInactive = !currentQoS.has_value() || !currentQoS->active.get();
        if (!currentQoS.has_value())
            currentQoS = state;
        if (state.active.get()) {
            std::vector<std::string> merged = currentQoS->reasons.get();
            for (const std::string &reason : reasons)
                if (std::find(merged.begin(), merged.end(), reason) == merged.end())
                    merged.push_back(reason);
            std::sort(merged.begin(), merged.end());

            currentQoS->active = true;
            currentQoS->reasons = merged;
            sendNow = wasInactive && sessionEstablished;
        }
        upcomingQoS = state;
    }

    if (sendNow)
        sendHeartbeat();
}

void Gateway::debugForceReconnect()
{
    Core::log(Core::LogLevel::Info, LogDiscord) << "DEBUG: Forcing reconnect (simulating op 7 RECONNECT)";
    shouldReconnect = true;
}

void Gateway::resume()
{
    Core::log(Core::LogLevel::Info, LogDiscord) << "Sending RESUME";
    Resume resumeMsg;
    resumeMsg.token = token;
    resumeMsg.sessionId = sessionId;
    resumeMsg.seq = lastReceivedSequence.load();
    sendPayload(resumeMsg.toJson());
    sessionEstablished = true;
}

bool Gateway::isFatalCloseCode(CloseCode code) const
{
    switch (code) {
    case CloseCode::AUTHENTICATION_FAILED:
    case CloseCode::INVALID_SHARD:
    case CloseCode::SHARDING_REQUIRED:
    case CloseCode::INVALID_API_VERSION:
    case CloseCode::INVALID_INTENTS:
    case CloseCode::DISALLOWED_INTENTS:
        return true;
    default:
        return false;
    }
}

} // namespace Discord
} // namespace Styx
