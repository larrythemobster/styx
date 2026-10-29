#include "Client.hpp"

#include <QDebug>
#include <QGuiApplication>
#include <QJsonObject>
#include <QPointer>
#include <QtMath>

#include "ApiError.hpp"
#include "Enums.hpp"
#include "Core/Logging.hpp"
#include "Proto/ProtoReader.hpp"
#include "Proto/UserSettings.hpp"

namespace Styx {
namespace Discord {

namespace {

Proto::GuildFolders guildFoldersFromLegacy(const QList<GuildFolderEntry> &entries)
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
            folder.name = entry.name.get().toStdString();
        if (entry.color.hasValue())
            folder.color = static_cast<uint64_t>(entry.color.get());
        result.folders.push_back(std::move(folder));
    }
    return result;
}

QString frecencySettingsEndpoint()
{
    return "/users/@me/settings-proto/" + QString::number(int(UserSettingsProtoType::FRECENCY));
}

std::optional<Proto::FrecencyUserSettings> decodeFrecencySettings(const QJsonObject &obj)
{
    const QByteArray proto = QByteArray::fromBase64(obj.value("settings").toString().toUtf8());
    if (proto.isEmpty())
        return std::nullopt;

    Proto::ProtoReader reader(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(proto.constData()), proto.size()));
    return Proto::FrecencyUserSettings::fromProto(reader);
}

QString guildEndpoint(Snowflake guildId, const QString &path = {})
{
    return "/guilds/" + QString::number(guildId) + path;
}

QString memberEndpoint(Snowflake guildId, Snowflake userId)
{
    return guildEndpoint(guildId, "/members/" + QString::number(userId));
}

QString roleEndpoint(Snowflake guildId, Snowflake roleId, const QString &path = {})
{
    return guildEndpoint(guildId, "/roles/" + QString::number(roleId) + path);
}

QJsonArray snowflakeArray(const QList<Snowflake> &ids)
{
    QJsonArray array;
    for (Snowflake id : ids)
        array.append(QString::number(id));
    return array;
}

HttpCallback actionHandler(const char *what, Client::ActionCallback callback)
{
    return [what, callback = std::move(callback)](const HttpResponse &response) {
        if (!response.success) {
            const ApiError error = ApiError::fromResponse(response);
            qCWarning(LogDiscord) << what << "failed with status" << response.statusCode << ":" << error.message;
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
            qCWarning(LogDiscord) << what << "failed with status" << response.statusCode << ":" << error.message;
            callback(Core::Result<T>::makeError(error.message, error.code));
            return;
        }
        callback(Core::Result<T>::makeOk(parse(QJsonDocument::fromJson(response.body))));
    };
}

template <typename T>
QList<T> parseArray(const QJsonDocument &doc)
{
    QList<T> list;
    for (const QJsonValue &value : doc.array())
        list.append(T::fromJson(value.toObject()));
    return list;
}

QByteArray originalMd5Header(const QMap<QString, QByteArray> &md5s)
{
    QList<QByteArray> parts;
    for (auto it = md5s.constBegin(); it != md5s.constEnd(); ++it)
        parts.append(it.key().toLower().toUtf8() + "=\"" + it.value() + '"');
    return parts.join(", ");
}

