#include "ReadStateManager.hpp"

#include <algorithm>
#include <chrono>

#include "Platform/Desktop.hpp"
#include "Platform/Dispatcher.hpp"

#include <optional>

#include "Core/Logging.hpp"
#include "Core/PermissionManager.hpp"
#include "Discord/Enums.hpp"

namespace Styx {
namespace Core {

namespace {

constexpr int kDeferredAckDelayMs = 3000;
constexpr std::int64_t kMsPerDay = 24 * 60 * 60 * 1000;

bool isOlderThanDays(Snowflake id, int days)
{
    if (!id.isValid())
        return true;
    return id.toDateTime().unixMillis() < Platform::DateTime::now().unixMillis() - days * kMsPerDay;
}

template <typename Settings, typename Flag>
bool hasFlag(const Settings &settings, Flag flag)
{
    return settings.flags.hasValue() && (settings.flags.get() & static_cast<int>(flag)) != 0;
}

bool usesNewNotifications(int notificationSettingsFlags)
{
    return (notificationSettingsFlags & static_cast<int>(Discord::NotificationSettingsFlag::USE_NEW_NOTIFICATIONS)) != 0;
}

template <typename Settings>
bool isMutedSetting(const Settings &settings)
{
    bool muted = settings.muted.hasValue() && settings.muted.get();
    const Discord::MuteConfig *config = settings.muteConfig.hasValue() ? &settings.muteConfig.get() : nullptr;
    return ReadStateManager::isMuteActive(muted, config);
}

} // namespace

ReadStateManager::ReadStateManager(Snowflake accountId, PermissionManager *perms)
    : accountId(accountId), permissionManager(perms)
{
    track(Platform::Desktop::activationChanged().connect([this](bool active) {
        if (active)
            tryAckActiveChannel();
    }));
}

void ReadStateManager::loadFromReady(const std::vector<Discord::ReadStateEntry> &readStates,
                                     const std::vector<Discord::UserGuildSettings> &guildSettings,
                                     int notificationSettingsFlags)
{
    channelReadStates.clear();
    guildSettingsMap.clear();
    guildInfo.clear();
    channelGuildMap.clear();
    resourceChannels.clear();
    voiceChannels.clear();
    forumChannels.clear();
    messageRequestChannels.clear();
    ackIdAtSelect.clear();
    outgoingAcks.clear();

    for (const auto &entry : readStates) {
        int rsType = entry.readStateType.hasValue() ? entry.readStateType.get() : 0;
        if (rsType != 0)
            continue;

        channelReadStates.insert_or_assign(entry.id.get(), entry);
    }

    channelOverrideCache.clear();
    guildOverrideChannels.clear();

    for (const auto &settings : guildSettings) {
        Snowflake key = settings.guildId.isNull() ? Snowflake(0) : settings.guildId.get();
        guildSettingsMap.insert_or_assign(key, settings);
        rebuildChannelOverrideCache(key);
    }

    useNewNotifications = usesNewNotifications(notificationSettingsFlags);

    Core::log(Core::LogLevel::Debug, LogCore) << "ReadStateManager loaded" << channelReadStates.size() << "read states and"
                     << guildSettingsMap.size() << "guild settings";
}

void ReadStateManager::setNotificationSettingsFlags(int flags)
{
    bool enabled = usesNewNotifications(flags);
    if (useNewNotifications == enabled)
        return;

    useNewNotifications = enabled;
    for (auto it = guildInfo.begin(); it != guildInfo.end(); ++it)
        guildSettingsUpdated.publish(it->first);
}

void ReadStateManager::onNotificationSettingsUpdate(const Discord::NotificationSettings &settings)
{
    if (settings.flags.hasValue())
        setNotificationSettingsFlags(settings.flags.get());
}

ChannelReadState ReadStateManager::computeChannelReadState(Snowflake channelId, Snowflake guildId,
                                                           Snowflake parentId) const
{
    ChannelReadState result;
    result.isMuted = isChannelMuted(channelId);
    if (!canTrackUnreads(channelId))
        return result;

    Snowflake lastMessageId = getChannelLastMessageId(channelId);
    bool canReadHistory = permissionManager->hasChannelPermission(accountId, channelId, Discord::Permission::READ_MESSAGE_HISTORY);
    result.mentionCount = canReadHistory ? getMentionCount(channelId) : 0;
    result.isUnread = isChannelUnread(channelId, lastMessageId, guildId);

    bool isVoice = voiceChannels.contains(channelId);
    if (isVoice && !permissionManager->hasChannelPermission(accountId, channelId, Discord::Permission::CONNECT))
        result.mentionCount = 0;

    if (isOptInGuild(guildId)) {
        bool optedIn = isChannelOptedIn(channelId) || (parentId.isValid() && isChannelOptedIn(parentId));
        if (isOlderThanDays(lastMessageId, 7)) {
            result.mentionCount = 0;
            result.isUnread = false;
        } else if (result.mentionCount == 0 && !optedIn && !hasRecentlyVisitedAndRead(channelId, lastMessageId)) {
            result.isUnread = false;
        }
    }

    bool countsWhenUnread = result.mentionCount > 0 || (!isVoice && unreadCountsForGuild(guildId, channelId, parentId));
    result.countsForGuildUnread = result.isUnread &&
                                  countsWhenUnread &&
                                  !isMutedThroughParents(channelId, parentId, Snowflake::Invalid, guildId);

    return result;
}

ChannelReadState ReadStateManager::computeDMReadState(Snowflake channelId) const
{
    ChannelReadState result;
    result.isMuted = isChannelMuted(channelId);
    if (messageRequestChannels.contains(channelId))
        return result;

    result.mentionCount = getMentionCount(channelId);
    result.isUnread = isChannelUnread(channelId, getChannelLastMessageId(channelId), Snowflake::Invalid);
    result.countsForGuildUnread = result.isUnread && !result.isMuted;
    return result;
}

ChannelReadState ReadStateManager::computeThreadReadState(Snowflake threadId, Snowflake guildId,
                                                          Snowflake parentId, Snowflake categoryId,
                                                          bool joined) const
{
    ChannelReadState result;
    result.isMuted = isChannelMuted(threadId) ||
                     (parentId.isValid() && isChannelMuted(parentId)) ||
                     (categoryId.isValid() && isChannelMuted(categoryId));
    if (!canTrackUnreads(threadId))
        return result;

    result.mentionCount = getMentionCount(threadId);
    result.isUnread = joined ? isChannelUnread(threadId, getChannelLastMessageId(threadId), guildId)
                             : result.mentionCount > 0;
    result.countsForGuildUnread = result.isUnread &&
                                  !isMutedThroughParents(threadId, parentId, categoryId, guildId);
    return result;
}

ChannelReadState ReadStateManager::computeForumPostReadState(Snowflake postId, Snowflake guildId,
                                                             Snowflake forumId, Snowflake categoryId) const
{
    ChannelReadState result;
    result.isMuted = isChannelMuted(postId);
    if (!canTrackUnreads(postId))
        return result;

    result.mentionCount = getMentionCount(postId);
    result.isUnread = isForumPostUnread(postId, getChannelLastMessageId(postId), false);
    result.countsForGuildUnread = result.isUnread &&
                                  !isMutedThroughParents(postId, forumId, categoryId, guildId);
    return result;
}

// Server Guide resource channels never track unreads or mentions, whatever their read state says.
bool ReadStateManager::canTrackUnreads(Snowflake channelId) const
{
    if (resourceChannels.contains(channelId))
        return false;
    return permissionManager->hasChannelPermission(accountId, channelId, Discord::Permission::VIEW_CHANNEL);
}

bool ReadStateManager::isOptInGuild(Snowflake guildId) const
{
    auto gi = guildInfo.find(guildId);
    if (gi == guildInfo.end() || !gi->second.isCommunity)
        return false;
    auto gs = guildSettingsMap.find(guildId);
    return gs != guildSettingsMap.end() && hasFlag(gs->second, Discord::UserGuildSettingsFlag::OPT_IN_CHANNELS_ON);
}

bool ReadStateManager::isChannelOptedIn(Snowflake channelId) const
{
    auto it = channelOverrideCache.find(channelId);
    return it != channelOverrideCache.end() && hasFlag(it->second, Discord::ChannelOverrideFlag::OPT_IN_ENABLED);
}

bool ReadStateManager::hasRecentlyVisitedAndRead(Snowflake channelId, Snowflake lastMessageId) const
{
    if (!lastMessageId.isValid())
        return false;
    auto it = channelReadStates.find(channelId);
    return it != channelReadStates.end() &&
           it->second.lastMessageId.hasValue() &&
           !isOlderThanDays(it->second.lastMessageId.get(), 3);
}

bool ReadStateManager::isMutedThroughParents(Snowflake channelId, Snowflake parentId, Snowflake categoryId,
                                             Snowflake guildId) const
{
    return isChannelMuted(channelId) ||
           (parentId.isValid() && isChannelMuted(parentId)) ||
           (categoryId.isValid() && isChannelMuted(categoryId)) ||
           (guildId.isValid() && isGuildMuted(guildId));
}

bool ReadStateManager::isChannelUnread(Snowflake channelId, Snowflake channelLastMessageId, Snowflake guildId) const
{
    if (!channelLastMessageId.isValid())
        return false;

    return channelLastMessageId > effectiveAckId(channelId, guildId);
}

bool ReadStateManager::hasUnreadOrMentions(Snowflake channelId) const
{
    return getMentionCount(channelId) > 0 ||
           isChannelUnread(channelId, getChannelLastMessageId(channelId), guildForChannel(channelId));
}

bool ReadStateManager::hasBeenRead(Snowflake channelId) const
{
    auto it = channelReadStates.find(channelId);
    return it != channelReadStates.end() && it->second.lastMessageId.hasValue();
}

bool ReadStateManager::isForumPostUnread(Snowflake threadId, Snowflake lastMessageId,
                                         bool archived) const
{
    if (archived || !lastMessageId.isValid())
        return false;

    auto it = channelReadStates.find(threadId);
    if (it != channelReadStates.end() && it->second.lastMessageId.hasValue())
        return lastMessageId > it->second.lastMessageId.get();

    return true;
}

bool ReadStateManager::isThreadRelevant(const Discord::Channel &thread) const
{
    const Snowflake threadId = thread.id.get();
    if (thread.isPinned())
        return true;
    if (getMentionCount(threadId) > 0)
        return true;
    if (isForumPostUnread(threadId, thread.effectiveLastMessageId(), thread.isArchived()) &&
        !isChannelMuted(threadId))
        return true;

    if (!thread.threadMetadata.hasValue() || !thread.threadMetadata->autoArchiveDuration.hasValue())
        return false;
    const auto &meta = thread.threadMetadata.get();
    std::int64_t base = thread.effectiveLastMessageId().toDateTime().unixMillis();
    if (meta.archiveTimestamp.hasValue())
        base = std::max(base, meta.archiveTimestamp.get().unixMillis());
    const std::int64_t expiry = base + static_cast<std::int64_t>(meta.autoArchiveDuration.get()) * 60000;
    return expiry > Platform::DateTime::now().unixMillis();
}

bool ReadStateManager::isForumPostNew(Snowflake threadId, Snowflake forumId, Snowflake guildId,
                                      bool archived) const
{
    if (archived || !threadId.isValid() || !forumId.isValid())
        return false;

    if (hasBeenRead(threadId))
        return false;

    auto snap = ackIdAtSelect.find(forumId);
    if (snap == ackIdAtSelect.end() || threadId <= snap->second)
        return false;

    if (!guildId.isValid())
        return true;

    auto gi = guildInfo.find(guildId);
    std::int64_t joinedAtMs = gi != guildInfo.end() && gi->second.joinedAtMs > 0
                                ? gi->second.joinedAtMs
                                : Platform::DateTime::now().unixMillis();
    return threadId.toDateTime().unixMillis() > joinedAtMs;
}

void ReadStateManager::markForumPostAsRead(Snowflake threadId, Snowflake lastMessageId)
{
    if (!lastMessageId.isValid())
        return;

    auto it = channelReadStates.find(threadId);
    if (it != channelReadStates.end() && it->second.lastMessageId.hasValue() && lastMessageId <= it->second.lastMessageId.get())
        return;

    ack(threadId, lastMessageId, true);
}

Snowflake ReadStateManager::effectiveAckId(Snowflake channelId, Snowflake guildId) const
{
    auto it = channelReadStates.find(channelId);
    if (it != channelReadStates.end() && it->second.lastMessageId.hasValue())
        return it->second.lastMessageId.get();

    if (guildId.isValid()) {
        auto gi = guildInfo.find(guildId);
        if (gi != guildInfo.end() && gi->second.joinedAtMs > 0)
            return Snowflake::fromUnixMs(gi->second.joinedAtMs);
        return Snowflake::fromUnixMs(Platform::DateTime::now().unixMillis());
    }

    if (channelId.isValid())
        return Snowflake::fromUnixMs(channelId.toDateTime().unixMillis());

    return Snowflake::fromUnixMs(Platform::DateTime::now().unixMillis());
}

Discord::MessageNotificationLevel ReadStateManager::resolveMessageNotifications(Snowflake guildId,
                                                                                Snowflake channelId,
                                                                                Snowflake parentId) const
{
    using Level = Discord::MessageNotificationLevel;

    auto overrideNotif = [this](Snowflake id) -> std::optional<Level> {
        auto it = channelOverrideCache.find(id);
        if (it != channelOverrideCache.end() && it->second.messageNotifications.hasValue() &&
            it->second.messageNotifications.get() != Level::INHERIT)
            return it->second.messageNotifications.get();
        return std::nullopt;
    };

    if (auto n = overrideNotif(channelId))
        return *n;
    if (parentId.isValid())
        if (auto n = overrideNotif(parentId))
            return *n;

    auto gs = guildSettingsMap.find(guildId);
    if (gs != guildSettingsMap.end() && gs->second.messageNotifications.hasValue() &&
        gs->second.messageNotifications.get() != Level::INHERIT)
        return gs->second.messageNotifications.get();

    auto gi = guildInfo.find(guildId);
    if (gi != guildInfo.end())
        return gi->second.defaultMessageNotifications;

    return Level::ALL_MESSAGES;
}

// Discord's resolveUnreadSetting. Accounts without the newer notification settings light the
// server icon for any unread channel; with them, explicit unread flags on the channel, its
// category and then the guild decide, falling back to the message notification level.
bool ReadStateManager::unreadCountsForGuild(Snowflake guildId, Snowflake channelId, Snowflake parentId) const
{
    if (!useNewNotifications)
        return true;

    using Discord::ChannelOverrideFlag;
    using Discord::UserGuildSettingsFlag;

    auto channelSetting = [this](Snowflake id) -> std::optional<bool> {
        auto it = channelOverrideCache.find(id);
        if (it == channelOverrideCache.end())
            return std::nullopt;
        if (hasFlag(it->second, ChannelOverrideFlag::UNREADS_ALL_MESSAGES))
            return true;
        if (hasFlag(it->second, ChannelOverrideFlag::UNREADS_ONLY_MENTIONS))
            return false;
        return std::nullopt;
    };

    if (auto s = channelSetting(channelId))
        return *s;
    if (parentId.isValid())
        if (auto s = channelSetting(parentId))
            return *s;

    auto gs = guildSettingsMap.find(guildId);
    if (gs != guildSettingsMap.end()) {
        if (hasFlag(gs->second, UserGuildSettingsFlag::UNREADS_ALL_MESSAGES))
            return true;
        if (hasFlag(gs->second, UserGuildSettingsFlag::UNREADS_ONLY_MENTIONS))
            return false;
    }

    return resolveMessageNotifications(guildId, channelId, parentId) == Discord::MessageNotificationLevel::ALL_MESSAGES;
}

Snowflake ReadStateManager::guildForChannel(Snowflake channelId) const
{
    auto it = channelGuildMap.find(channelId);
    return it != channelGuildMap.end() ? it->second : Snowflake::Invalid;
}

void ReadStateManager::setGuildReadInfo(Snowflake guildId, const Platform::DateTime &joinedAt,
                                        Discord::MessageNotificationLevel defaultMessageNotifications,
                                        bool isCommunity)
{
    if (!guildId.isValid())
        return;

    GuildReadInfo info;
    info.joinedAtMs = joinedAt.valid() ? joinedAt.unixMillis() : 0;
    info.defaultMessageNotifications = defaultMessageNotifications;
    info.isCommunity = isCommunity;
    guildInfo.insert_or_assign(guildId, info);
}

void ReadStateManager::updateGuildDefaults(Snowflake guildId, Discord::MessageNotificationLevel defaultMessageNotifications, bool isCommunity)
{
    auto it = guildInfo.find(guildId);
    if (it == guildInfo.end())
        return;
    if (it->second.defaultMessageNotifications == defaultMessageNotifications && it->second.isCommunity == isCommunity)
        return;

    it->second.defaultMessageNotifications = defaultMessageNotifications;
    it->second.isCommunity = isCommunity;
    guildSettingsUpdated.publish(guildId);
}

void ReadStateManager::registerChannelGuild(Snowflake channelId, Snowflake guildId)
{
    if (!channelId.isValid() || !guildId.isValid())
        return;
    channelGuildMap.insert_or_assign(channelId, guildId);
}

void ReadStateManager::registerChannel(const Discord::Channel &channel, Snowflake guildId)
{
    Snowflake channelId = channel.id.get();
    registerChannelGuild(channelId, guildId);

    if (channel.isVoice())
        voiceChannels.insert(channelId);
    else
        voiceChannels.erase(channelId);

    bool isForum = channel.type.hasValue() &&
                   (channel.type.get() == Discord::ChannelType::GUILD_FORUM ||
                    channel.type.get() == Discord::ChannelType::GUILD_MEDIA);
    if (isForum)
        forumChannels.insert(channelId);
    else
        forumChannels.erase(channelId);

    if (!channel.flags.hasValue())
        return;
    if (channel.flags->testFlag(Discord::ChannelFlag::IS_GUILD_RESOURCE_CHANNEL))
        resourceChannels.insert(channelId);
    else
        resourceChannels.erase(channelId);
}

void ReadStateManager::registerPrivateChannel(const Discord::Channel &channel)
{
    Snowflake channelId = channel.id.get();

    if (channel.isPendingMessageRequest())
        messageRequestChannels.insert(channelId);
    else
        messageRequestChannels.erase(channelId);

    if (channel.lastMessageId.hasValue())
        updateChannelLastMessageId(channelId, channel.lastMessageId.get());
}

void ReadStateManager::removeGuild(Snowflake guildId)
{
    std::vector<Snowflake> channels;
    for (const auto &[channelId, channelGuildId] : channelGuildMap)
        if (channelGuildId == guildId)
            channels.push_back(channelId);
    for (Snowflake channelId : channels) {
        channelReadStates.erase(channelId);
        channelLastMessageIds.erase(channelId);
        ackIdAtSelect.erase(channelId);
        channelOverrideCache.erase(channelId);
        channelGuildMap.erase(channelId);
        resourceChannels.erase(channelId);
        voiceChannels.erase(channelId);
        forumChannels.erase(channelId);
        outgoingAcks.erase(channelId);
        if (activeChannelId == channelId)
            activeChannelId = Snowflake();
    }

    guildInfo.erase(guildId);
    guildSettingsMap.erase(guildId);
    guildOverrideChannels.erase(guildId);
}

int ReadStateManager::getMentionCount(Snowflake channelId) const
{
    auto it = channelReadStates.find(channelId);
    if (it == channelReadStates.end())
        return 0;

    return it->second.mentionCount.hasValue() ? it->second.mentionCount.get() : 0;
}

bool ReadStateManager::isChannelMuted(Snowflake channelId) const
{
    auto it = channelOverrideCache.find(channelId);
    return it != channelOverrideCache.end() && isMutedSetting(it->second);
}

bool ReadStateManager::isGuildMuted(Snowflake guildId) const
{
    auto it = guildSettingsMap.find(guildId);
    return it != guildSettingsMap.end() && isMutedSetting(it->second);
}

bool ReadStateManager::isSuppressEveryone(Snowflake guildId) const
{
    auto it = guildSettingsMap.find(guildId);
    return it != guildSettingsMap.end() && it->second.suppressEveryone.hasValue() && it->second.suppressEveryone.get();
}

bool ReadStateManager::isSuppressRoles(Snowflake guildId) const
{
    auto it = guildSettingsMap.find(guildId);
    return it != guildSettingsMap.end() && it->second.suppressRoles.hasValue() && it->second.suppressRoles.get();
}

bool ReadStateManager::isMuteActive(bool muted, const Discord::MuteConfig *muteConfig)
{
    if (!muted)
        return false;

    if (!muteConfig)
        return true;

    if (!muteConfig->endTime.hasValue())
        return true;

    const std::string &endTimeStr = muteConfig->endTime.get();
    if (endTimeStr.empty())
        return true;

    Platform::DateTime endTime = Platform::DateTime::parse(endTimeStr);
    if (!endTime.valid())
        return true;

    return Platform::DateTime::now() < endTime;
}

Discord::ReadStateEntry &ReadStateManager::entryFor(Snowflake channelId)
{
    auto it = channelReadStates.find(channelId);
    if (it != channelReadStates.end())
        return it->second;

    Discord::ReadStateEntry entry;
    entry.id = channelId;
    entry.mentionCount = 0;
    return channelReadStates.emplace(channelId, std::move(entry)).first->second;
}

void ReadStateManager::onMessageAck(const Discord::MessageAck &ack)
{
    Snowflake channelId = ack.channelId.get();
    Snowflake messageId = ack.messageId.get();
    bool manual = ack.manual.hasValue() && ack.manual.get();
    auto &entry = entryFor(channelId);

    if (manual)
        outgoingAcks.erase(channelId);
    else if (entry.lastMessageId.hasValue() && entry.lastMessageId.get() == messageId)
        return;

    entry.lastMessageId = messageId;
    entry.mentionCount = manual && ack.mentionCount.hasValue() ? ack.mentionCount.get() : 0;
    readStateUpdated.publish(channelId);
}

void ReadStateManager::onUserGuildSettingsUpdate(const Discord::UserGuildSettings &settings)
{
    Snowflake key = settings.guildId.isNull() ? Snowflake(0) : settings.guildId.get();

    guildSettingsMap.insert_or_assign(key, settings);
    rebuildChannelOverrideCache(key);

    guildSettingsUpdated.publish(key);
}

void ReadStateManager::ackLocally(Snowflake channelId, Snowflake messageId)
{
    auto &entry = entryFor(channelId);
    entry.mentionCount = 0;
    if (messageId.isValid())
        entry.lastMessageId = messageId;
    readStateUpdated.publish(channelId);
}

void ReadStateManager::ack(Snowflake channelId, Snowflake messageId, bool immediate)
{
    if (!messageId.isValid())
        messageId = getChannelLastMessageId(channelId);

    bool clearsMentions = getMentionCount(channelId) > 0;
    ackLocally(channelId, messageId);
    if (!messageId.isValid())
        return;

    bool alreadyScheduled = outgoingAcks.contains(channelId);
    outgoingAcks.insert_or_assign(channelId, messageId);
    if (alreadyScheduled)
        return;

    auto token = lifetime();
    Platform::Dispatcher::main().setTimer(std::chrono::milliseconds(immediate || clearsMentions ? 0 : kDeferredAckDelayMs), false,
        [this, token, channelId]() {
            if (!token.expired())
                flushOutgoingAck(channelId);
        });
}

void ReadStateManager::flushOutgoingAck(Snowflake channelId)
{
    auto it = outgoingAcks.find(channelId);
    if (it == outgoingAcks.end())
        return;

    Snowflake messageId = it->second;
    outgoingAcks.erase(it);
    ackRequested.publish(channelId, messageId);
}

void ReadStateManager::setActiveChannel(Snowflake channelId)
{
    if (activeChannelId != channelId) {
        if (channelId.isValid())
            ackIdAtSelect.insert_or_assign(channelId, effectiveAckId(channelId, guildForChannel(channelId)));

        activeChannelId = channelId;
        activeChannelAtBottom = true;
    }

    tryAckActiveChannel();
}

void ReadStateManager::setActiveChannelAtBottom(bool atBottom)
{
    if (activeChannelAtBottom == atBottom)
        return;

    activeChannelAtBottom = atBottom;
    if (atBottom)
        tryAckActiveChannel();
}

bool ReadStateManager::canAutoAckActiveChannel() const
{
    if (!activeChannelId.isValid() || !activeChannelAtBottom)
        return false;
    return Platform::Desktop::applicationActive();
}

void ReadStateManager::tryAckActiveChannel()
{
    if (!canAutoAckActiveChannel() || !hasUnreadOrMentions(activeChannelId))
        return;

    ack(activeChannelId, getChannelLastMessageId(activeChannelId), false);
}

void ReadStateManager::markChannelAsRead(Snowflake channelId, Snowflake lastMessageId)
{
    if (!lastMessageId.isValid() || !hasUnreadOrMentions(channelId))
        return;

    ack(channelId, lastMessageId, true);
}

Snowflake ReadStateManager::markAsReadMessageId(Snowflake channelId) const
{
    if (forumChannels.contains(channelId))
        return Snowflake::fromUnixMs(Platform::DateTime::now().unixMillis());
    return getChannelLastMessageId(channelId);
}

void ReadStateManager::markChannelsAsRead(const std::vector<Snowflake> &channelIds)
{
    std::vector<std::pair<Snowflake, Snowflake>> toAck;
    for (Snowflake channelId : channelIds) {
        if (!forumChannels.contains(channelId) && !hasUnreadOrMentions(channelId))
            continue;
        Snowflake messageId = markAsReadMessageId(channelId);
        if (messageId.isValid())
            toAck.push_back({ channelId, messageId });
    }

    if (toAck.size() == 1) {
        ack(toAck.front().first, toAck.front().second, true);
        return;
    }

    for (const auto &[channelId, messageId] : toAck) {
        outgoingAcks.erase(channelId);
        ackLocally(channelId, messageId);
    }

    if (!toAck.empty())
        bulkAckRequested.publish(toAck);
}

void ReadStateManager::handleMessageCreated(Snowflake channelId, Snowflake messageId, bool fromSelf, bool isMention)
{
    updateChannelLastMessageId(channelId, messageId);

    if (fromSelf) {
        outgoingAcks.erase(channelId);
        ackLocally(channelId, messageId);
        return;
    }

    if (channelId == activeChannelId && canAutoAckActiveChannel()) {
        ack(channelId, messageId, false);
        return;
    }

    if (!isMention)
        return;

    auto &entry = entryFor(channelId);
    entry.mentionCount = getMentionCount(channelId) + 1;
    readStateUpdated.publish(channelId);
}

void ReadStateManager::updateChannelLastMessageId(Snowflake channelId, Snowflake messageId)
{
    if (!messageId.isValid())
        return;

    auto it = channelLastMessageIds.find(channelId);
    if (it == channelLastMessageIds.end() || messageId > it->second)
        channelLastMessageIds.insert_or_assign(channelId, messageId);
}

Snowflake ReadStateManager::getChannelLastMessageId(Snowflake channelId) const
{
    auto it = channelLastMessageIds.find(channelId);
    return it != channelLastMessageIds.end() ? it->second : Snowflake::Invalid;
}

int ReadStateManager::daysSinceDiscordEpoch()
{
    constexpr std::int64_t discordEpochMs = 1420070400000LL;
    return static_cast<int>((Platform::DateTime::now().unixMillis() - discordEpochMs) / kMsPerDay) + 1;
}

void ReadStateManager::rebuildChannelOverrideCache(Snowflake guildSettingsKey)
{
    auto old = guildOverrideChannels.find(guildSettingsKey);
    if (old != guildOverrideChannels.end()) {
        for (const auto &channelId : old->second)
            channelOverrideCache.erase(channelId);
        guildOverrideChannels.erase(old);
    }

    auto it = guildSettingsMap.find(guildSettingsKey);
    if (it == guildSettingsMap.end())
        return;

    const auto &settings = it->second;
    if (!settings.channelOverrides.hasValue())
        return;

    std::unordered_set<Snowflake> newChannels;
    for (const auto &override_ : settings.channelOverrides.get()) {
        Snowflake channelId = override_.channelId.get();
        channelOverrideCache.insert_or_assign(channelId, override_);
        newChannels.insert(channelId);
    }
    guildOverrideChannels.insert_or_assign(guildSettingsKey, std::move(newChannels));
}

} // namespace Core
} // namespace Styx
