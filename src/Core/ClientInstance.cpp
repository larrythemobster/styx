#include "ClientInstance.hpp"
#include "ReadStateManager.hpp"
#include "Calls/CallStore.hpp"
#ifndef STYX_NO_VOICE
#include "Core/Audio/VoiceManager.hpp"
#endif
#ifdef STYX_HAVE_SCREENSHARE
#include "Core/Stream/StreamManager.hpp"
#endif

#include <algorithm>

#include <chrono>
#include <cstdint>
#include "Platform/Desktop.hpp"
#include "Platform/Dispatcher.hpp"

#include "Core/Logging.hpp"
#include "ForumManager.hpp"
#include "Discord/Enums.hpp"
#include "Storage/DatabaseManager.hpp"
#include "Storage/GuildRepository.hpp"
#include "Storage/ChannelRepository.hpp"
#include "Storage/RoleRepository.hpp"
#include "Storage/MemberRepository.hpp"
#include "Storage/Transaction.hpp"

namespace Styx {
namespace Core {

namespace {

void removeBanOf(std::vector<Discord::Ban> &bans, Snowflake userId)
{
    bans.erase(std::remove_if(bans.begin(), bans.end(), [userId](const Discord::Ban &ban) { return ban.user->id.get() == userId; }), bans.end());
}

void removeInvite(std::vector<Discord::Invite> &invites, const std::string &code)
{
    invites.erase(std::remove_if(invites.begin(), invites.end(), [&code](const Discord::Invite &invite) { return invite.code.get() == code; }), invites.end());
}

} // namespace

ClientInstance::ClientInstance(const AccountInfo &info,
                               Discord::CaptchaResolver *captchaResolver)
    : account(info),
      roleRepo(info.id),
      guildRepo(info.id),
      channelRepo(info.id),
      memberRepo(info.id)
{
    client = std::make_unique<Discord::Client>(info.token, info.gatewayUrl, info.restUrl, info.proxy, captchaResolver);
    track(client->heartbeatSessionChanged.connect([this](auto &&...args) { onHeartbeatSessionChanged(args...); }));
    client->restoreHeartbeatSession(accountRepo.getHeartbeatSession(info.id));

    Storage::DatabaseManager::instance().openCacheDatabase(info.id);

    userManager = std::make_unique<UserManager>(info.id);
    messageManager = std::make_unique<MessageManager>(info.id, client.get(), userManager.get());
    messageManager->setChannelResolver([this](Snowflake channelId) -> std::string {
        auto ch = channelRepo.getChannel(channelId);
        if (ch && ch->name.hasValue())
            return ch->name.get();
        return channelId.toString();
    });
    messageManager->setChannelLinkResolver([this](const Markdown::ChannelLinkRef &ref) {
        return channelLinkParts(ref);
    });

    permissionManager = std::make_unique<PermissionManager>(info.id);
    readStateManager = std::make_unique<ReadStateManager>(info.id, permissionManager.get());
    forumManager = std::make_unique<ForumManager>(client.get(), channelRepo, readStateManager.get());
    memberListManager = std::make_unique<MemberListManager>(channelRepo, roleRepo);
    relationshipManager = std::make_unique<RelationshipManager>();
    userManager->setRelationshipManager(relationshipManager.get());
    presenceManager = std::make_unique<PresenceManager>();
    emojiManager = std::make_unique<EmojiManager>(client.get(), this);
    favoriteGifManager = std::make_unique<FavoriteGifs>(client.get());
    messageManager->setEmojiManager(emojiManager.get());

    callStore = std::make_unique<CallStore>();
    commandIndexCache = std::make_unique<CommandIndexCache>(
        [discord = client.get()](CommandIndexCache::Scope scope, CommandIndexCache::FetchCallback done) {
            switch (scope.kind) {
            case CommandIndexCache::ScopeKind::Guild:
                discord->fetchGuildCommandIndex(scope.id, std::move(done));
                break;
            case CommandIndexCache::ScopeKind::Channel:
                discord->fetchChannelCommandIndex(scope.id, std::move(done));
                break;
            case CommandIndexCache::ScopeKind::User:
                discord->fetchUserCommandIndex(std::move(done));
                break;
            }
        });
    // A new session may come with different apps installed.
    track(client->ready.connect([this](const auto &) { commandIndexCache->clear(); }));
    track(client->callCreated.connect([this](auto &&...args) { onCallCreated(args...); }));
    track(client->callUpdated.connect([this](const Discord::CallUpdate &event) {
        callStore->handleCallUpdate(event);
    }));
    track(client->callDeleted.connect([this](auto &&...args) { onCallDeleted(args...); }));
#ifndef STYX_NO_VOICE
    voiceManager = std::make_unique<Audio::VoiceManager>(info.id, info.proxy);
    track(voiceManager->voiceConnected.connect([this](auto &&...args) { onVoiceConnected(args...); }));
    track(voiceManager->voiceDisconnected.connect([this](auto &&...args) { onVoiceDisconnected(args...); }));
#endif
#ifdef STYX_HAVE_SCREENSHARE
    streamManager = std::make_unique<Stream::StreamManager>(info.id, info.proxy, client.get(), voiceManager.get());
    track(client->streamCreated.connect([this](auto &&...args) { streamManager->handleStreamCreate(args...); }));
    track(client->streamServerUpdated.connect([this](auto &&...args) { streamManager->handleStreamServerUpdate(args...); }));
    track(client->streamDeleted.connect([this](auto &&...args) { streamManager->handleStreamDelete(args...); }));
    track(voiceManager->voiceDisconnected.connect([this](auto &&...args) { streamManager->stopAll(args...); }));
    // a watched streamer who stops sharing leaves the call or flips self_stream off
    track(voiceManager->participantLeft.connect([this](Snowflake userId) {
        if (streamManager->isWatching(userId))
            streamManager->stopWatching(userId);
    }));
#endif

    track(client->stateChanged.connect([this](auto &&...args) { stateChanged.publish(args...); }));
    track(client->reconnecting.connect([this](auto &&...args) { reconnecting.publish(args...); }));
    track(client->authenticationFailed.connect([this]() { authenticationFailed.publish(account); }));
    track(client->currentUserUpdated.connect([this](const Discord::User &user) {
        if (user.id != account.id)
            return;
        userManager->saveUser(user);
        account.username = user.username.valueOr();
        account.displayName = user.globalName.valueOr();
        account.avatar = user.avatar.valueOr();
        detailsUpdated.publish(account);
    }));

    track(client->ready.connect([this](const Discord::Ready &ready) {
        auto &db = Storage::DatabaseManager::instance().getDatabase(Storage::DatabaseManager::instance().getCacheConnectionName(account.id));
        gatewaySessionId = ready.sessionId.valueOr(std::string());
        callStore->reset();
        connectSelectedCall(true);

        account.username = ready.user->username;
        account.displayName = ready.user->globalName;
        account.avatar = ready.user->avatar;

        std::vector<Snowflake> guildIds;
        guildIds.reserve(ready.guilds->size());

        permissionManager->setSelfMfaEnabled(ready.user->mfaEnabled.valueOr(true));

        Storage::Transaction txn(db);
        for (size_t i = 0; i < ready.guilds->size(); i++) {
            const auto &guild = ready.guilds->at(i);
            const std::vector<Discord::Member> *members = ready.mergedMembers.hasValue() ? &ready.mergedMembers->at(i) : nullptr;
            saveGuild(guild, members, ready.user->id.get(), db);
            guildIds.push_back(guild.properties->id);
        }

        userManager->saveUser(ready.user);

        if (ready.users.hasValue())
            userManager->saveUsers(ready.users.get());

        if (ready.relationships.hasValue())
            relationshipManager->loadFromReady(ready.relationships.get());
        userManager->loadNotesFromReady(ready.notes.get());

        if (ready.privateChannels.hasValue()) {
            for (const auto &channel : ready.privateChannels.get()) {
                channelRepo.saveChannel(channel, db);

                std::vector<Core::Snowflake> recipientIds;
                if (channel.recipients.hasValue()) {
                    for (const auto &user : channel.recipients.get()) {
                        userManager->saveUser(user);
                        recipientIds.push_back(user.id.get());
                    }
                } else if (channel.recipientIds.hasValue()) {
                    recipientIds = channel.recipientIds.get();
                }

                if (!recipientIds.empty())
                    channelRepo.saveChannelRecipients(channel.id.get(), recipientIds, db);
            }
        }

        txn.commit();

        presenceManager->setSelfUserId(ready.user->id.get());
        presenceManager->reset();
        presenceManager->applyProtoStatus(client->getSettings());
        if (ready.mergedPresences.hasValue())
            presenceManager->loadMergedPresences(ready.mergedPresences.get(), guildIds);

        readStateManager->loadFromReady(
                ready.readState.hasValue() ? ready.readState.get()
                                           : std::vector<Discord::ReadStateEntry>{},
                ready.userGuildSettings.hasValue() ? ready.userGuildSettings.get()
                                                   : std::vector<Discord::UserGuildSettings>{},
                ready.notificationSettings.hasValue() && ready.notificationSettings->flags.hasValue()
                        ? ready.notificationSettings->flags.get()
                        : 0);

        for (const auto &guild : ready.guilds.get())
            initGuildReadState(guild);

        forumManager->loadFromReady(ready.guilds.get());

        if (ready.privateChannels.hasValue()) {
            for (const auto &channel : ready.privateChannels.get())
                readStateManager->registerPrivateChannel(channel);
        }

        detailsUpdated.publish(account);
        this->ready.publish(ready);
    }));

    track(client->readySupplemental.connect([this](const Discord::ReadySupplemental &data) {
                auto &db = Storage::DatabaseManager::instance().getDatabase(Storage::DatabaseManager::instance().getCacheConnectionName(account.id));

                Storage::Transaction txn(db);

                for (size_t i = 0; i < data.mergedMembers->size(); i++) {
                    const auto &guild = data.guilds->at(i);
                    const auto &members = data.mergedMembers->at(i);

                    for (const auto &member : members)
                        memberRepo.saveMember(guild.id, member.userId.get(), member);
                }

                txn.commit();

                if (data.mergedPresences.hasValue()) {
                    std::vector<Snowflake> guildIds;
                    guildIds.reserve(data.guilds->size());
                    for (const auto &guild : data.guilds.get())
                        guildIds.push_back(guild.id.get());
                    presenceManager->loadMergedPresences(data.mergedPresences.get(), guildIds);
                }

#ifndef STYX_NO_VOICE
                for (const auto &guild : data.guilds.get()) {
                    if (!guild.voiceStates.hasValue())
                        continue;
                    for (auto vs : guild.voiceStates.get()) {
                        if (!vs.guildId.hasValue())
                            vs.guildId = guild.id.get();
                        voiceManager->handleVoiceStateUpdate(vs);
                    }
                }
#endif
            }));

    track(client->messageCreated.connect([this](auto &&...args) { messageManager->onMessageCreated(args...); }));
    track(client->messageUpdated.connect([this](auto &&...args) { messageManager->onMessageUpdated(args...); }));
    track(client->messageDeleted.connect([this](auto &&...args) { messageManager->onMessageDeleted(args...); }));
    track(client->messageSendFailed.connect([this](auto &&...args) { messageManager->onMessageSendFailed(args...); }));
    track(client->attachmentUploadProgress.connect([this](auto &&...args) { messageManager->attachmentUploadProgress.publish(args...); }));
    track(client->messageReactionAdd.connect([this](auto &&...args) { messageManager->onReactionAdd(args...); }));
    track(client->messageReactionAddMany.connect([this](auto &&...args) { messageManager->onReactionAddMany(args...); }));
    track(client->messageReactionRemove.connect([this](auto &&...args) { messageManager->onReactionRemove(args...); }));
    track(client->messageReactionRemoveAll.connect([this](auto &&...args) { messageManager->onReactionRemoveAll(args...); }));
    track(client->messageReactionRemoveEmoji.connect([this](auto &&...args) { messageManager->onReactionRemoveEmoji(args...); }));
    track(client->relationshipAdded.connect([this](auto &&...args) { relationshipManager->onRelationshipAdded(args...); }));
    track(client->relationshipUpdated.connect([this](auto &&...args) { relationshipManager->onRelationshipUpdated(args...); }));
    track(client->relationshipRemoved.connect([this](auto &&...args) { relationshipManager->onRelationshipRemoved(args...); }));
    track(client->userNoteUpdated.connect([this](const Discord::UserNoteUpdate &event) {
                if (event.id.hasValue() && event.note.hasValue())
                    userManager->setCachedNote(event.id.get(), event.note.get());
            }));

    track(client->ready.connect([this](const auto &) { emojiManager->onReady(); }));
    track(client->guildEmojisUpdated.connect([this](auto &&...args) { emojiManager->onGuildEmojisUpdated(args...); }));
    track(client->userSettingsProtoUpdated.connect([this](auto &&...args) { emojiManager->onUserSettingsProtoUpdated(args...); }));
    track(client->frecencySettingsReceived.connect([this](auto &&...args) { emojiManager->onFrecencySettingsReceived(args...); }));
    track(client->userSettingsProtoUpdated.connect([this](auto &&...args) { favoriteGifManager->onUserSettingsProtoUpdated(args...); }));
    track(client->frecencySettingsReceived.connect([this](auto &&...args) { favoriteGifManager->onFrecencySettingsReceived(args...); }));
    track(client->settingsChanged.connect([this]() { presenceManager->applyProtoStatus(client->getSettings()); }));

    track(client->presenceUpdated.connect([this](auto &&...args) { presenceManager->onPresenceUpdate(args...); }));
    track(client->presencesReplaced.connect([this](auto &&...args) { presenceManager->onPresencesReplace(args...); }));
    track(client->sessionsReplaced.connect([this](auto &&...args) { presenceManager->onSessionsReplace(args...); }));
    track(client->guildCreated.connect([this](auto &&...args) { presenceManager->onGuildCreated(args...); }));
    track(client->guildMembersChunk.connect([this](auto &&...args) { presenceManager->onGuildMembersChunk(args...); }));

    track(client->guildCreated.connect([this](auto &&...args) { onGuildCreated(args...); }));
    track(client->guildUpdated.connect([this](auto &&...args) { onGuildUpdated(args...); }));
    track(client->guildDeleted.connect([this](auto &&...args) { onGuildDeleted(args...); }));
    track(client->channelCreated.connect([this](auto &&...args) { onChannelCreated(args...); }));
    track(client->channelUpdated.connect([this](auto &&...args) { onChannelUpdated(args...); }));
    track(client->channelDeleted.connect([this](auto &&...args) { onChannelDeleted(args...); }));
    track(client->guildRoleCreated.connect([this](auto &&...args) { onGuildRoleCreated(args...); }));
    track(client->guildRoleUpdated.connect([this](auto &&...args) { onGuildRoleUpdated(args...); }));
    track(client->guildRoleDeleted.connect([this](auto &&...args) { onGuildRoleDeleted(args...); }));
    track(client->guildMembersChunk.connect([this](auto &&...args) { onGuildMembersChunk(args...); }));
    track(client->guildMemberAdded.connect([this](auto &&...args) { onGuildMemberAdded(args...); }));
    track(client->guildMemberUpdated.connect([this](auto &&...args) { onGuildMemberUpdate(args...); }));
    track(client->guildMemberRemoved.connect([this](auto &&...args) { onGuildMemberRemoved(args...); }));
    track(client->guildBanAdded.connect([this](auto &&...args) { onGuildBanAdded(args...); }));
    track(client->guildBanRemoved.connect([this](auto &&...args) { onGuildBanRemoved(args...); }));
    track(client->inviteCreated.connect([this](auto &&...args) { onInviteCreated(args...); }));
    track(client->inviteRevoked.connect([this](auto &&...args) { onInviteRevoked(args...); }));
    track(client->guildMemberListUpdate.connect([this](auto &&...args) { presenceManager->onGuildMemberListUpdate(args...); }));
    track(client->guildMemberListUpdate.connect([this](auto &&...args) { memberListManager->handleMemberListUpdate(args...); }));
    track(client->guildMemberListUpdate.connect([this](auto &&...args) { onGuildMemberListUpdate(args...); }));
    track(memberListManager->subscriptionRequested.connect([this](auto &&...args) { client->subscribeToGuildChannel(args...); }));
    track(messageManager->messagesReceived.connect([this](auto &&...args) { onMessagesReceived(args...); }));
    track(messageManager->membersLearnedFromMessage.connect([this](auto &&...args) { membersUpdated.publish(args...); }));

    track(client->messageAcked.connect([this](auto &&...args) { readStateManager->onMessageAck(args...); }));
    track(client->userGuildSettingsUpdated.connect([this](auto &&...args) { readStateManager->onUserGuildSettingsUpdate(args...); }));
    track(client->notificationSettingsUpdated.connect([this](auto &&...args) { readStateManager->onNotificationSettingsUpdate(args...); }));

    track(client->messageCreated.connect([this](auto &&...args) { onMessageCreated(args...); }));

    track(client->threadCreated.connect([this](auto &&...args) { forumManager->onThreadCreated(args...); }));
    track(client->threadUpdated.connect([this](auto &&...args) { forumManager->onThreadUpdated(args...); }));
    track(client->threadDeleted.connect([this](auto &&...args) { forumManager->onThreadDeleted(args...); }));
    track(client->threadListSync.connect([this](auto &&...args) { forumManager->onThreadListSync(args...); }));
    track(client->threadMemberUpdated.connect([this](auto &&...args) { forumManager->onThreadMemberUpdate(args...); }));
    track(client->forumUnreads.connect([this](auto &&...args) { forumManager->onForumUnreads(args...); }));
    track(client->messageCreated.connect([this](auto &&...args) { forumManager->onMessageCreated(args...); }));

    track(client->threadCreated.connect([this](auto &&...args) { onThreadCreated(args...); }));
    track(client->threadUpdated.connect([this](auto &&...args) { onThreadUpdated(args...); }));
    track(client->threadDeleted.connect([this](auto &&...args) { onThreadDeleted(args...); }));
    track(client->threadListSync.connect([this](auto &&...args) { onThreadListSync(args...); }));
    track(client->threadMemberUpdated.connect([this](auto &&...args) { onThreadMemberUpdate(args...); }));
    track(client->threadMembersUpdated.connect([this](auto &&...args) { onThreadMembersUpdate(args...); }));

    track(forumManager->badgeChanged.connect([this](auto &&...args) { forumBadgeChanged.publish(args...); }));
    track(forumManager->joinedPostsChanged.connect([this](auto &&...args) { forumJoinedPostsChanged.publish(args...); }));

    track(readStateManager->readStateUpdated.connect([this](auto &&...args) { readStateChanged.publish(args...); }));
    track(readStateManager->guildSettingsUpdated.connect([this](auto &&...args) { guildSettingsChanged.publish(args...); }));

    track(readStateManager->ackRequested.connect([this](auto &&...args) { handleAckRequest(args...); }));
    track(readStateManager->bulkAckRequested.connect([this](auto &&...args) { handleBulkAckRequest(args...); }));

    track(client->voiceStateUpdated.connect([this](auto &&...args) { onVoiceStateUpdated(args...); }));

#ifndef STYX_NO_VOICE
    track(client->voiceServerUpdated.connect([this](const Discord::VoiceServerUpdate &event) {
                log(LogLevel::Info, LogVoice) << "VOICE_SERVER_UPDATE: guild" << event.guildId.get()
                                 << "endpoint" << (event.endpoint.isNull() ? "null" : event.endpoint.get())
                                 << "token" << event.token.get().substr(0, 8) + "...";

                voiceManager->handleVoiceServerUpdate(event);
            }));
#endif
}

void ClientInstance::onVoiceConnected()
{
    client->setVoiceConnected(true);
}

void ClientInstance::onVoiceDisconnected()
{
    client->setVoiceConnected(false);
}

void ClientInstance::onCallCreated(const Discord::CallCreate &event)
{
    callStore->handleCallCreate(event);

    if (!event.voiceStates.hasValue())
        return;

    for (Discord::VoiceState state : event.voiceStates.get()) {
        if (state.channelId.isNull() || !state.channelId.get().isValid())
            state.channelId = event.channelId.get();
        onVoiceStateUpdated(state);
    }
}

void ClientInstance::onCallDeleted(const Discord::CallDelete &event)
{
    callStore->handleCallDelete(event);
}

void ClientInstance::onVoiceStateUpdated(const Discord::VoiceState &event)
{
    log(LogLevel::Info, LogVoice) << "VOICE_STATE_UPDATE: user" << event.userId.get()
                     << "channel" << (event.channelId.isNull() ? 0 : static_cast<std::uint64_t>(event.channelId.get()))
                     << "session" << event.sessionId.get()
                     << "mute" << event.selfMute.get()
                     << "deaf" << event.selfDeaf.get();

    // track our own voice state
    if (event.userId.get() == account.id) {
        if (event.channelId.isNull() || !event.channelId.get().isValid()) {
            currentVoiceChannelId = Snowflake::Invalid;
            currentVoiceGuildId = Snowflake::Invalid;
        } else {
            currentVoiceChannelId = event.channelId.get();
            currentVoiceGuildId = event.guildId.hasValue() ? event.guildId.get() : Snowflake::Invalid;
        }
        voiceStateChanged.publish(currentVoiceChannelId, currentVoiceGuildId);
    }

#ifndef STYX_NO_VOICE
    voiceManager->handleVoiceStateUpdate(event);
#endif
}

void ClientInstance::noteChannelSelected(Snowflake channelId)
{
    selectedChannelId = channelId;
    connectSelectedCall(false);
}

void ClientInstance::connectSelectedCall(bool force)
{
    if (!selectedChannelId.isValid() || gatewaySessionId.empty())
        return;

    const auto channel = getChannel(selectedChannelId);
    if (!channel || (channel->type.get() != Discord::ChannelType::DM && channel->type.get() != Discord::ChannelType::GROUP_DM))
        return;

    if (callStore->shouldConnect(selectedChannelId, force))
        client->sendCallConnect(selectedChannelId);
}

void ClientInstance::onHeartbeatSessionChanged(const Discord::HeartbeatSession &session)
{
    accountRepo.updateHeartbeatSession(account.id, session);
}

void ClientInstance::saveGuild(const Discord::GatewayGuild &guild, const std::vector<Discord::Member> *members, Snowflake myId, Platform::Database &db)
{
    Snowflake guildId = guild.properties->id.get();

    const Discord::Member *me = nullptr;
    if (members) {
        for (const auto &member : *members) {
            Snowflake memberId = member.userId.hasValue()
                                         ? member.userId.get()
                                         : (member.user.hasValue() ? member.user->id.get()
                                                                   : Snowflake::Invalid);
            if (!memberId.isValid())
                continue;
            if (member.user.hasValue())
                userManager->saveUser(member.user.get());
            memberRepo.saveMember(guildId, memberId, member);
            if (memberId == myId)
                me = &member;
        }
    }

    if (me && guild.roles.hasValue() && guild.channels.hasValue())
        permissionManager->precomputeGuildPermissions(guild.asGuild(), *me, guild.roles.get(), guild.channels.get(), myId);

    guildRepo.saveGuild(guild.asGuild(), db);

    if (guild.emojis.hasValue())
        emojiManager->setGuildEmojis(guildId, guild.emojis.get());

    if (guild.roles.hasValue())
        roleRepo.saveRoles(guildId, guild.roles.get(), db);

    if (guild.channels.hasValue()) {
        for (const auto &channel : guild.channels.get()) {
            // we dont get a guild_id from the gateway here
            Discord::Channel copy = channel;
            copy.guildId = guildId;
            channelRepo.saveChannel(copy, db);

            if (copy.permissionOverwrites.hasValue())
                channelRepo.savePermissionOverwrites(copy.id.get(), copy.permissionOverwrites.get(), db);
        }
    }
}

void ClientInstance::initGuildReadState(const Discord::GatewayGuild &guild)
{
    const Discord::Guild &props = guild.properties.get();
    Snowflake guildId = props.id.get();

    readStateManager->setGuildReadInfo(
            guildId,
            guild.joinedAt.hasValue() ? guild.joinedAt.get() : Platform::DateTime(),
            props.defaultMessageNotifications.hasValue()
                    ? props.defaultMessageNotifications.get()
                    : Discord::MessageNotificationLevel::ALL_MESSAGES,
            props.hasFeature(("COMMUNITY")));

    if (guild.channels.hasValue()) {
        for (const auto &channel : guild.channels.get()) {
            readStateManager->registerChannel(channel, guildId);
            if (channel.lastMessageId.hasValue())
                readStateManager->updateChannelLastMessageId(channel.id.get(), channel.lastMessageId.get());
        }
    }

    if (guild.threads.hasValue()) {
        for (const auto &thread : guild.threads.get()) {
            if (isForumParent(thread.parentId.hasValue() ? thread.parentId.get() : Snowflake()))
                registerThreadReadState(thread, guildId);
            else
                ingestThread(thread, guildId);
        }
    }
}

void ClientInstance::onGuildCreated(const Discord::GatewayGuild &guild)
{
    if (!guild.properties.hasValue())
        return;

    auto &db = Storage::DatabaseManager::instance().getDatabase(Storage::DatabaseManager::instance().getCacheConnectionName(account.id));

    Storage::Transaction txn(db);
    saveGuild(
            guild,
            guild.members.hasValue() ? &guild.members.get() : nullptr,
            client->getMe().id.get(),
            db);
    txn.commit();

    initGuildReadState(guild);

    guildCreated.publish(guild);
}

void ClientInstance::onGuildUpdated(const Discord::Guild &guild)
{
    const Snowflake guildId = guild.id.get();
    auto previous = guildRepo.getGuild(guildId);
    if (!previous)
        return;

    if (!runInCacheTransaction("guild update", [&](Platform::Database &db) { guildRepo.saveGuild(guild, db); }))
        return;

    if (previous->ownerId.get() != guild.ownerId.get() ||
        previous->mfaLevel.valueOr(Discord::MfaLevel::NONE) != guild.mfaLevel.valueOr(Discord::MfaLevel::NONE))
        permissionManager->invalidateUserGuildCache(account.id, guildId);

    readStateManager->updateGuildDefaults(
            guildId,
            guild.defaultMessageNotifications.valueOr(Discord::MessageNotificationLevel::ALL_MESSAGES),
            guild.hasFeature(("COMMUNITY")));

    guildUpdated.publish(guild);
}

void ClientInstance::onGuildDeleted(const Discord::GuildDelete &event)
{
    if (!event.id.hasValue() || !event.userRemoved())
        return;

    Snowflake guildId = event.id.get();

    log(LogLevel::Info, LogCore) << "Removed from guild:" << guildId;

    presenceManager->onGuildRemoved(guildId);

    std::vector<Discord::Channel> channels = channelRepo.getChannelsForGuild(guildId);

    if (!runInCacheTransaction("guild deletion", [&](Platform::Database &db) {
            for (const auto &channel : channels)
                channelRepo.deleteChannel(channel.id.get(), db);
            roleRepo.deleteRolesForGuild(guildId, db);
            memberRepo.deleteMembersForGuild(guildId, db);
            guildRepo.deleteGuild(guildId, db);
        }))
        return;

    for (const auto &channel : channels) {
        Snowflake channelId = channel.id.get();
        permissionManager->invalidateChannelCache(channelId);
        forumParentCache.erase(channelId);
    }

    readStateManager->removeGuild(guildId);
    emojiManager->removeGuild(guildId);
    userManager->removeGuildMembers(guildId);
    memberListManager->clearGuild(guildId);
    permissionManager->invalidateUserGuildCache(account.id, guildId);
    rolesCacheByGuild.erase(guildId);
    observedBansByGuild.erase(guildId);
    createdInvitesByGuild.erase(guildId);

    guildRemoved.publish(guildId);
}

void ClientInstance::onChannelCreated(const Discord::ChannelCreate &event)
{
    if (!event.channel.hasValue())
        return;

    const Discord::Channel &channel = event.channel.get();
    Core::Snowflake channelId = channel.id.get();

    auto &db = Storage::DatabaseManager::instance().getDatabase(Storage::DatabaseManager::instance().getCacheConnectionName(account.id));

    Storage::Transaction txn(db);

    channelRepo.saveChannel(channel, db);

    if (channel.permissionOverwrites.hasValue())
        channelRepo.savePermissionOverwrites(channelId, channel.permissionOverwrites.get(), db);

    if (channel.type == Discord::ChannelType::DM || channel.type == Discord::ChannelType::GROUP_DM) {
        std::vector<Core::Snowflake> recipientIds;
        if (channel.recipients.hasValue()) {
            for (const auto &user : channel.recipients.get()) {
                userManager->saveUser(user);
                recipientIds.push_back(user.id.get());
            }
        } else if (channel.recipientIds.hasValue()) {
            recipientIds = channel.recipientIds.get();
        }

        if (!recipientIds.empty())
            channelRepo.saveChannelRecipients(channelId, recipientIds, db);
    }

    txn.commit();

    if (channel.type == Discord::ChannelType::DM || channel.type == Discord::ChannelType::GROUP_DM) {
        readStateManager->registerPrivateChannel(channel);
    } else {
        if (channel.guildId.hasValue())
            readStateManager->registerChannel(channel, channel.guildId.get());
        if (channel.lastMessageId.hasValue())
            readStateManager->updateChannelLastMessageId(channelId, channel.lastMessageId.get());
    }

    forumParentCache.erase(channelId);

    channelCreated.publish(event);
}

void ClientInstance::onChannelUpdated(const Discord::ChannelUpdate &event)
{
    if (!event.channel.hasValue())
        return;

    const Discord::Channel &channel = event.channel.get();
    Core::Snowflake channelId = channel.id.get();

    log(LogLevel::Debug, LogCore) << "Channel updated:" << channelId;

    auto &db = Storage::DatabaseManager::instance().getDatabase(Storage::DatabaseManager::instance().getCacheConnectionName(account.id));

    Storage::Transaction txn(db);

    channelRepo.saveChannel(channel, db);

    if (channel.permissionOverwrites.hasValue())
        channelRepo.savePermissionOverwrites(channelId, channel.permissionOverwrites.get(), db);

    txn.commit();

    permissionManager->invalidateChannelCache(channelId);
    forumParentCache.erase(channelId);

    if (channel.guildId.hasValue()) {
        readStateManager->registerChannel(channel, channel.guildId.get());
        readStateChanged.publish(channelId);
    } else if (channel.type == Discord::ChannelType::DM || channel.type == Discord::ChannelType::GROUP_DM) {
        readStateManager->registerPrivateChannel(channel);
        readStateChanged.publish(channelId);
    }

    channelUpdated.publish(event);
}

void ClientInstance::onChannelDeleted(const Discord::ChannelDelete &event)
{
    if (!event.id.hasValue())
        return;

    Core::Snowflake channelId = event.id.get();
    callStore->forget(channelId);

    log(LogLevel::Debug, LogCore) << "Channel deleted:" << channelId;

    auto &db = Storage::DatabaseManager::instance().getDatabase(Storage::DatabaseManager::instance().getCacheConnectionName(account.id));

    Storage::Transaction txn(db);

    channelRepo.deleteChannel(channelId, db);

    txn.commit();

    permissionManager->invalidateChannelCache(channelId);
    forumParentCache.erase(channelId);

    channelDeleted.publish(event);
}

bool ClientInstance::isForumParent(Snowflake parentId)
{
    if (!parentId.isValid())
        return false;
    auto it = forumParentCache.find(parentId);
    if (it != forumParentCache.end())
        return it->second;
    auto ch = channelRepo.getChannel(parentId);
    bool isForum = ch && ch->type.hasValue() &&
                   (ch->type.get() == Discord::ChannelType::GUILD_FORUM ||
                    ch->type.get() == Discord::ChannelType::GUILD_MEDIA);
    forumParentCache.insert_or_assign(parentId, isForum);
    return isForum;
}

void ClientInstance::registerThreadReadState(const Discord::Channel &thread, Snowflake guildId)
{
    if (guildId.isValid())
        readStateManager->registerChannelGuild(thread.id.get(), guildId);
    if (thread.lastMessageId.hasValue())
        readStateManager->updateChannelLastMessageId(thread.id.get(), thread.lastMessageId.get());
}

void ClientInstance::cacheThread(const Discord::Channel &thread, Snowflake guildId)
{
    Discord::Channel copy = thread;
    if (!copy.guildId.hasValue() && guildId.isValid())
        copy.guildId = guildId;
    threadCache.insert_or_assign(copy.id.get(), copy);
    const Snowflake resolvedGuildId = copy.guildId.hasValue() ? copy.guildId.get() : guildId;
    client->registerChannelGuild(copy.id.get(), resolvedGuildId);
    registerThreadReadState(copy, resolvedGuildId);
}

void ClientInstance::ingestThread(const Discord::Channel &thread, Snowflake guildId)
{
    cacheThread(thread, guildId);
    if (thread.member.hasValue())
        joinedThreads.insert(thread.id.get());
}

void ClientInstance::onThreadCreated(const Discord::ChannelCreate &event)
{
    if (!event.channel.hasValue())
        return;
    const Discord::Channel &thread = event.channel.get();
    if (!thread.parentId.hasValue() || isForumParent(thread.parentId.get()))
        return;

    Snowflake threadId = thread.id.get();
    Snowflake guildId = thread.guildId.hasValue() ? thread.guildId.get() : Snowflake::Invalid;
    ingestThread(thread, guildId);

    bool isMember = thread.member.hasValue() || (thread.ownerId.hasValue() && thread.ownerId.get() == account.id);
    if (isMember) {
        joinedThreads.insert(threadId);
        threadCreated.publish(thread);
    }
}

void ClientInstance::onThreadUpdated(const Discord::ChannelUpdate &event)
{
    if (!event.channel.hasValue())
        return;
    const Discord::Channel &thread = event.channel.get();
    if (!thread.parentId.hasValue() || isForumParent(thread.parentId.get()))
        return;

    Snowflake guildId = thread.guildId.hasValue() ? thread.guildId.get() : Snowflake::Invalid;
    ingestThread(thread, guildId);

    threadUpdated.publish(thread);
}

void ClientInstance::onThreadDeleted(const Discord::ThreadDelete &event)
{
    if (!event.id.hasValue())
        return;
    Snowflake parentId = event.parentId.hasValue() ? event.parentId.get() : Snowflake::Invalid;
    if (isForumParent(parentId))
        return;

    Snowflake threadId = event.id.get();
    Snowflake guildId = event.guildId.hasValue() ? event.guildId.get() : Snowflake::Invalid;
    threadCache.erase(threadId);
    joinedThreads.erase(threadId);

    threadDeleted.publish(threadId, parentId, guildId);
}

void ClientInstance::onThreadListSync(const Discord::ThreadListSync &event)
{
    Snowflake guildId = event.guildId.get();

    std::vector<Discord::Channel> textThreads;
    std::unordered_set<Snowflake> textThreadIds;
    if (event.threads.hasValue()) {
        for (const auto &thread : event.threads.get()) {
            if (!thread.parentId.hasValue() || isForumParent(thread.parentId.get()))
                continue;
            ingestThread(thread, guildId);
            textThreadIds.insert(thread.id.get());
            textThreads.push_back(thread);
        }
    }

    if (event.members.hasValue())
        for (const auto &member : event.members.get())
            if (member.id.hasValue() && textThreadIds.contains(member.id.get()))
                joinedThreads.insert(member.id.get());

    std::vector<Snowflake> parentIds =
            event.channelIds.hasValue() ? event.channelIds.get() : std::vector<Snowflake>{};
    threadListSynced.publish(guildId, parentIds, textThreads);
}

void ClientInstance::onThreadMemberUpdate(const Discord::ThreadMemberUpdate &event)
{
    if (event.userId.hasValue() && event.userId.get() != account.id)
        return;
    if (!event.member.id.hasValue())
        return;
    Snowflake threadId = event.member.id.get();

    if (!threadCache.contains(threadId))
        return;

    joinedThreads.insert(threadId);
    threadMembershipChanged.publish(threadId);
}

void ClientInstance::onThreadMembersUpdate(const Discord::ThreadMembersUpdate &event)
{
    if (!event.id.hasValue())
        return;
    Snowflake threadId = event.id.get();
    if (!threadCache.contains(threadId))
        return;

    bool changed = false;
    if (event.addedMembers.hasValue()) {
        for (const auto &member : event.addedMembers.get()) {
            if (member.userId.hasValue() && member.userId.get() == account.id &&
                !joinedThreads.contains(threadId)) {
                joinedThreads.insert(threadId);
                changed = true;
            }
        }
    }
    if (event.removedMemberIds.hasValue()) {
        for (Snowflake userId : event.removedMemberIds.get())
            if (userId == account.id && joinedThreads.erase(threadId))
                changed = true;
    }

    if (changed)
        threadMembershipChanged.publish(threadId);
}

std::optional<Discord::Channel> ClientInstance::getChannel(Snowflake channelId)
{
    auto it = threadCache.find(channelId);
    if (it != threadCache.end())
        return it->second;
    return channelRepo.getChannel(channelId);
}

bool ClientInstance::isThreadJoined(Snowflake threadId) const
{
    return joinedThreads.contains(threadId);
}

void ClientInstance::fetchThreadList(Snowflake channelId, bool archived, int offset, ThreadListPageCallback callback)
{
    fetchThreadListAttempt(channelId, archived, offset, 0, std::move(callback));
}

void ClientInstance::searchMessages(Snowflake guildId, Snowflake channelId, const Discord::MessageSearchQuery &query,
                                    Discord::Client::MessageSearchCallback callback)
{
    client->searchMessages(guildId, channelId, query,
            [this, guard = Platform::Guard<ClientInstance>(this), guildId, callback = std::move(callback)](const Result<Discord::MessageSearchResult> &res) {
                if (!guard) return;
                if (res.success()) {
                    std::vector<Discord::User> authors;
                    authors.reserve(res.value->messages.size());
                    for (const auto &message : res.value->messages)
                        if (message.author.hasValue())
                            authors.push_back(message.author.get());
                    if (!authors.empty())
                        userManager->saveUsers(authors);

                    for (const auto &thread : res.value->threads)
                        ingestThread(thread, guildId);
                }
                callback(res);
            });
}

void ClientInstance::fetchThreadListAttempt(Snowflake channelId, bool archived, int offset, int attempt, const ThreadListPageCallback &callback)
{
    static constexpr int kMaxRetries = 5;

    client->searchThreads(
            channelId, archived, offset,
            [this, guard = Platform::Guard<ClientInstance>(this), channelId, archived, offset, attempt,
             callback](const Result<Discord::Client::ThreadListResult> &res) {
                if (!guard) return;
                if (!res.success()) {
                    callback(Result<ThreadListPage>::makeError(res.error));
                    return;
                }

                const auto &data = *res.value;
                if (data.indexNotReady) {
                    if (attempt >= kMaxRetries) {
                        log(LogLevel::Warning, LogCore)
                                << "Thread search index not ready after retries for" << channelId;
                        callback(Result<ThreadListPage>::makeError("Thread list is not ready yet"));
                        return;
                    }
                    int delayMs = std::max(1, data.retryAfterSeconds) * 1000;
                    Platform::Dispatcher::main().setTimer(delayMs, false,
                                       [guard = Platform::Guard<ClientInstance>(this), channelId, archived, offset, attempt, callback]() {
                                           if (guard) guard->fetchThreadListAttempt(channelId, archived, offset, attempt + 1, callback);
                                       });
                    return;
                }

                Snowflake guildId = Snowflake::Invalid;
                if (auto ch = getChannel(channelId); ch && ch->guildId.hasValue())
                    guildId = ch->guildId.get();
                for (const auto &thread : data.threads)
                    ingestThread(thread, guildId);

                for (const auto &member : data.members)
                    if (member.id.hasValue())
                        joinedThreads.insert(member.id.get());

                ThreadListPage page;
                page.threads = data.threads;
                page.hasMore = data.hasMore;
                callback(Result<ThreadListPage>::makeOk(page));
            });
}

bool ClientInstance::runInCacheTransaction(const char *what,
                                           const std::function<void(Platform::Database &)> &op)
{
    auto &db = Storage::DatabaseManager::instance().getDatabase(Storage::DatabaseManager::instance().getCacheConnectionName(account.id));

    Storage::Transaction txn(db);

    op(db);

    if (!txn.commit()) {
        log(LogLevel::Warning, LogCore) << "Failed to commit" << what;
        return false;
    }
    return true;
}

void ClientInstance::onGuildRoleCreated(const Discord::GuildRoleCreate &event)
{
    if (!event.guildId.hasValue() || !event.role.hasValue()) {
        log(LogLevel::Warning, LogCore) << "Invalid GUILD_ROLE_CREATE event: missing guildId or role";
        return;
    }

    Core::Snowflake guildId = event.guildId.get();
    const Discord::Role &role = event.role.get();

    if (!role.id.hasValue()) {
        log(LogLevel::Warning, LogCore) << "Invalid role in GUILD_ROLE_CREATE: missing role ID";
        return;
    }

    log(LogLevel::Debug, LogCore) << "Role created:" << role.id.get() << "in guild:" << guildId;

    if (!runInCacheTransaction("role creation",
                               [&](Platform::Database &db) { roleRepo.saveRole(guildId, role, db); }))
        return;

    rolesCacheByGuild.erase(guildId);
    permissionManager->invalidateUserGuildCache(account.id, guildId);
    memberListManager->handleRoleCreated(guildId, role);
    guildRoleCreated.publish(event);
}

void ClientInstance::onGuildRoleUpdated(const Discord::GuildRoleUpdate &event)
{
    if (!event.guildId.hasValue() || !event.role.hasValue()) {
        log(LogLevel::Warning, LogCore) << "Invalid GUILD_ROLE_UPDATE event: missing guildId or role";
        return;
    }

    Core::Snowflake guildId = event.guildId.get();
    const Discord::Role &role = event.role.get();

    if (!role.id.hasValue()) {
        log(LogLevel::Warning, LogCore) << "Invalid role in GUILD_ROLE_UPDATE: missing role ID";
        return;
    }

    log(LogLevel::Debug, LogCore) << "Role updated:" << role.id.get() << "in guild:" << guildId;

    if (!runInCacheTransaction("role update",
                               [&](Platform::Database &db) { roleRepo.saveRole(guildId, role, db); }))
        return;

    rolesCacheByGuild.erase(guildId);
    permissionManager->invalidateUserGuildCache(account.id, guildId);
    memberListManager->handleRoleUpdated(guildId, role);
    guildRoleUpdated.publish(event);
}

void ClientInstance::onGuildRoleDeleted(const Discord::GuildRoleDelete &event)
{
    if (!event.guildId.hasValue() || !event.roleId.hasValue()) {
        log(LogLevel::Warning, LogCore) << "Invalid GUILD_ROLE_DELETE event: missing guildId or roleId";
        return;
    }

    Core::Snowflake guildId = event.guildId.get();
    Core::Snowflake roleId = event.roleId.get();

    log(LogLevel::Debug, LogCore) << "Role deleted:" << roleId << "in guild:" << guildId;

    if (!runInCacheTransaction("role deletion",
                               [&](Platform::Database &db) { roleRepo.deleteRole(guildId, roleId, db); }))
        return;

    rolesCacheByGuild.erase(guildId);
    permissionManager->invalidateUserGuildCache(account.id, guildId);
    memberListManager->handleRoleDeleted(guildId, roleId);
    guildRoleDeleted.publish(event);
}

void ClientInstance::onGuildMembersChunk(const Discord::GuildMembersChunk &chunk)
{
    Snowflake guildId = chunk.guildId.get();
    std::vector<Snowflake> updatedUserIds;

    for (const auto &member : chunk.members.get()) {
        Snowflake userId =
                member.userId.hasValue()
                        ? member.userId.get()
                        : (member.user.hasValue() ? member.user->id.get() : Snowflake::Invalid);
        if (!userId.isValid())
            continue;

        memberRepo.saveMember(guildId, userId, member);
        if (member.user.hasValue())
            userManager->saveUser(member.user.get());

        pendingMemberRequests.erase(std::make_pair(guildId, userId));
        updatedUserIds.push_back(userId);
    }

    if (!updatedUserIds.empty())
        membersUpdated.publish(guildId, updatedUserIds);
}

void ClientInstance::onGuildMemberAdded(const Discord::GuildMemberAdd &event)
{
    const auto &member = event.member.get();
    if (!member.user.hasValue() || !member.user->id.get().isValid())
        return;

    Snowflake guildId = event.guildId.get();
    userManager->saveMemberWithUser(guildId, member);
    membersUpdated.publish(guildId, { member.user->id.get() });
}

void ClientInstance::onGuildMemberRemoved(const Discord::GuildMemberRemove &event)
{
    Snowflake guildId = event.guildId.get();
    Snowflake userId = event.user->id.get();
    if (!userId.isValid())
        return;

    userManager->removeMember(guildId, userId);
    memberRemoved.publish(guildId, userId);
}

std::vector<Discord::Ban> ClientInstance::observedBans(Snowflake guildId) const
{
    auto it = observedBansByGuild.find(guildId);
    return it == observedBansByGuild.end() ? std::vector<Discord::Ban>{} : it->second;
}

std::vector<Discord::Invite> ClientInstance::createdInvites(Snowflake guildId) const
{
    std::vector<Discord::Invite> unexpired;
    const Platform::DateTime now = Platform::DateTime::now();
    auto it = createdInvitesByGuild.find(guildId);
    if (it == createdInvitesByGuild.end()) return unexpired;
    for (const Discord::Invite &invite : it->second)
        if (!invite.hasExpired(now))
            unexpired.push_back(invite);
    return unexpired;
}

std::optional<RoleHierarchy> ClientInstance::selfRoleHierarchy(Snowflake guildId)
{
    const auto guild = getGuild(guildId);
    if (!guild)
        return std::nullopt;
    const auto selfRoles = userManager->getMemberRoles(guildId, account.id);
    return RoleHierarchy(guildId, guild->ownerId.get(), account.id, getRolesForGuild(guildId), selfRoles.value_or(std::vector<Snowflake>()));
}

void ClientInstance::onGuildBanAdded(const Discord::GuildBanEvent &event)
{
    const Snowflake userId = event.user->id.get();
    if (!userId.isValid())
        return;
    std::vector<Discord::Ban> &bans = observedBansByGuild[event.guildId.get()];
    removeBanOf(bans, userId);
    Discord::Ban ban;
    ban.user = event.user.get();
    ban.reason = nullptr;
    bans.push_back(ban);
}

void ClientInstance::onGuildBanRemoved(const Discord::GuildBanEvent &event)
{
    const auto bans = observedBansByGuild.find(event.guildId.get());
    if (bans != observedBansByGuild.end())
        removeBanOf(bans->second, event.user->id.get());
}

void ClientInstance::onInviteCreated(const Discord::Invite &invite)
{
    Snowflake guildId = invite.guildId.hasValue() ? invite.guildId.get() : Snowflake();
    if (!guildId.isValid())
        if (const auto channel = getChannel(invite.resolvedChannelId()); channel && channel->guildId.hasValue())
            guildId = channel->guildId.get();
    if (!guildId.isValid())
        return;

    std::vector<Discord::Invite> &invites = createdInvitesByGuild[guildId];
    removeInvite(invites, invite.code.get());
    invites.push_back(invite);
}

void ClientInstance::onInviteRevoked(const std::string &code)
{
    for (auto &[guildId, invites] : createdInvitesByGuild)
        removeInvite(invites, code);
}

void ClientInstance::onGuildMemberUpdate(const Discord::GuildMemberUpdate &event)
{
    Snowflake guildId = event.guildId.get();
    const auto &member = event.member.get();

    Snowflake userId = member.user.hasValue() ? member.user->id.get() : Snowflake::Invalid;
    if (!userId.isValid())
        return;

    if (member.user.hasValue())
        userManager->saveUser(member.user.get());
    userManager->saveMember(guildId, userId, member);

    if (userId == account.id)
        permissionManager->invalidateUserGuildCache(userId, guildId);

    membersUpdated.publish(guildId, { userId });
}

void ClientInstance::onGuildMemberListUpdate(const Discord::GuildMemberListUpdate &update)
{
    if (!update.guildId.hasValue() || !update.ops.hasValue())
        return;

    const Snowflake guildId = update.guildId.get();
    std::vector<Discord::User> users;
    std::vector<Discord::Member> members;

    auto persist = [&](const Discord::Member &member) {
        if (!member.user.hasValue() || !member.user->id.hasValue())
            return;
        users.push_back(member.user.get());
        members.push_back(member);
        members.back().presence.undefine();
    };

    for (const auto &op : update.ops.get()) {
        if (op.items.hasValue()) {
            for (const auto &item : op.items.get()) {
                if (item.member.hasValue())
                    persist(item.member.get());
            }
        }
        if (op.item.hasValue() && op.item.get().member.hasValue())
            persist(op.item.get().member.get());
    }

    if (!users.empty())
        userManager->saveUsers(users);
    if (!members.empty())
        userManager->saveMembers(guildId, members);
}

static Markdown::ChannelLinkPart channelLinkPart(const Discord::Channel &channel)
{
    using Discord::ChannelType;

    std::string icon = "hash";
    if (channel.isThread()) {
        icon = "thread";
    } else if (channel.type.hasValue()) {
        switch (channel.type.get()) {
        case ChannelType::GUILD_FORUM:
        case ChannelType::GUILD_MEDIA:
            icon = "forum";
            break;
        case ChannelType::GUILD_VOICE:
        case ChannelType::GUILD_STAGE_VOICE:
            icon = "voice";
            break;
        case ChannelType::GUILD_NEWS:
            icon = "announcement";
            break;
        default:
            break;
        }
    }

    std::string name = channel.name.hasValue() ? channel.name.get() : channel.id.get().toString();
    return { icon, name };
}

std::vector<Markdown::ChannelLinkPart> ClientInstance::channelLinkParts(const Markdown::ChannelLinkRef &ref)
{
    using Part = Markdown::ChannelLinkPart;

    auto source = channelRepo.getChannel(ref.sourceChannelId);
    Snowflake sourceGuildId = source && source->guildId.hasValue() ? source->guildId.get() : Snowflake::Invalid;
    bool hasMessage = ref.messageId.isValid();

    auto channel = channelRepo.getChannel(ref.channelId);
    if (!channel) {
        std::vector<Part> parts;
        if (ref.guildId.isValid()) {
            auto guild = guildRepo.getGuild(ref.guildId);
            if (!guild)
                return {};
            if (ref.guildId != sourceGuildId)
                parts.push_back({ {}, guild->name.get() });
        }
        parts.push_back({ "hash", "unknown", true });
        return parts;
    }

    if (!channel->guildId.hasValue())
        return {};
    auto guild = guildRepo.getGuild(channel->guildId.get());
    if (!guild)
        return {};

    if (!permissionManager->hasChannelPermission(accountId(), ref.channelId, Discord::Permission::VIEW_CHANNEL))
        return { { "locked", "No Access" } };

    bool isForumPost = channel->isThread() && channel->parentId.hasValue() && isForumParent(channel->parentId.get());

    Part channelPart = channelLinkPart(*channel);
    if (isForumPost)
        channelPart.icon = "post";
    Part messagePart{ isForumPost ? "post" : "message", {} };
    Part guildPart{ {}, guild->name.get() };

    bool sameGuild = channel->guildId.get() == sourceGuildId;
    if (sameGuild && hasMessage) {
        auto forum = isForumPost ? channelRepo.getChannel(channel->parentId.get()) : std::nullopt;
        if (forum)
            return { channelLinkPart(*forum), channelPart };
        return { channelPart, messagePart };
    }
    if (sameGuild)
        return { channelPart };
    if (hasMessage)
        return { guildPart, isForumPost ? channelPart : messagePart };
    return { guildPart, channelPart };
}

void ClientInstance::onMessagesReceived(const MessageRequestResult &result)
{
    if (!result.success || result.messages.empty())
        return;

    if (result.type == Discord::Client::MessageLoadType::Created)
        return;

    auto channelOpt = channelRepo.getChannel(result.channelId);
    if (!channelOpt || !channelOpt->guildId.hasValue())
        return;

    Snowflake guildId = channelOpt->guildId.get();
    std::vector<Snowflake> missingUserIds;

    for (const auto &msg : result.messages) {
        if (!msg.author.hasValue())
            continue;

        Snowflake userId = msg.author->id.get();

        if (pendingMemberRequests.contains(std::make_pair(guildId, userId)))
            continue;

        if (userManager->getMember(guildId, userId))
            continue;

        pendingMemberRequests.insert(std::make_pair(guildId, userId));
        missingUserIds.push_back(userId);
    }

    if (!missingUserIds.empty())
        client->requestGuildMembers(guildId, missingUserIds);
}

void ClientInstance::onMessageCreated(const Discord::Message &msg)
{
    if (msg.isEphemeral() || !msg.channelId.hasValue() || !msg.id.hasValue())
        return;

    Snowflake channelId = msg.channelId.get();
    Snowflake messageId = msg.id.get();

    // we can get a message in a thread without knowing the thread exists maybe
    if (msg.guildId.hasValue())
        readStateManager->registerChannelGuild(channelId, msg.guildId.get());

    bool fromSelf = msg.author.hasValue() && msg.author->id.get() == account.id;
    bool isMention = !fromSelf && isMessageMentioningMe(msg);
    readStateManager->handleMessageCreated(channelId, messageId, fromSelf, isMention);

    channelLastMessageUpdated.publish(channelId, messageId);
    if (const auto facts = notificationFactsFor(msg); shouldNotify(facts))
        notificationRequested.publish(msg, isMentioned(facts));
}

bool ClientInstance::isMessageMentioningMe(const Discord::Message &msg) const
{
    if (msg.author.hasValue() && relationshipManager->isBlockedOrIgnored(msg.author->id.get()))
        return false;

    if (!msg.guildId.hasValue()) {
        if (msg.type.hasValue() && msg.type.get() == Discord::MessageType::RECIPIENT_REMOVE)
            return false;
        return !readStateManager->isChannelMuted(msg.channelId.get());
    }

    Snowflake guildId = msg.guildId.get();

    if (msg.mentionEveryone.hasValue() && msg.mentionEveryone.get() &&
        !readStateManager->isSuppressEveryone(guildId))
        return true;

    if (msg.mentions.hasValue()) {
        for (const auto &user : msg.mentions.get())
            if (user.id.get() == account.id)
                return true;
    }

    if (!msg.mentionRoles.hasValue() || readStateManager->isSuppressRoles(guildId))
        return false;

    auto myRoles = userManager->getMemberRoles(guildId, account.id);
    if (!myRoles)
        return false;
    for (const auto &roleId : msg.mentionRoles.get())
        if (std::find(myRoles->begin(), myRoles->end(), roleId) != myRoles->end())
            return true;

    return false;
}

NotificationFacts ClientInstance::notificationFactsFor(const Discord::Message &msg)
{
    NotificationFacts facts;
    const Snowflake channelId = msg.channelId.get();
    const Snowflake authorId = msg.author.hasValue() ? msg.author->id.get() : Snowflake::Invalid;
    facts.fromSelf = authorId == account.id;
    facts.authorBlocked = authorId.isValid() && relationshipManager->isBlockedOrIgnored(authorId);
    facts.viewingChannel = readStateManager->activeChannel() == channelId && Platform::Desktop::applicationActive();
    facts.doNotDisturb = presenceManager->selfStatus() == Discord::StatusType::DND;

    if (msg.mentions.hasValue())
        for (const auto &user : msg.mentions.get())
            if (user.id.get() == account.id)
                facts.mentionsMe = true;
    facts.mentionsEveryone = msg.mentionEveryone.valueOr(false);

    auto channel = getChannel(channelId);
    // Message requests stay quiet until they're accepted, as in the official client.
    const bool request = channel && channel->isMessageRequest.valueOr(false);
    if (!msg.guildId.hasValue()) {
        facts.privateChannel = true;
        facts.muted = request || readStateManager->isChannelMuted(channelId);
        return facts;
    }

    const Snowflake guildId = msg.guildId.get();
    Snowflake parentId = channel ? channel->parentId.valueOr(Snowflake::Invalid) : Snowflake::Invalid;
    Snowflake categoryId;
    // A thread inherits from its parent channel and that channel's category.
    if (channel && channel->isThread() && parentId.isValid())
        if (auto parent = getChannel(parentId))
            categoryId = parent->parentId.valueOr(Snowflake::Invalid);
    facts.muted = request || readStateManager->isMutedThroughParents(channelId, parentId, categoryId, guildId);
    facts.level = readStateManager->resolveMessageNotifications(guildId, channelId, parentId);
    facts.suppressEveryone = readStateManager->isSuppressEveryone(guildId);
    facts.suppressRoles = readStateManager->isSuppressRoles(guildId);

    if (msg.mentionRoles.hasValue())
        if (auto myRoles = userManager->getMemberRoles(guildId, account.id))
            for (const auto &roleId : msg.mentionRoles.get())
                if (std::find(myRoles->begin(), myRoles->end(), roleId) != myRoles->end())
                    facts.mentionsMyRole = true;
    return facts;
}

void ClientInstance::handleAckRequest(Snowflake channelId, Snowflake messageId)
{
    auto channelOpt = channelRepo.getChannel(channelId);
    bool isGuildChannel = channelOpt && channelOpt->guildId.hasValue();
    if (!isGuildChannel)
        isGuildChannel = client->getGuildIdForChannel(channelId).isValid();
    int ackFlags = 0;
    if (channelOpt && channelOpt->isThread())
        ackFlags = static_cast<int>(Discord::ReadStateFlag::IS_THREAD);
    else if (isGuildChannel)
        ackFlags = static_cast<int>(Discord::ReadStateFlag::IS_GUILD_CHANNEL);
    int lastViewed = ReadStateManager::daysSinceDiscordEpoch();
    client->ackMessage(channelId, messageId, ackFlags, lastViewed);
}

void ClientInstance::handleBulkAckRequest(const std::vector<std::pair<Snowflake, Snowflake>> &pairs)
{
    for (const auto &[channelId, messageId] : pairs)
        queuedBulkAcks.push_back({ channelId, messageId, 0 });

    if (!bulkAckInFlight)
        sendNextBulkAckBatch();
}

void ClientInstance::sendNextBulkAckBatch()
{
    constexpr int maxPerRequest = 100;
    constexpr int delayBetweenRequestsMs = 1000;

    bulkAckInFlight = !queuedBulkAcks.empty();
    if (!bulkAckInFlight)
        return;

    const auto batchEnd = queuedBulkAcks.begin() + std::min<std::size_t>(queuedBulkAcks.size(), maxPerRequest);
    const std::vector<Discord::Client::AckEntry> batch(queuedBulkAcks.begin(), batchEnd);
    queuedBulkAcks.erase(queuedBulkAcks.begin(), queuedBulkAcks.begin() + batch.size());

    client->ackBulk(batch, [this, guard = Platform::Guard<ClientInstance>(this)](bool success) {
        if (!guard) return;
        if (!success) {
            queuedBulkAcks.clear();
            bulkAckInFlight = false;
            return;
        }
        Platform::Dispatcher::main().setTimer(delayBetweenRequestsMs, false, [guard = Platform::Guard<ClientInstance>(this)]() { if (guard) guard->sendNextBulkAckBatch(); });
    });
}

std::vector<Snowflake> ClientInstance::markableChannelIds(Snowflake guildId, Snowflake categoryId)
{
    std::vector<Snowflake> ids;
    std::unordered_set<Snowflake> viewableParents;

    for (const auto &channel : channelRepo.getChannelsForGuild(guildId)) {
        if (!channel.type.hasValue() || channel.isThread() ||
            channel.type.get() == Discord::ChannelType::GUILD_CATEGORY)
            continue;
        if (categoryId.isValid() && (!channel.parentId.hasValue() || channel.parentId.get() != categoryId))
            continue;

        Snowflake channelId = channel.id.get();
        if (!permissionManager->hasChannelPermission(account.id, channelId, Discord::Permission::VIEW_CHANNEL))
            continue;
        if (channel.isVoice() &&
            !permissionManager->hasChannelPermission(account.id, channelId, Discord::Permission::CONNECT))
            continue;

        ids.push_back(channelId);
        viewableParents.insert(channelId);

        if (isForumParent(channelId))
            for (const auto &post : forumManager->joinedPosts(channelId))
                ids.push_back(post.id.get());
    }

    for (auto thread = threadCache.begin(); thread != threadCache.end(); ++thread) {
        if (!thread->second.parentId.hasValue() || !viewableParents.contains(thread->second.parentId.get()))
            continue;

        bool activeJoined = joinedThreads.contains(thread->first) && !thread->second.isArchived();
        if (activeJoined || readStateManager->getMentionCount(thread->first) > 0)
            ids.push_back(thread->first);
    }

    return ids;
}

void ClientInstance::markGuildsAsRead(const std::vector<Snowflake> &guildIds)
{
    std::vector<Snowflake> channelIds;
    for (Snowflake guildId : guildIds)
        { auto ids = markableChannelIds(guildId, Snowflake::Invalid);
        channelIds.insert(channelIds.end(), ids.begin(), ids.end()); }

    readStateManager->markChannelsAsRead(channelIds);
}

void ClientInstance::markCategoryAsRead(Snowflake guildId, Snowflake categoryId)
{
    readStateManager->markChannelsAsRead(markableChannelIds(guildId, categoryId));
}

ClientInstance::~ClientInstance()
{
#ifdef STYX_HAVE_SCREENSHARE
    streamManager.reset();
#endif
#ifndef STYX_NO_VOICE
    voiceManager.reset();
#endif
    messageManager.reset();
    forumManager.reset();
    favoriteGifManager.reset();
    emojiManager.reset();
    memberListManager.reset();
    readStateManager.reset();
    presenceManager.reset();
    userManager.reset();
    relationshipManager.reset();
    permissionManager.reset();
    client.reset();
    Storage::DatabaseManager::instance().closeCacheDatabase(account.id);
}

void ClientInstance::start()
{
    discord()->start();
}

void ClientInstance::stop()
{
    emojiManager->savePendingUsage();

    if (isInVoice()) {
        discord()->sendVoiceStateUpdate(currentVoiceGuildId, Snowflake::Invalid, false, false);
#ifndef STYX_NO_VOICE
        voiceManager->disconnect();
#endif
    }
    discord()->stop();
}

Discord::Client *ClientInstance::discord() const
{
    return client.get();
}

MessageManager *ClientInstance::messages() const
{
    return messageManager.get();
}

ForumManager *ClientInstance::forums() const
{
    return forumManager.get();
}

UserManager *ClientInstance::users() const
{
    return userManager.get();
}

PermissionManager *ClientInstance::permissions() const
{
    return permissionManager.get();
}

ReadStateManager *ClientInstance::readState() const
{
    return readStateManager.get();
}

RelationshipManager *ClientInstance::relationships() const
{
    return relationshipManager.get();
}

PresenceManager *ClientInstance::presences() const
{
    return presenceManager.get();
}

MemberListManager *ClientInstance::memberList() const
{
    return memberListManager.get();
}

EmojiManager *ClientInstance::emojis() const
{
    return emojiManager.get();
}

FavoriteGifs *ClientInstance::favoriteGifs() const
{
    return favoriteGifManager.get();
}

CallStore *ClientInstance::calls() const
{
    return callStore.get();
}

CommandIndexCache *ClientInstance::commandIndexes() const
{
    return commandIndexCache.get();
}

#ifndef STYX_NO_VOICE
Audio::VoiceManager *ClientInstance::voice() const
{
    return voiceManager.get();
}

#ifdef STYX_HAVE_SCREENSHARE
Stream::StreamManager *ClientInstance::streams() const
{
    return streamManager.get();
}
#endif
#endif

std::vector<Discord::Role> ClientInstance::getRolesForGuild(Snowflake guildId)
{
    auto it = rolesCacheByGuild.find(guildId);
    if (it != rolesCacheByGuild.end())
        return it->second;

    auto roles = roleRepo.getRolesForGuild(guildId);
    rolesCacheByGuild.insert_or_assign(guildId, roles);
    return roles;
}

MentionNames ClientInstance::mentionNames(Snowflake guildId)
{
    using JsonUtils::detail::snowflake;
    MentionNames names;
    names.user = [users = userManager.get(), guildId](std::string_view userId) -> std::string {
        const Snowflake id = snowflake(userId);
        return id.isValid() ? users->getDisplayName(id, guildId) : std::string();
    };
    names.channel = [this](std::string_view channelId) -> std::string {
        auto channel = getChannel(snowflake(channelId));
        return channel ? channel->name.valueOr(std::string()) : std::string();
    };
    names.role = [this, guildId](std::string_view roleId) -> std::string {
        const Snowflake id = snowflake(roleId);
        for (const auto &role : getRolesForGuild(guildId))
            if (role.id.hasValue() && role.id.get() == id)
                return role.name.valueOr(std::string());
        return std::string();
    };
    return names;
}

std::vector<Discord::Role> ClientInstance::getMemberRolesSorted(Snowflake guildId, Snowflake userId)
{
    auto memberRoleIds = userManager->getMemberRoles(guildId, userId);
    if (!memberRoleIds || memberRoleIds->empty())
        return {};

    std::unordered_set<Snowflake> memberRoleSet(memberRoleIds->begin(), memberRoleIds->end());
    std::vector<Discord::Role> result;
    result.reserve(memberRoleIds->size());
    for (const auto &role : getRolesForGuild(guildId)) {
        if (memberRoleSet.contains(role.id.get()))
            result.push_back(role);
    }

    std::sort(result.begin(), result.end(),
              [](const Discord::Role &a, const Discord::Role &b) {
                  return a.position.get() > b.position.get();
              });
    return result;
}

std::optional<Discord::Guild> ClientInstance::getGuild(Snowflake guildId)
{
    return guildRepo.getGuild(guildId);
}

std::vector<Discord::Channel> ClientInstance::getGuildChannels(Snowflake guildId)
{
    return channelRepo.getChannelsForGuild(guildId);
}

std::optional<Snowflake> ClientInstance::findDmChannelWithUser(Snowflake userId)
{
    return channelRepo.findDmChannelWithUser(userId);
}

void ClientInstance::openDmChannel(Snowflake userId, DmChannelCallback callback)
{
    if (auto existing = findDmChannelWithUser(userId)) {
        callback(Result<Snowflake>::makeOk(*existing));
        return;
    }

    client->openDmChannel(userId, [this, guard = Platform::Guard<ClientInstance>(this), callback](const Result<Discord::ChannelCreate> &res) {
        if (!guard) return;
        if (!res.success()) {
            callback(Result<Snowflake>::makeError(res.error));
            return;
        }

        // idempotent so its ok
        onChannelCreated(*res.value);
        callback(Result<Snowflake>::makeOk(res.value->channel.get().id.get()));
    });
}

int ClientInstance::getChannelRateLimit(Snowflake channelId)
{
    auto channelOpt = getChannel(channelId);
    if (!channelOpt || !channelOpt->rateLimitPerUser.hasValue())
        return 0;
    return channelOpt->rateLimitPerUser.get();
}

ConnectionState ClientInstance::state() const
{
    return client->getState();
}

Snowflake ClientInstance::accountId() const
{
    return account.id;
}

const AccountInfo &ClientInstance::accountInfo() const
{
    return account;
}

Snowflake ClientInstance::voiceChannelId() const
{
    return currentVoiceChannelId;
}

Snowflake ClientInstance::voiceGuildId() const
{
    return currentVoiceGuildId;
}

bool ClientInstance::isInVoice() const
{
    return currentVoiceChannelId.isValid();
}

} // namespace Core
} // namespace Styx