QString isoTimestamp(const QDateTime &time)
{
    return time.toUTC().toString(Qt::ISODateWithMs);
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

    const std::string protoStr = data.userSettingsProto.get();
    settings = Proto::PreloadedUserSettings::fromBase64(protoStr);

    if ((!settings.guildFolders.has_value() || settings.guildFolders->folders.empty()) && data.userSettings.hasValue() && !data.userSettings->guildFolders->empty())
        settings.guildFolders = guildFoldersFromLegacy(data.userSettings->guildFolders.get());

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
    if (event.type.get() == UserSettingsProtoType::PRELOADED) {
        const std::string protoStr = event.proto.get();
        auto updated = Proto::PreloadedUserSettings::fromBase64(protoStr);

        if (event.partial.hasValue() && event.partial.get()) {
            settings.mergeFrom(updated);
        } else {
            if (!updated.guildFolders.has_value() || updated.guildFolders->folders.empty())
                updated.guildFolders = settings.guildFolders;
            settings = updated;
        }

        applyDiscordLocale();
        settingsChanged.publish();
    }

    userSettingsProtoUpdated.publish(event);
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

void Client::updateActiveState()
{
    identity.setActivity(appFocused, voiceConnected);
    gateway->setActiveState(appFocused, voiceConnected);
    syncHeartbeatSession();

    if (appFocused || voiceConnected) {
        if (!heartbeatSessionTimer->isActive())
            heartbeatSessionTimer->start();
    } else {
        heartbeatSessionTimer->stop();
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
        emit heartbeatSessionChanged(*session);

    if (update == HeartbeatSessionUpdate::Created)
        gateway->sendUpdateTimeSpentSessionId();
}

void Client::subscribeToGuildChannel(Snowflake guildId, Snowflake channelId,
                                     const QList<QPair<int, int>> &ranges)
{
    gateway->subscribeToGuild(guildId, channelId, ranges);
    subscribedGuilds.insert(guildId);
}

void Client::ensureSubscriptionByChannel(Snowflake channelId)
{
    if (!channelToGuild.contains(channelId))
        return;

    Snowflake guildId = channelToGuild.value(channelId);
    if (!subscribedGuilds.contains(guildId)) {
        QList<QPair<int, int>> defaultRanges = { { 0, 99 } };
        subscribeToGuildChannel(guildId, channelId, defaultRanges);
    }
}

void Client::requestForumUnreads(Snowflake forumId, const QList<QPair<Snowflake, Snowflake>> &threads)
{
    if (threads.isEmpty())
        return;

    Snowflake guildId = getGuildIdForChannel(forumId);
    if (!guildId.isValid())
        return;

    gateway->requestForumUnreads(guildId, forumId, threads);
}

Snowflake Client::getGuildIdForChannel(Snowflake channelId) const
{
    return channelToGuild.value(channelId, Snowflake::Invalid);
}

void Client::registerChannelGuild(Snowflake channelId, Snowflake guildId)
{
    if (channelId.isValid() && guildId.isValid())
        channelToGuild.insert(channelId, guildId);
}

PremiumTier Client::getGuildPremiumTier(Snowflake guildId) const
{
    return guildPremiumTiers.value(guildId, PremiumTier::NONE);
}

qint64 Client::getMaxUploadSize(Snowflake channelId) const
{
    constexpr qint64 MiB = 1024 * 1024;

    auto premiumType = me.premiumType.hasValue() ? me.premiumType.get() : PremiumType::NONE;
    qint64 userLimit = 10 * MiB;
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

    qint64 guildLimit = 10 * MiB;
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

    return qMax(userLimit, guildLimit);
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

void Client::sendStreamDelete(const QString &streamKey)
{
    gateway->sendStreamDelete(streamKey);
}

void Client::sendStreamWatch(const QString &streamKey)
{
    gateway->sendStreamWatch(streamKey);
}

void Client::sendStreamSetPaused(const QString &streamKey, bool paused)
{
    gateway->sendStreamSetPaused(streamKey, paused);
}

void Client::requestGuildMembers(Snowflake guildId, const QList<Snowflake> &userIds, bool presences)
{
    gateway->requestGuildMembers(guildId, userIds, presences);
}

void Client::queryGuildMembers(Snowflake guildId, const QString &query, int limit)
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
    QPointer<Client> self(this);
    httpClient->get(frecencySettingsEndpoint(), {}, [self, callback](const HttpResponse &response) {
        if (!response.success) {
            QString err = QStringLiteral("status=%1 error=%2")
                                  .arg(response.statusCode)
                                  .arg(response.error);
            qCWarning(LogDiscord) << "Failed to fetch frecency settings:" << err;
            callback(Core::Result<Proto::FrecencyUserSettings>::makeError(err));
            return;
        }

        const QJsonObject obj = QJsonDocument::fromJson(response.body).object();
        if (!obj.value("settings").isString()) {
            qCWarning(LogDiscord) << "Frecency settings response carries no settings";
            callback(Core::Result<Proto::FrecencyUserSettings>::makeError(QStringLiteral("response carries no settings")));
            return;
        }

        const Proto::FrecencyUserSettings settings = decodeFrecencySettings(obj).value_or(Proto::FrecencyUserSettings());
        if (self)
            emit self->frecencySettingsReceived(settings);
        callback(Core::Result<Proto::FrecencyUserSettings>::makeOk(settings));
    });
}

void Client::patchFrecencySettings(const QByteArray &partialProto,
                                   std::optional<uint32_t> requiredDataVersion,
                                   FrecencyPatchCallback callback)
{
    QJsonObject payload;
    payload["settings"] = QString::fromLatin1(partialProto.toBase64());
    if (requiredDataVersion)
        payload["required_data_version"] = static_cast<qint64>(*requiredDataVersion);

    QPointer<Client> self(this);
    httpClient->patch(frecencySettingsEndpoint(), payload,
                      [self, callback](const HttpResponse &response) {
                          FrecencyPatchResult result;
                          result.rateLimited = response.rateLimited();

                          if (!response.success) {
                              result.error = QStringLiteral("status=%1 error=%2")
                                                     .arg(response.statusCode)
                                                     .arg(response.error);
                              qCWarning(LogDiscord) << "Failed to patch frecency settings:" << result.error;
                              callback(result);
                              return;
                          }

                          const QJsonObject obj = QJsonDocument::fromJson(response.body).object();
                          result.success = true;
                          result.outOfDate = obj.value("out_of_date").toBool();
                          result.settings = decodeFrecencySettings(obj);
                          if (self && result.settings)
                              emit self->frecencySettingsReceived(*result.settings);

                          callback(result);
                      });
}

HttpCallback Client::applyThen(std::function<void(const QJsonDocument &)> apply, HttpCallback finish)
{
    QPointer<Client> self(this);
    return [self, apply = std::move(apply), finish = std::move(finish)](const HttpResponse &response) {
        if (self && response.success)
            apply(QJsonDocument::fromJson(response.body));
        finish(response);
    };
}

void Client::fetchGuildProfile(Snowflake guildId, ResultCallback<GuildProfileEdit> callback)
{
    httpClient->get(guildEndpoint(guildId, "/profile"), {},
                    valueHandler<GuildProfileEdit>("Fetching the guild profile", std::move(callback),
                                                   [](const QJsonDocument &doc) {
                                                       return GuildProfileEdit::fromProfile(doc.object());
                                                   }));
}

void Client::modifyGuildProfile(Snowflake guildId, const GuildProfileEdit &edit, ResultCallback<GuildProfileEdit> callback)
{
    httpClient->patch(guildEndpoint(guildId, "/profile"), edit.toJson(),
                      valueHandler<GuildProfileEdit>("Saving the guild profile", std::move(callback),
                                                     [](const QJsonDocument &doc) {
                                                         return GuildProfileEdit::fromProfile(doc.object());
                                                     }));
}

void Client::modifyGuild(Snowflake guildId, const GuildEdit &edit, const QMap<QString, QByteArray> &originalMd5s, ActionCallback callback)
{
    RequestOptions options;
    options.originalMd5 = originalMd5Header(originalMd5s);
    httpClient->send(HttpClient::Method::PATCH, guildEndpoint(guildId), edit.toJson().toBytes(), options,
                     applyThen([this](const QJsonDocument &doc) { onGatewayGuildUpdate(Guild::fromJson(doc.object())); },
                               actionHandler("Saving guild settings", std::move(callback))));
}

void Client::deleteGuild(Snowflake guildId, ActionCallback callback)
{
    httpClient->send(HttpClient::Method::POST, guildEndpoint(guildId, "/delete"), {}, {}, actionHandler("Deleting a guild", std::move(callback)));
}

void Client::createRole(Snowflake guildId, ResultCallback<Role> callback)
{
    Core::OrderedJson colors;
    colors.insert("primary_color", 0);
    colors.insert("secondary_color", QJsonValue::Null);
    colors.insert("tertiary_color", QJsonValue::Null);

    Core::OrderedJson body;
    body.insert("name", "new role");
    body.insert("color", 0);
    body.insert("colors", colors);
    body.insert("permissions", "0");

    httpClient->post(guildEndpoint(guildId, "/roles"), body,
                     applyThen(
                             [this, guildId](const QJsonDocument &doc) {
                                 GuildRoleCreate event;
                                 event.guildId = guildId;
                                 event.role = Role::fromJson(doc.object());
                                 onGatewayGuildRoleCreate(event);
                             },
                             valueHandler<Role>("Creating a role", std::move(callback),
                                                [](const QJsonDocument &doc) { return Role::fromJson(doc.object()); })));
}

void Client::modifyRole(Snowflake guildId, Snowflake roleId, const RoleEdit &edit, ActionCallback callback)
{
    httpClient->patch(roleEndpoint(guildId, roleId), edit.toJson(),
                      applyThen(
                              [this, guildId](const QJsonDocument &doc) {
                                  GuildRoleUpdate event;
                                  event.guildId = guildId;
                                  event.role = Role::fromJson(doc.object());
                                  onGatewayGuildRoleUpdate(event);
                              },
                              actionHandler("Saving a role", std::move(callback))));
}

void Client::modifyRolePositions(Snowflake guildId, const QList<QPair<Snowflake, int>> &positions, ActionCallback callback)
{
    Core::OrderedJson::Array body;
    QSet<Snowflake> moved;
    for (const auto &[roleId, position] : positions) {
        Core::OrderedJson entry;
        entry.insert("id", QString::number(roleId));
        entry.insert("position", position);
        body.append(entry);
        moved.insert(roleId);
    }

    httpClient->patch(guildEndpoint(guildId, "/roles"), body,
                      applyThen(
                              [this, guildId, moved](const QJsonDocument &doc) {
                                  for (const Role &role : parseArray<Role>(doc)) {
                                      if (!moved.contains(role.id.get()))
                                          continue;
                                      GuildRoleUpdate event;
                                      event.guildId = guildId;
                                      event.role = role;
                                      onGatewayGuildRoleUpdate(event);
                                  }
                              },
                              actionHandler("Reordering roles", std::move(callback))));
}

void Client::deleteRole(Snowflake guildId, Snowflake roleId, ActionCallback callback)
{
    httpClient->delete_(roleEndpoint(guildId, roleId), applyThen(
                                                               [this, guildId, roleId](const QJsonDocument &) {
                                                                   GuildRoleDelete event;
                                                                   event.guildId = guildId;
                                                                   event.roleId = roleId;
                                                                   onGatewayGuildRoleDelete(event);
                                                               },
                                                               actionHandler("Deleting a role", std::move(callback))));
}

void Client::fetchRoleMemberCounts(Snowflake guildId, ResultCallback<QHash<Snowflake, int>> callback)
{
    httpClient->get(guildEndpoint(guildId, "/roles/member-counts"), {},
                    valueHandler<QHash<Snowflake, int>>("Fetching role member counts", std::move(callback),
                                                        [](const QJsonDocument &doc) {
                                                            QHash<Snowflake, int> counts;
                                                            const QJsonObject obj = doc.object();
                                                            for (auto it = obj.constBegin(); it != obj.constEnd(); ++it)
                                                                counts.insert(Snowflake(it.key().toULongLong()), it.value().toInt());
                                                            return counts;
                                                        }));
}

void Client::fetchRoleMemberIds(Snowflake guildId, Snowflake roleId, ResultCallback<QList<Snowflake>> callback)
{
    httpClient->get(roleEndpoint(guildId, roleId, "/member-ids"), {},
                    valueHandler<QList<Snowflake>>("Fetching role members", std::move(callback),
                                                   [](const QJsonDocument &doc) {
                                                       QList<Snowflake> ids;
                                                       for (const QJsonValue &value : doc.array())
                                                           ids.append(Snowflake(value.toString().toULongLong()));
                                                       return ids;
                                                   }));
}

void Client::addRoleMembers(Snowflake guildId, Snowflake roleId, const QList<Snowflake> &userIds, ResultCallback<QList<Snowflake>> callback)
{
    Core::OrderedJson body;
    body.insert("member_ids", snowflakeArray(userIds));

    QPointer<Client> self(this);
    auto counted = [self, guildId, roleId, callback = std::move(callback)](const Core::Result<QList<Snowflake>> &result) {
        if (self && result.success())
            emit self->roleMemberCountChanged(guildId, roleId, int(result.value->size()));
        callback(result);
    };
    httpClient->patch(roleEndpoint(guildId, roleId, "/members"), body,
                      applyThen(
                              [this, guildId](const QJsonDocument &doc) {
                                  const QJsonObject members = doc.object();
                                  for (auto it = members.constBegin(); it != members.constEnd(); ++it) {
                                      GuildMemberUpdate event;
                                      event.guildId = guildId;
                                      event.member = Member::fromJson(it.value().toObject());
                                      emit guildMemberUpdated(event);
                                  }
                              },
                              valueHandler<QList<Snowflake>>("Adding role members", std::move(counted), [](const QJsonDocument &doc) {
                                  QList<Snowflake> added;
                                  const QJsonObject members = doc.object();
                                  for (auto it = members.constBegin(); it != members.constEnd(); ++it)
                                      added.append(Snowflake(it.key().toULongLong()));
                                  return added;
                              })));
}

void Client::searchGuildMembers(Snowflake guildId, const MemberSearchQuery &query, ResultCallback<MemberSearchPage> callback)
{
    httpClient->post(guildEndpoint(guildId, "/members-search"), query.toJson(), [callback](const HttpResponse &response) {
        if (!response.success) {
            const ApiError error = ApiError::fromResponse(response);
            qCWarning(LogDiscord) << "Member search failed with status" << response.statusCode << ":" << error.message;
            callback(Core::Result<MemberSearchPage>::makeError(error.message, error.code));
            return;
        }

        const QJsonObject obj = QJsonDocument::fromJson(response.body).object();
        MemberSearchPage page;
        if (response.statusCode == 202) {
            page.indexingRetryAfterSeconds = qMax(1, qRound(obj.value("retry_after").toDouble(1.0)));
            callback(Core::Result<MemberSearchPage>::makeOk(page));
            return;
        }

        for (const QJsonValue &value : obj.value("members").toArray())
            page.members.append(MemberSearchResult::fromJson(value.toObject()));
        page.totalResultCount = obj.value("total_result_count").toInt();
        callback(Core::Result<MemberSearchPage>::makeOk(page));
    });
}

HttpCallback Client::memberUpdateHandler(Snowflake guildId, const char *what, ActionCallback callback)
{
    return applyThen(
            [this, guildId](const QJsonDocument &doc) {
                GuildMemberUpdate event;
                event.guildId = guildId;
                event.member = Member::fromJson(doc.object());
                emit guildMemberUpdated(event);
            },
            actionHandler(what, std::move(callback)));
}

HttpCallback Client::memberRemovalHandler(Snowflake guildId, Snowflake userId, const char *what, ActionCallback callback)
{
    return applyThen(
            [this, guildId, userId](const QJsonDocument &) {
                User user;
                user.id = userId;
                GuildMemberRemove event;
                event.guildId = guildId;
                event.user = user;
                emit guildMemberRemoved(event);
            },
            actionHandler(what, std::move(callback)));
}

void Client::setMemberRoles(Snowflake guildId, Snowflake userId, const QList<Snowflake> &roleIds,
                            const QList<Snowflake> &added, const QList<Snowflake> &removed, ActionCallback callback)
{
    Core::OrderedJson body;
    body.insert("roles", snowflakeArray(roleIds));

    QPointer<Client> self(this);
    auto counted = [self, guildId, added, removed, callback = std::move(callback)](const Core::Result<void> &result) {
        if (self && result.success()) {
            for (Snowflake roleId : added)
                emit self->roleMemberCountChanged(guildId, roleId, 1);
            for (Snowflake roleId : removed)
                emit self->roleMemberCountChanged(guildId, roleId, -1);
        }
        if (callback)
            callback(result);
    };
    httpClient->patch(memberEndpoint(guildId, userId), body, memberUpdateHandler(guildId, "Updating member roles", std::move(counted)));
}

void Client::setMemberNickname(Snowflake guildId, Snowflake userId, const QString &nick, ActionCallback callback)
{
    Core::OrderedJson body;
    body.insert("nick", nick);
    httpClient->patch(memberEndpoint(guildId, userId), body, memberUpdateHandler(guildId, "Changing a nickname", std::move(callback)));
}

void Client::setMemberTimeout(Snowflake guildId, Snowflake userId, const QDateTime &until, const std::optional<QString> &reason, ActionCallback callback)
{
    Core::OrderedJson body;
    body.insert("communication_disabled_until", until.isValid() ? QJsonValue(isoTimestamp(until)) : QJsonValue());

    RequestOptions options;
    options.auditLogReason = reason;
    httpClient->send(HttpClient::Method::PATCH, memberEndpoint(guildId, userId), body.toBytes(), options,
                     memberUpdateHandler(guildId, "Timing out a member", std::move(callback)));
}

void Client::kickMember(Snowflake guildId, Snowflake userId, const QString &reason, ActionCallback callback)
{
    const QString endpoint = memberEndpoint(guildId, userId) + "?reason=" + QString::fromLatin1(encodeUriComponent(reason));
    httpClient->delete_(endpoint, memberRemovalHandler(guildId, userId, "Kicking a member", std::move(callback)));
}

void Client::banMember(Snowflake guildId, Snowflake userId, int deleteMessageSeconds, const QString &reason, ActionCallback callback)
{
    Core::OrderedJson body;
    body.insert("delete_message_seconds", deleteMessageSeconds);

    RequestOptions options;
    options.auditLogReason = reason;
    httpClient->send(HttpClient::Method::PUT, guildEndpoint(guildId, "/bans/" + QString::number(userId)),
                     body.toBytes(), options,
                     memberRemovalHandler(guildId, userId, "Banning a member", std::move(callback)));
}

void Client::unbanMember(Snowflake guildId, Snowflake userId, ActionCallback callback)
{
    httpClient->delete_(guildEndpoint(guildId, "/bans/" + QString::number(userId)), actionHandler("Revoking a ban", std::move(callback)));
}

void Client::requestPruneCount(Snowflake guildId, int days, const QList<Snowflake> &includeRoles, ActionCallback callback)
{
    QUrlQuery query;
    query.addQueryItem("days", QString::number(days));
    for (Snowflake roleId : includeRoles)
        query.addQueryItem("include_roles", QString::number(roleId));
    httpClient->get(guildEndpoint(guildId, "/prune/v2"), query, actionHandler("Estimating a prune", std::move(callback)));
}

void Client::pruneMembers(Snowflake guildId, int days, const QList<Snowflake> &includeRoles, ActionCallback callback)
{
    Core::OrderedJson body;
    body.insert("days", days);
    body.insert("compute_prune_count", false);
    body.insert("include_roles", snowflakeArray(includeRoles));
    httpClient->post(guildEndpoint(guildId, "/prune"), body, actionHandler("Pruning members", std::move(callback)));
}

void Client::setMemberUpdatesSubscription(Snowflake guildId, bool subscribed)
{
    gateway->setMemberUpdatesSubscription(guildId, subscribed);
}

void Client::fetchBans(Snowflake guildId, Snowflake after, ResultCallback<QList<Ban>> callback)
{
    QUrlQuery query;
    query.addQueryItem("limit", "1000");
    if (after.isValid())
        query.addQueryItem("after", QString::number(after));
    httpClient->get(guildEndpoint(guildId, "/bans"), query, valueHandler<QList<Ban>>("Fetching bans", std::move(callback), parseArray<Ban>));
}

void Client::searchBans(Snowflake guildId, const QString &text, const QList<Snowflake> &userIds, ResultCallback<QList<Ban>> callback)
{
    QUrlQuery query;
    query.addQueryItem("limit", "10");
    for (Snowflake userId : userIds)
        query.addQueryItem("user_ids", QString::number(userId));
    if (!text.trimmed().isEmpty())
        query.addQueryItem("query", text);
    httpClient->get(guildEndpoint(guildId, "/bans/search"), query, valueHandler<QList<Ban>>("Searching bans", std::move(callback), parseArray<Ban>));
}

void Client::fetchGuildInvites(Snowflake guildId, ResultCallback<QList<Invite>> callback)
{
    httpClient->get(guildEndpoint(guildId, "/invites"), {}, valueHandler<QList<Invite>>("Fetching invites", std::move(callback), parseArray<Invite>));
}

void Client::createInvite(Snowflake channelId, int maxAgeSeconds, int maxUses, bool temporary, ResultCallback<Invite> callback)
{
    Core::OrderedJson body;
    body.insert("max_age", maxAgeSeconds);
    body.insert("max_uses", maxUses);
    body.insert("temporary", temporary);
    body.insert("flags", 0);

    RequestOptions options;
    options.context = ContextProperties::location("Settings Invite");
    QPointer<Client> self(this);
    auto announced = [self, callback = std::move(callback)](const Core::Result<Invite> &result) {
        if (self && result.success())
            emit self->inviteCreated(*result.value);
        if (callback)
            callback(result);
    };
    httpClient->send(HttpClient::Method::POST, "/channels/" + QString::number(channelId) + "/invites",
                     body.toBytes(), options,
                     valueHandler<Invite>("Creating an invite", std::move(announced),
                                          [](const QJsonDocument &doc) { return Invite::fromJson(doc.object()); }));
}

void Client::revokeInvite(const QString &code, ActionCallback callback)
{
    QPointer<Client> self(this);
    auto announced = [self, code, callback = std::move(callback)](const Core::Result<void> &result) {
        if (self && (result.success() || result.code == ApiError::UnknownInvite))
            emit self->inviteRevoked(code);
        if (callback)
            callback(result);
    };
    httpClient->delete_("/invites/" + QString::fromLatin1(QUrl::toPercentEncoding(code)), actionHandler("Revoking an invite", std::move(announced)));
}

void Client::setIncidentActions(Snowflake guildId, const QDateTime &invitesDisabledUntil,
                                const QDateTime &dmsDisabledUntil, std::optional<int> lockdownHours,
                                ActionCallback callback)
{
    Core::OrderedJson body;
    body.insert("invites_disabled_until", invitesDisabledUntil.isValid() ? QJsonValue(isoTimestamp(invitesDisabledUntil)) : QJsonValue());
    body.insert("dms_disabled_until", dmsDisabledUntil.isValid() ? QJsonValue(isoTimestamp(dmsDisabledUntil)) : QJsonValue());
    body.insert("lockdown_duration_hours", lockdownHours ? QJsonValue(*lockdownHours) : QJsonValue());
    httpClient->put(guildEndpoint(guildId, "/incident-actions"), body, actionHandler("Updating invite and DM pauses", std::move(callback)));
}

void Client::fetchGuildEmojis(Snowflake guildId, ResultCallback<QList<Emoji>> callback)
{
    httpClient->get(guildEndpoint(guildId, "/emojis"), {}, valueHandler<QList<Emoji>>("Fetching emojis", std::move(callback), parseArray<Emoji>));
}

void Client::createGuildEmoji(Snowflake guildId, const QString &name, const QString &imageDataUri,
                              const QByteArray &originalMd5, ResultCallback<Emoji> callback)
{
    Core::OrderedJson body;
    body.insert("image", imageDataUri);
    body.insert("name", name);

    RequestOptions options;
    options.context = ContextProperties().add("client_event_source", "Guild Settings");
    options.originalMd5 = originalMd5;
    httpClient->send(HttpClient::Method::POST, guildEndpoint(guildId, "/emojis"), body.toBytes(), options,
                     valueHandler<Emoji>("Uploading an emoji", std::move(callback),
                                         [](const QJsonDocument &doc) { return Emoji::fromJson(doc.object()); }));
}

void Client::renameGuildEmoji(Snowflake guildId, Snowflake emojiId, const QString &name, ActionCallback callback)
{
    Core::OrderedJson body;
    body.insert("name", name);
    httpClient->patch(guildEndpoint(guildId, "/emojis/" + QString::number(emojiId)), body, actionHandler("Renaming an emoji", std::move(callback)));
}

void Client::deleteGuildEmoji(Snowflake guildId, Snowflake emojiId, ActionCallback callback)
{
    httpClient->delete_(guildEndpoint(guildId, "/emojis/" + QString::number(emojiId)), actionHandler("Deleting an emoji", std::move(callback)));
}

void Client::fetchGuildStickers(Snowflake guildId, ResultCallback<QList<Sticker>> callback)
{
    httpClient->get(guildEndpoint(guildId, "/stickers"), {}, valueHandler<QList<Sticker>>("Fetching stickers", std::move(callback), parseArray<Sticker>));
}

void Client::createGuildSticker(Snowflake guildId, const QString &name, const QString &tags, const QString &description,
                                const FileUpload &file, const QByteArray &originalMd5, ResultCallback<Sticker> callback)
{
    FileUpload part = file;
    part.fieldName = "file";

    RequestOptions options;
    options.originalMd5 = originalMd5;
    httpClient->postForm(guildEndpoint(guildId, "/stickers"),
                         { { "name", name }, { "tags", tags }, { "description", description } }, { part }, options,
                         valueHandler<Sticker>("Uploading a sticker", std::move(callback),
                                               [](const QJsonDocument &doc) { return Sticker::fromJson(doc.object()); }));
}

void Client::modifyGuildSticker(Snowflake guildId, Snowflake stickerId, const QString &name, const QString &tags,
                                const QString &description, ActionCallback callback)
{
    Core::OrderedJson body;
    body.insert("name", name);
    body.insert("tags", tags);
    body.insert("description", description);
    httpClient->patch(guildEndpoint(guildId, "/stickers/" + QString::number(stickerId)), body, actionHandler("Editing a sticker", std::move(callback)));
}

void Client::deleteGuildSticker(Snowflake guildId, Snowflake stickerId, ActionCallback callback)
{
    httpClient->delete_(guildEndpoint(guildId, "/stickers/" + QString::number(stickerId)), actionHandler("Deleting a sticker", std::move(callback)));
}

void Client::fetchAuditLog(Snowflake guildId, const AuditLogQuery &auditQuery, ResultCallback<AuditLog> callback)
{
    QUrlQuery query;
    query.addQueryItem("limit", QString::number(AuditLogQuery::PageSize));
    if (auditQuery.before.isValid())
        query.addQueryItem("before", QString::number(auditQuery.before));
    if (auditQuery.userId.isValid())
        query.addQueryItem("user_id", QString::number(auditQuery.userId));
    if (auditQuery.actionType)
        query.addQueryItem("action_type", QString::number(static_cast<int>(*auditQuery.actionType)));
    httpClient->get(guildEndpoint(guildId, "/audit-logs"), query,
                    valueHandler<AuditLog>("Fetching the audit log", std::move(callback),
                                           [](const QJsonDocument &doc) { return AuditLog::fromJson(doc.object()); }));
}

void Client::sendFriendRequest(const QString &username, ActionCallback callback)
{
    QJsonObject body;
    int hashIdx = username.lastIndexOf('#');
    if (hashIdx != -1) {
        body["username"] = username.left(hashIdx);
        body["discriminator"] = username.mid(hashIdx + 1).toInt();
    } else {
        body["username"] = username;
    }
    httpClient->post("/users/@me/relationships", body, actionHandler("Sending a friend request", std::move(callback)));
}


void Client::setState(Core::ConnectionState state)
{
    if (this->state != state) {
        this->state = state;
        emit stateChanged(state);
    }
}

} // namespace Discord
} // namespace Styx