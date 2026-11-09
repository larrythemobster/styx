#include "Client.hpp"

#include "Platform/Base64.hpp"
#include "Platform/Utf8.hpp"
#include <algorithm>
#include <charconv>
#include <format>
#include <utility>
#include <string_view>

#include "ApiError.hpp"
#include "Enums.hpp"
#include "Core/Logging.hpp"
#include "Proto/ProtoReader.hpp"
#include "Proto/PreloadedSettings.hpp"
#include "Proto/UserSettings.hpp"

namespace Styx {
namespace Discord {

namespace {

Proto::GuildFolders guildFoldersFromLegacy(const std::vector<GuildFolderEntry> &entries)
{
    Proto::GuildFolders result;
    result.folders.reserve(entries.size());
    for (const auto &entry : entries) {
        Proto::GuildFolder folder;
        const auto &gids = entry.guildIds.get();
        folder.guildIds.assign(gids.begin(), gids.end());
        if (entry.id.hasValue())
            folder.id = entry.id.get();
        if (entry.name.hasValue())
            folder.name = entry.name.get();
        if (entry.color.hasValue())
            folder.color = static_cast<uint64_t>(entry.color.get());
        result.folders.push_back(std::move(folder));
    }
    return result;
}

std::string frecencySettingsEndpoint()
{
    return "/users/@me/settings-proto/" + std::to_string(int(UserSettingsProtoType::FRECENCY));
}

std::optional<Proto::FrecencyUserSettings> decodeFrecencySettings(const nlohmann::json &object)
{
    if (!object.contains("settings") || !object["settings"].is_string()) return std::nullopt;
    auto bytes = Platform::fromBase64(object["settings"].get_ref<const std::string &>());
    if (!bytes || bytes->empty()) return std::nullopt;
    Proto::ProtoReader reader(*bytes);
    return Proto::FrecencyUserSettings::fromProto(reader);
}

Core::Result<Proto::PreloadedSettingsSnapshot> decodePreloadedSettingsProto(std::string_view base64)
{
    const auto decoded = Platform::fromBase64(base64);
    if (!decoded)
        return Core::Result<Proto::PreloadedSettingsSnapshot>::makeError("Invalid base64 in settings");
    return Proto::PreloadedSettingsSnapshot::fromProto(
        std::string_view(reinterpret_cast<const char *>(decoded->data()), decoded->size()));
}

Core::Result<Proto::PreloadedSettingsSnapshot> decodePreloadedSettingsResponse(const HttpResponse &response, bool *outOfDate = nullptr)
{
    if (outOfDate)
        *outOfDate = false;

    if (!response.success) {
        const ApiError error = ApiError::fromResponse(response);
        return Core::Result<Proto::PreloadedSettingsSnapshot>::makeError(error.message, error.code);
    }

    const auto document = nlohmann::json::parse(response.body, nullptr, false);
    if (!document.is_object())
        return Core::Result<Proto::PreloadedSettingsSnapshot>::makeError("Invalid JSON response");

    if (outOfDate && document.contains("out_of_date")) {
        if (!document["out_of_date"].is_boolean())
            return Core::Result<Proto::PreloadedSettingsSnapshot>::makeError("Invalid out_of_date in response");
        *outOfDate = document["out_of_date"].get<bool>();
    }

    if (!document.contains("settings") || !document["settings"].is_string())
        return Core::Result<Proto::PreloadedSettingsSnapshot>::makeError("Missing settings field");

    return decodePreloadedSettingsProto(document["settings"].get_ref<const std::string &>());
}

std::string guildEndpoint(Snowflake guild, const std::string &path = {})
{
    return "/guilds/" + guild.toString() + path;
}

HttpCallback actionHandler(const char *what, Client::ActionCallback callback)
{
    return [what, callback = std::move(callback)](const HttpResponse &response) {
        if (!response.success) {
            const ApiError error = ApiError::fromResponse(response);
            Core::log(Core::LogLevel::Warning, LogDiscord) << what << "failed with status" << response.statusCode << ":" << error.message;
            if (callback)
                callback(Core::Result<void>::makeError(error.message, error.code));
            return;
        }
        if (callback)
            callback(Core::Result<void>::makeOk());
    };
}

template <typename T, typename Parse>
HttpCallback valueHandler(const char *what, Client::ResultCallback<T> callback, Parse parse)
{
    return [what, callback = std::move(callback), parse = std::move(parse)](const HttpResponse &response) {
        if (!response.success) {
            const ApiError error = ApiError::fromResponse(response);
            Core::log(Core::LogLevel::Warning, LogDiscord) << what << "failed with status" << response.statusCode << ":" << error.message;
            callback(Core::Result<T>::makeError(error.message, error.code));
            return;
        }
        auto document = nlohmann::json::parse(response.body, nullptr, false);
        if (document.is_discarded()) {
            callback(Core::Result<T>::makeError("Invalid JSON response"));
            return;
        }
        callback(Core::Result<T>::makeOk(parse(document)));
    };
}

std::string originalMd5Header(const std::map<std::string, std::string> &hashes)
{
    std::string result;
    for (const auto &[name, hash] : hashes) {
        if (!result.empty()) result += ", ";
        result += Platform::toLowerAscii(name) + "=\"" + hash + '"';
    }
    return result;
}

} // namespace

Client::Client(const std::string &token, const std::string &gatewayUrl, const std::string &baseUrl,
               const Core::ProxyConfig &proxy, CaptchaResolver *captchaResolver)
    : baseUrl(baseUrl), proxyConfig(proxy), token(token)
{
    gateway = new Gateway(token, gatewayUrl, identity, proxy);
    httpClient = new HttpClient(baseUrl, token, identity, proxy, captchaResolver);
    updateActiveState();

    track(gateway->connected.connect([this] { onConnected(); }, Platform::ConnectionType::Queued));
    track(gateway->disconnected.connect([this](auto code, const auto &reason) { onDisconnected(code, reason); }, Platform::ConnectionType::Queued));
    track(gateway->finished.connect([this] { setState(Core::ConnectionState::Disconnected); }, Platform::ConnectionType::Queued));
    track(gateway->gatewayReady.connect([this](const auto &v) { onGatewayReady(v); }));
    track(gateway->gatewayReadySupplemental.connect([this](const auto &v) { onGatewayReadySupplemental(v); }));
    track(gateway->gatewayMessageCreate.connect([this](const auto &v) { onGatewayMessageCreate(v); }));
    track(gateway->gatewayMessageUpdate.connect([this](const auto &v) { onGatewayMessageUpdate(v); }));
    track(gateway->gatewayMessageDelete.connect([this](const auto &v) { onGatewayMessageDelete(v); }));
    track(gateway->gatewayTypingStart.connect([this](const auto &v) { typingStart.publish(v); }));
    track(gateway->gatewayChannelCreate.connect([this](const auto &v) { onGatewayChannelCreate(v); }));
    track(gateway->gatewayChannelUpdate.connect([this](const auto &v) { onGatewayChannelUpdate(v); }));
    track(gateway->gatewayChannelDelete.connect([this](const auto &v) { onGatewayChannelDelete(v); }));
    track(gateway->gatewayThreadCreate.connect([this](const auto &v) { onGatewayThreadCreate(v); }));
    track(gateway->gatewayThreadUpdate.connect([this](const auto &v) { onGatewayThreadUpdate(v); }));
    track(gateway->gatewayThreadDelete.connect([this](const auto &v) { onGatewayThreadDelete(v); }));
    track(gateway->gatewayThreadListSync.connect([this](const auto &v) { onGatewayThreadListSync(v); }));
    track(gateway->gatewayThreadMemberUpdate.connect([this](const auto &v) { threadMemberUpdated.publish(v); }));
    track(gateway->gatewayThreadMembersUpdate.connect([this](const auto &v) { threadMembersUpdated.publish(v); }));
    track(gateway->gatewayForumUnreads.connect([this](const auto &v) { forumUnreads.publish(v); }));
    track(gateway->gatewayGuildCreate.connect([this](const auto &v) { onGatewayGuildCreate(v); }));
    track(gateway->gatewayGuildUpdate.connect([this](const auto &v) { onGatewayGuildUpdate(v); }));
    track(gateway->gatewayGuildDelete.connect([this](const auto &v) { onGatewayGuildDelete(v); }));
    track(gateway->gatewayGuildMembersChunk.connect([this](const auto &v) { guildMembersChunk.publish(v); }));
    track(gateway->gatewayGuildMemberAdd.connect([this](const auto &v) { guildMemberAdded.publish(v); }));
    track(gateway->gatewayGuildMemberUpdate.connect([this](const auto &v) { guildMemberUpdated.publish(v); }));
    track(gateway->gatewayGuildMemberRemove.connect([this](const auto &v) { guildMemberRemoved.publish(v); }));
    track(gateway->gatewayGuildRoleCreate.connect([this](const auto &v) { onGatewayGuildRoleCreate(v); }));
    track(gateway->gatewayGuildRoleUpdate.connect([this](const auto &v) { onGatewayGuildRoleUpdate(v); }));
    track(gateway->gatewayGuildRoleDelete.connect([this](const auto &v) { onGatewayGuildRoleDelete(v); }));
    track(gateway->gatewayGuildEmojisUpdate.connect([this](const auto &v) { guildEmojisUpdated.publish(v); }));
    track(gateway->gatewayGuildStickersUpdate.connect([this](const auto &v) { guildStickersUpdated.publish(v); }));
    track(gateway->gatewayGuildBanAdd.connect([this](const auto &v) { guildBanAdded.publish(v); }));
    track(gateway->gatewayGuildBanRemove.connect([this](const auto &v) { guildBanRemoved.publish(v); }));
    track(gateway->gatewayGuildPruneUpdate.connect([this](const auto &v) { guildPruneUpdated.publish(v); }));
    track(gateway->gatewayMessageAck.connect([this](const auto &v) { messageAcked.publish(v); }));
    track(gateway->gatewayMessageReactionAdd.connect([this](const auto &v) { messageReactionAdd.publish(v); }));
    track(gateway->gatewayMessageReactionAddMany.connect([this](const auto &v) { messageReactionAddMany.publish(v); }));
    track(gateway->gatewayMessageReactionRemove.connect([this](const auto &v) { messageReactionRemove.publish(v); }));
    track(gateway->gatewayMessageReactionRemoveAll.connect([this](const auto &v) { messageReactionRemoveAll.publish(v); }));
    track(gateway->gatewayMessageReactionRemoveEmoji.connect([this](const auto &v) { messageReactionRemoveEmoji.publish(v); }));
    track(gateway->gatewayUserGuildSettingsUpdate.connect([this](const auto &v) { userGuildSettingsUpdated.publish(v); }));
    track(gateway->gatewayNotificationSettingsUpdate.connect([this](const auto &v) { notificationSettingsUpdated.publish(v); }));
    track(gateway->gatewayGuildMemberListUpdate.connect([this](const auto &v) { guildMemberListUpdate.publish(v); }));
    track(gateway->gatewayVoiceStateUpdate.connect([this](const auto &v) { voiceStateUpdated.publish(v); }));
    track(gateway->gatewayVoiceServerUpdate.connect([this](const auto &v) { voiceServerUpdated.publish(v); }));
    track(gateway->gatewayCallCreate.connect([this](const auto &v) { callCreated.publish(v); }));
    track(gateway->gatewayCallUpdate.connect([this](const auto &v) { callUpdated.publish(v); }));
    track(gateway->gatewayCallDelete.connect([this](const auto &v) { callDeleted.publish(v); }));
    track(gateway->gatewayStreamCreate.connect([this](const auto &v) { streamCreated.publish(v); }));
    track(gateway->gatewayStreamUpdate.connect([this](const auto &v) { streamUpdated.publish(v); }));
    track(gateway->gatewayStreamServerUpdate.connect([this](const auto &v) { streamServerUpdated.publish(v); }));
    track(gateway->gatewayStreamDelete.connect([this](const auto &v) { streamDeleted.publish(v); }));
    track(gateway->gatewayInteractionCreate.connect([this](const auto &v) { interactionCreated.publish(v); }));
    track(gateway->gatewayInteractionSuccess.connect([this](const auto &v) { interactionSucceeded.publish(v); }));
    track(gateway->gatewayAutocompleteResponse.connect([this](const auto &v) { autocompleteReceived.publish(v); }));
    track(gateway->gatewayInteractionFailure.connect([this](const auto &v) { interactionFailed.publish(v); }));
    track(gateway->gatewayRelationshipAdd.connect([this](const auto &v) { relationshipAdded.publish(v); }));
    track(gateway->gatewayRelationshipUpdate.connect([this](const auto &v) { relationshipUpdated.publish(v); }));
    track(gateway->gatewayRelationshipRemove.connect([this](const auto &v) { relationshipRemoved.publish(v); }));
    track(gateway->gatewayUserNoteUpdate.connect([this](const auto &v) { userNoteUpdated.publish(v); }));
    track(gateway->gatewayUserSettingsProtoUpdate.connect([this](const auto &v) { onGatewayUserSettingsProtoUpdate(v); }));
    track(gateway->gatewayPresenceUpdate.connect([this](const auto &v) { presenceUpdated.publish(v); }));
    track(gateway->gatewayPresencesReplace.connect([this](const auto &v) { presencesReplaced.publish(v); }));
    track(gateway->gatewaySessionsReplace.connect([this](const auto &v) { sessionsReplaced.publish(v); }));
    track(gateway->reconnecting.connect([this](int attempt, int maxAttempts) {
        setState(Core::ConnectionState::Connecting);
        reconnecting.publish(attempt, maxAttempts);
    }, Platform::ConnectionType::Queued));
}

Client::~Client()
{
    heartbeatSessionTimer.stop();
    delete gateway;
    delete httpClient;
}

void Client::start()
{
    setState(Core::ConnectionState::Connecting);
    gateway->start();
}

void Client::stop()
{
    if (!gateway->isRunning()) {
        setState(Core::ConnectionState::Disconnected);
        return;
    }

    setState(Core::ConnectionState::Disconnecting);
    gateway->stop();
}

[[nodiscard]] Core::ConnectionState Client::getState() const
{
    return state;
}

void Client::onConnected()
{
    Core::log(Core::LogLevel::Info, LogDiscord) << "Connected to gateway";

    setState(Core::ConnectionState::Connected);
}

void Client::onDisconnected(CloseCode code, const std::string &reason)
{
    Core::log(Core::LogLevel::Warning, LogDiscord) << "Disconnected from gateway: " << code << reason;

    // Fatal close codes — no reconnection, transition straight to Disconnected
    if (code == CloseCode::AUTHENTICATION_FAILED ||
        code == CloseCode::INVALID_SHARD ||
        code == CloseCode::SHARDING_REQUIRED ||
        code == CloseCode::INVALID_API_VERSION ||
        code == CloseCode::INVALID_INTENTS ||
        code == CloseCode::DISALLOWED_INTENTS) {
        setState(Core::ConnectionState::Disconnected);
        if (code == CloseCode::AUTHENTICATION_FAILED) {
            errorOccurred.publish("Invalid token");
            authenticationFailed.publish();
        } else {
            errorOccurred.publish("Fatal gateway error: " + reason);
        }
        return;
    }

    // User-initiated disconnect (via stop()) — transition to Disconnected
    if (state == Core::ConnectionState::Disconnecting) {
        setState(Core::ConnectionState::Disconnected);
        return;
    }

    // Non-fatal: Gateway will handle reconnection automatically
    // stateChanged(Connecting) is emitted via the reconnecting signal
}

void Client::onGatewayReady(const Ready &data)
{
    for (const auto &guild : data.guilds.get())
        indexGuildMappings(guild);

    legacyGuildFolders.reset();
    preloadedSnapshot = Proto::PreloadedSettingsSnapshot();

    auto snapshotResult = decodePreloadedSettingsProto(data.userSettingsProto.get());
    if (snapshotResult.success())
        preloadedSnapshot = std::move(*snapshotResult.value);

    Proto::ProtoReader reader(preloadedSnapshot.bytes());
    settings = Proto::PreloadedUserSettings::fromProto(reader);

    if (!preloadedSnapshot.hasGroup(14) && data.userSettings.hasValue()
        && data.userSettings->guildFolders.hasValue() && !data.userSettings->guildFolders->empty()) {
        legacyGuildFolders = guildFoldersFromLegacy(data.userSettings->guildFolders.get());
        settings.guildFolders = legacyGuildFolders;
    }

    applyDiscordLocale();

    me = data.user;

    ready.publish(data);
}

void Client::onGatewayReadySupplemental(const ReadySupplemental &data)
{
    readySupplemental.publish(data);
}

void Client::onGatewayUserSettingsProtoUpdate(const UserSettingsProtoUpdate &event)
{
    Platform::Guard<Client> self(this);
    if (event.type.get() == UserSettingsProtoType::PRELOADED) {
        auto snapshotResult = decodePreloadedSettingsProto(event.proto.get());
        if (snapshotResult.success()) {
            const bool partial = event.partial.hasValue() && event.partial.get();
            applyPreloadedSnapshot(std::move(*snapshotResult.value), partial);
        } else {
            Core::log(Core::LogLevel::Warning, LogDiscord) << "Failed to parse preloaded user settings proto update";
        }
    }

    if (self)
        self->userSettingsProtoUpdated.publish(event);
}

void Client::applyPreloadedSnapshot(Proto::PreloadedSettingsSnapshot snapshot, bool partial)
{
    const auto cachedVersion = preloadedSnapshot.dataVersion();
    const auto incomingVersion = snapshot.dataVersion();
    if (cachedVersion && incomingVersion && *incomingVersion < *cachedVersion)
        return;

    const bool hasGuildFolders = snapshot.hasGroup(14);
    if (partial)
        preloadedSnapshot.mergeFrom(snapshot);
    else
        preloadedSnapshot = std::move(snapshot);

    if (hasGuildFolders)
        legacyGuildFolders.reset();

    Proto::ProtoReader reader(preloadedSnapshot.bytes());
    settings = Proto::PreloadedUserSettings::fromProto(reader);
    if (!settings.guildFolders.has_value() && legacyGuildFolders.has_value())
        settings.guildFolders = legacyGuildFolders;

    applyDiscordLocale();
    settingsChanged.publish();
}

void Client::applyDiscordLocale()
{
    if (settings.localization.has_value() && settings.localization->locale.has_value())
        identity.setDiscordLocale(settings.localization->locale.value());
}

void Client::onGatewayMessageCreate(const Message &msg)
{
    messageCreated.publish(msg);
}

void Client::onGatewayMessageUpdate(const Message &msg)
{
    messageUpdated.publish(msg);
}

void Client::onGatewayMessageDelete(const MessageDelete &event)
{
    messageDeleted.publish(event);
}

void Client::onGatewayChannelCreate(const ChannelCreate &event)
{
    channelCreated.publish(event);
}

void Client::onGatewayGuildCreate(const GatewayGuild &guild)
{
    indexGuildMappings(guild);

    guildCreated.publish(guild);
}

void Client::onGatewayGuildUpdate(const Guild &guild)
{
    guildPremiumTiers[guild.id.get()] = guild.premiumTier.valueOr(PremiumTier::NONE);

    guildUpdated.publish(guild);
}

void Client::onGatewayGuildDelete(const GuildDelete &event)
{
    if (event.userRemoved() && event.id.hasValue())
        removeGuildMappings(event.id.get());

    guildDeleted.publish(event);
}

void Client::indexGuildMappings(const GatewayGuild &guild)
{
    if (!guild.properties.hasValue())
        return;

    Snowflake guildId = guild.properties->id.get();
    if (guild.channels.hasValue())
        for (const auto &channel : guild.channels.get())
            channelToGuild[channel.id] = guildId;
    if (guild.threads.hasValue())
        for (const auto &thread : guild.threads.get())
            channelToGuild[thread.id] = guildId;
    guildPremiumTiers[guildId] = guild.properties->premiumTier.hasValue()
                                              ? guild.properties->premiumTier.get()
                                              : PremiumTier::NONE;
}

void Client::removeGuildMappings(Snowflake guildId)
{
    for (auto it = channelToGuild.begin(); it != channelToGuild.end();) {
        if (it->second == guildId)
            it = channelToGuild.erase(it);
        else
            ++it;
    }
    guildPremiumTiers.erase(guildId);
}

void Client::onGatewayChannelUpdate(const ChannelUpdate &event)
{
    channelUpdated.publish(event);
}

void Client::onGatewayChannelDelete(const ChannelDelete &event)
{
    channelDeleted.publish(event);
}

void Client::onGatewayThreadCreate(const ChannelCreate &event)
{
    const Channel &thread = event.channel.get();
    Snowflake guildId = thread.guildId.hasValue() ? thread.guildId.get() : Snowflake::Invalid;
    if (!guildId.isValid() && thread.parentId.hasValue()) {
        auto it = channelToGuild.find(thread.parentId.get());
        if (it != channelToGuild.end())
            guildId = it->second;
    }
    if (guildId.isValid())
        channelToGuild[thread.id] = guildId;

    threadCreated.publish(event);
}

void Client::onGatewayThreadUpdate(const ChannelUpdate &event)
{
    const Channel &thread = event.channel.get();
    if (thread.guildId.hasValue())
        channelToGuild[thread.id] = thread.guildId.get();

    threadUpdated.publish(event);
}

void Client::onGatewayThreadDelete(const ThreadDelete &event)
{
    channelToGuild.erase(event.id);

    threadDeleted.publish(event);
}

void Client::onGatewayThreadListSync(const ThreadListSync &event)
{
    Snowflake guildId = event.guildId.get();
    if (event.threads.hasValue())
        for (const auto &thread : event.threads.get())
            channelToGuild[thread.id] = guildId;

    threadListSync.publish(event);
}

void Client::onGatewayGuildRoleCreate(const GuildRoleCreate &event)
{
    guildRoleCreated.publish(event);
}

void Client::onGatewayGuildRoleUpdate(const GuildRoleUpdate &event)
{
    guildRoleUpdated.publish(event);
}

void Client::onGatewayGuildRoleDelete(const GuildRoleDelete &event)
{
    guildRoleDeleted.publish(event);
}

void Client::debugForceReconnect()
{
    gateway->debugForceReconnect();
}

void Client::setVoiceConnected(bool connected)
{
    if (voiceConnected == connected)
        return;
    voiceConnected = connected;
    updateActiveState();
}

void Client::setAppFocused(bool focused)
{
    if (appFocused == focused) return;
    appFocused = focused;
    updateActiveState();
}

void Client::updateActiveState()
{
    identity.setActivity(appFocused, voiceConnected);
    gateway->setActiveState(appFocused, voiceConnected);
    syncHeartbeatSession();

    if (appFocused || voiceConnected) {
        if (!heartbeatSessionTimer.active())
            heartbeatSessionTimer.start(std::chrono::seconds(15), true, [this] { onHeartbeatSessionTimer(); });
    } else {
        heartbeatSessionTimer.stop();
    }
}

void Client::restoreHeartbeatSession(const std::optional<HeartbeatSession> &stored)
{
    identity.restoreHeartbeatSession(stored);
    syncHeartbeatSession();
}

void Client::onHeartbeatSessionTimer()
{
    syncHeartbeatSession();
}

void Client::syncHeartbeatSession()
{
    HeartbeatSessionUpdate update = identity.touchHeartbeatSession();
    if (update == HeartbeatSessionUpdate::Unchanged)
        return;

    if (std::optional<HeartbeatSession> session = identity.heartbeatSession())
        heartbeatSessionChanged.publish(*session);

    if (update == HeartbeatSessionUpdate::Created)
        gateway->sendUpdateTimeSpentSessionId();
}

void Client::subscribeToGuildChannel(Snowflake guildId, Snowflake channelId,
                                     const std::vector<std::pair<int, int>> &ranges)
{
    gateway->subscribeToGuild(guildId, channelId, ranges);
    subscribedGuilds.insert(guildId);
}

void Client::ensureSubscriptionByChannel(Snowflake channelId)
{
    if (!channelToGuild.contains(channelId))
        return;

    Snowflake guildId = channelToGuild.at(channelId);
    if (!subscribedGuilds.contains(guildId)) {
        std::vector<std::pair<int, int>> defaultRanges = { { 0, 99 } };
        subscribeToGuildChannel(guildId, channelId, defaultRanges);
    }
}

void Client::requestForumUnreads(Snowflake forumId, const std::vector<std::pair<Snowflake, Snowflake>> &threads)
{
    if (threads.empty())
        return;

    Snowflake guildId = getGuildIdForChannel(forumId);
    if (!guildId.isValid())
        return;

    gateway->requestForumUnreads(guildId, forumId, threads);
}

Snowflake Client::getGuildIdForChannel(Snowflake channelId) const
{
    auto it = channelToGuild.find(channelId);
    return it != channelToGuild.end() ? it->second : Snowflake::Invalid;
}

void Client::registerChannelGuild(Snowflake channelId, Snowflake guildId)
{
    if (channelId.isValid() && guildId.isValid())
        channelToGuild[channelId] = guildId;
}

PremiumTier Client::getGuildPremiumTier(Snowflake guildId) const
{
    auto it = guildPremiumTiers.find(guildId);
    return it != guildPremiumTiers.end() ? it->second : PremiumTier::NONE;
}

int64_t Client::getMaxUploadSize(Snowflake channelId) const
{
    constexpr int64_t MiB = 1024 * 1024;

    auto premiumType = me.premiumType.hasValue() ? me.premiumType.get() : PremiumType::NONE;
    int64_t userLimit = 10 * MiB;
    switch (premiumType) {
    case PremiumType::TIER_1:
        userLimit = 10 * MiB;
        break;
    case PremiumType::TIER_2:
        userLimit = 500 * MiB;
        break;
    case PremiumType::TIER_3:
        userLimit = 50 * MiB;
        break;
    default:
        break;
    }

    int64_t guildLimit = 10 * MiB;
    Snowflake guildId = getGuildIdForChannel(channelId);
    if (guildId.isValid()) {
        switch (getGuildPremiumTier(guildId)) {
        case PremiumTier::TIER_2:
            guildLimit = 50 * MiB;
            break;
        case PremiumTier::TIER_3:
            guildLimit = 100 * MiB;
            break;
        default:
            break;
        }
    }

    return std::max(userLimit, guildLimit);
}

void Client::sendVoiceStateUpdate(Snowflake guildId, Snowflake channelId, bool selfMute, bool selfDeaf)
{
    gateway->sendVoiceStateUpdate(guildId, channelId, selfMute, selfDeaf);
}

void Client::sendCallConnect(Snowflake channelId)
{
    gateway->sendCallConnect(channelId);
}

void Client::sendStreamCreate(Snowflake guildId, Snowflake channelId)
{
    gateway->sendStreamCreate(guildId, channelId);
}

void Client::sendStreamDelete(const std::string &streamKey)
{
    gateway->sendStreamDelete(streamKey);
}

void Client::sendStreamWatch(const std::string &streamKey)
{
    gateway->sendStreamWatch(streamKey);
}

void Client::sendStreamSetPaused(const std::string &streamKey, bool paused)
{
    gateway->sendStreamSetPaused(streamKey, paused);
}

void Client::requestGuildMembers(Snowflake guildId, const std::vector<Snowflake> &userIds, bool presences)
{
    gateway->requestGuildMembers(guildId, userIds, presences);
}

void Client::queryGuildMembers(Snowflake guildId, const std::string &query, int limit)
{
    gateway->queryGuildMembers(guildId, query, limit, false);
}

[[nodiscard]] const Proto::PreloadedUserSettings &Client::getSettings() const
{
    return settings;
}

[[nodiscard]] const User &Client::getMe() const
{
    return me;
}

bool Client::isPremium() const
{
    return me.premiumType.hasValue() && me.premiumType.get() != PremiumType::NONE;
}

void Client::fetchFrecencySettings(FrecencyCallback callback)
{
    Platform::Guard<Client> self(this);
    httpClient->get(frecencySettingsEndpoint(), {}, [self, callback = std::move(callback)](const HttpResponse &response) {
        if (!response.success) {
            callback(Core::Result<Proto::FrecencyUserSettings>::makeError(std::format("status={} error={}", response.statusCode, response.error)));
            return;
        }
        auto object = nlohmann::json::parse(response.body, nullptr, false);
        if (object.is_discarded() || !object.contains("settings") || !object["settings"].is_string()) {
            callback(Core::Result<Proto::FrecencyUserSettings>::makeError("response carries no settings"));
            return;
        }
        auto decoded = decodeFrecencySettings(object);
        if (!decoded) {
            callback(Core::Result<Proto::FrecencyUserSettings>::makeError("Invalid frecency settings"));
            return;
        }
        if (self) self->frecencySettingsReceived.publish(*decoded);
        callback(Core::Result<Proto::FrecencyUserSettings>::makeOk(*decoded));
    });
}

void Client::patchFrecencySettings(const std::string &partialProto, std::optional<uint32_t> version, FrecencyPatchCallback callback)
{
    nlohmann::json payload{{"settings", Platform::toBase64(partialProto)}};
    if (version) payload["required_data_version"] = *version;
    Platform::Guard<Client> self(this);
    httpClient->patch(frecencySettingsEndpoint(), payload, [self, callback = std::move(callback)](const HttpResponse &response) {
        FrecencyPatchResult result;
        result.rateLimited = response.rateLimited();
        if (!response.success) result.error = std::format("status={} error={}", response.statusCode, response.error);
        else {
            auto object = nlohmann::json::parse(response.body, nullptr, false);
            if (object.is_discarded()) result.error = "Invalid JSON response";
            else {
                result.success = true;
                result.outOfDate = object.value("out_of_date", false);
                result.settings = decodeFrecencySettings(object);
                if (self && result.settings) self->frecencySettingsReceived.publish(*result.settings);
            }
        }
        callback(result);
    });
}

void Client::modifyPreloadedSettings(Proto::PreloadedSettingsEdit edit, ActionCallback callback)
{
    if (edit.empty()) {
        if (callback)
            callback(Core::Result<void>::makeOk());
        return;
    }

    pendingPreloadedEdits.push_back({ std::move(edit), std::move(callback) });
    if (!preloadedEditInFlight)
        startNextPreloadedEdit();
}

void Client::startNextPreloadedEdit()
{
    if (pendingPreloadedEdits.empty() || preloadedEditInFlight)
        return;

    preloadedEditInFlight = true;

    Platform::Guard<Client> self(this);
    httpClient->get("/users/@me/settings-proto/1", {}, [self](const HttpResponse &response) {
        if (!self)
            return;

        auto snapshotResult = decodePreloadedSettingsResponse(response);
        if (!snapshotResult.success()) {
            self->finishPreloadedEdit(Core::Result<void>::makeError(std::move(snapshotResult.error), snapshotResult.code));
            return;
        }

        auto freshSnapshot = std::move(*snapshotResult.value);
        auto patchResult = Proto::preloadedSettingsPatch(freshSnapshot, self->pendingPreloadedEdits.front().edit);
        if (!patchResult.success()) {
            self->finishPreloadedEdit(Core::Result<void>::makeError(std::move(patchResult.error)));
            return;
        }

        nlohmann::json payload;
        payload["settings"] = Platform::toBase64(*patchResult.value);
        payload["required_data_version"] = freshSnapshot.dataVersion().value_or(0);
        self->applyPreloadedSnapshot(std::move(freshSnapshot), false);
        if (!self)
            return;

        self->httpClient->patch("/users/@me/settings-proto/1", payload, [self](const HttpResponse &patchResponse) {
            if (!self)
                return;

            bool outOfDate = false;
            auto patchSnapshotResult = decodePreloadedSettingsResponse(patchResponse, &outOfDate);
            if (outOfDate) {
                if (patchSnapshotResult.success())
                    self->applyPreloadedSnapshot(std::move(*patchSnapshotResult.value), false);
                if (self)
                    self->finishPreloadedEdit(Core::Result<void>::makeError("Settings changed on another client. Try again."));
                return;
            }

            if (!patchSnapshotResult.success()) {
                self->finishPreloadedEdit(Core::Result<void>::makeError(std::move(patchSnapshotResult.error), patchSnapshotResult.code));
                return;
            }

            self->applyPreloadedSnapshot(std::move(*patchSnapshotResult.value), false);
            if (self)
                self->finishPreloadedEdit(Core::Result<void>::makeOk());
        });
    });
}

void Client::finishPreloadedEdit(Core::Result<void> result)
{
    ActionCallback callback = std::move(pendingPreloadedEdits.front().callback);
    pendingPreloadedEdits.pop_front();

    Platform::Guard<Client> self(this);
    if (callback)
        callback(result);

    if (!self)
        return;

    preloadedEditInFlight = false;
    startNextPreloadedEdit();
}

void Client::setCustomStatus(const std::optional<Proto::CustomStatus> &customStatus, ActionCallback callback)
{
    Proto::PreloadedSettingsEdit edit;
    edit.customStatus.emplace(customStatus);
    modifyPreloadedSettings(std::move(edit), std::move(callback));
}

HttpCallback Client::applyThen(std::function<void(const nlohmann::json &)> apply, HttpCallback finish)
{
    Platform::Guard<Client> self(this);
    return [self, apply = std::move(apply), finish = std::move(finish)](const HttpResponse &response) {
        if (self && response.success) {
            auto document = nlohmann::json::parse(response.body, nullptr, false);
            if (!document.is_discarded()) apply(document);
        }
        if (finish) finish(response);
    };
}

namespace {

constexpr auto parseCommandIndex = [](const nlohmann::json &document) {
    return parseApplicationCommandIndex(document);
};

} // namespace

void Client::fetchGuildCommandIndex(Snowflake guildId, ResultCallback<ApplicationCommandIndex> callback)
{
    httpClient->get(guildEndpoint(guildId, "/application-command-index"), {},
        valueHandler<ApplicationCommandIndex>("Fetching guild command index", std::move(callback), parseCommandIndex));
}

void Client::fetchChannelCommandIndex(Snowflake channelId, ResultCallback<ApplicationCommandIndex> callback)
{
    httpClient->get("/channels/" + channelId.toString() + "/application-command-index", {},
        valueHandler<ApplicationCommandIndex>("Fetching channel command index", std::move(callback), parseCommandIndex));
}

void Client::fetchUserCommandIndex(ResultCallback<ApplicationCommandIndex> callback)
{
    httpClient->get("/users/@me/application-command-index", {},
        valueHandler<ApplicationCommandIndex>("Fetching user command index", std::move(callback), parseCommandIndex));
}

void Client::fetchGuildProfile(Snowflake guildId, ResultCallback<GuildProfileEdit> callback)
{
    httpClient->get(guildEndpoint(guildId, "/profile"), {},
        valueHandler<GuildProfileEdit>("Fetching the guild profile", std::move(callback),
            [](const nlohmann::json &document) { return GuildProfileEdit::fromProfile(document); }));
}

void Client::modifyGuildProfile(Snowflake guildId, const GuildProfileEdit &edit, ResultCallback<GuildProfileEdit> callback)
{
    httpClient->patch(guildEndpoint(guildId, "/profile"), edit.toJson(),
        valueHandler<GuildProfileEdit>("Saving the guild profile", std::move(callback),
            [](const nlohmann::json &document) { return GuildProfileEdit::fromProfile(document); }));
}

void Client::modifyGuild(Snowflake guildId, const GuildEdit &edit, const std::map<std::string, std::string> &hashes, ActionCallback callback)
{
    RequestOptions options;
    options.originalMd5 = originalMd5Header(hashes);
    httpClient->send(HttpClient::Method::PATCH, guildEndpoint(guildId), edit.toJson().toBytes(), options,
        applyThen([this](const nlohmann::json &document) { onGatewayGuildUpdate(Guild::fromJson(document)); },
            actionHandler("Saving guild settings", std::move(callback))));
}

void Client::modifyCurrentUser(const CurrentUserEdit &edit, const std::map<std::string, std::string> &originalMd5s, ResultCallback<User> callback)
{
    RequestOptions options;
    options.originalMd5 = originalMd5Header(originalMd5s);

    Platform::Guard<Client> self(this);
    httpClient->send(HttpClient::Method::PATCH, "/users/@me", edit.toJson().toBytes(), options,
        [self, callback = std::move(callback)](const HttpResponse &response) {
            if (!response.success) {
                const ApiError error = ApiError::fromResponse(response);
                if (callback)
                    callback(Core::Result<User>::makeError(error.message, error.code));
                return;
            }

            const auto document = nlohmann::json::parse(response.body, nullptr, false);
            if (!document.is_object()) {
                if (callback)
                    callback(Core::Result<User>::makeError("Invalid JSON response"));
                return;
            }

            User user;
            try {
                user = User::fromJson(document);
            } catch (const nlohmann::json::exception &) {
                if (callback)
                    callback(Core::Result<User>::makeError("Invalid user response"));
                return;
            }
            if (!self || !user.id->isValid() || user.id.get() != self->me.id.get()) {
                if (callback)
                    callback(Core::Result<User>::makeError("User ID mismatch or invalid"));
                return;
            }

            self->me = user;
            self->currentUserUpdated.publish(self->me);

            if (callback)
                callback(Core::Result<User>::makeOk(std::move(user)));
        });
}

void Client::modifyCurrentUserProfile(const UserProfileEdit &edit, const std::map<std::string, std::string> &originalMd5s, ResultCallback<UserProfileData> callback)
{
    RequestOptions options;
    options.originalMd5 = originalMd5Header(originalMd5s);

    Platform::Guard<Client> self(this);
    httpClient->send(HttpClient::Method::PATCH, "/users/@me/profile", edit.toJson().toBytes(), options,
        [self, callback = std::move(callback)](const HttpResponse &response) {
            if (!response.success) {
                const ApiError error = ApiError::fromResponse(response);
                if (callback)
                    callback(Core::Result<UserProfileData>::makeError(error.message, error.code));
                return;
            }

            const auto document = nlohmann::json::parse(response.body, nullptr, false);
            if (!document.is_object() || !document.contains("pronouns") || !document["pronouns"].is_string()) {
                if (callback)
                    callback(Core::Result<UserProfileData>::makeError("Invalid JSON response"));
                return;
            }

            UserProfileData metadata;
            try {
                metadata = UserProfileData::fromJson(document);
            } catch (const nlohmann::json::exception &) {
                if (callback)
                    callback(Core::Result<UserProfileData>::makeError("Invalid profile response"));
                return;
            }
            if (!metadata.banner.isUndefined() && self) {
                self->me.banner = metadata.banner;
                self->currentUserUpdated.publish(self->me);
            }

            if (callback)
                callback(Core::Result<UserProfileData>::makeOk(std::move(metadata)));
        });
}


void Client::sendFriendRequest(const std::string &username, ActionCallback callback)
{
    nlohmann::json body;
    const auto hash = username.rfind('#');
    if (hash != std::string::npos) {
        body["username"] = username.substr(0, hash);
        int discriminator = 0;
        std::from_chars(username.data() + hash + 1, username.data() + username.size(), discriminator);
        body["discriminator"] = discriminator;
    } else body["username"] = username;
    httpClient->post("/users/@me/relationships", body, actionHandler("Sending a friend request", std::move(callback)));
}

void Client::acceptFriendRequest(Snowflake userId, ActionCallback callback)
{
    httpClient->put("/users/@me/relationships/" + userId.toString(), nlohmann::json::object(), actionHandler("Accepting a friend request", std::move(callback)));
}

void Client::removeRelationship(Snowflake userId, ActionCallback callback)
{
    httpClient->delete_("/users/@me/relationships/" + userId.toString(), actionHandler("Removing a relationship", std::move(callback)));
}

void Client::blockUser(Snowflake userId, ActionCallback callback)
{
    httpClient->put("/users/@me/relationships/" + userId.toString(), nlohmann::json{{"type", static_cast<int>(RelationshipType::BLOCKED)}}, actionHandler("Blocking a user", std::move(callback)));
}


void Client::setState(Core::ConnectionState state)
{
    if (this->state != state) {
        this->state = state;
        stateChanged.publish(state);
    }
}

} // namespace Discord
} // namespace Styx