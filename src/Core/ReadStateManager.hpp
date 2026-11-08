#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Platform/Native.hpp"
#include "Platform/Trackable.hpp"

#include "ChannelReadState.hpp"
#include "Snowflake.hpp"
#include "Discord/Entities.hpp"
#include "Discord/Events.hpp"

namespace Styx {
namespace Core {

class PermissionManager;

class ReadStateManager : public Platform::Trackable
{
public:
    explicit ReadStateManager(Snowflake accountId, PermissionManager *perms);

    void loadFromReady(const std::vector<Discord::ReadStateEntry> &readStates,
                       const std::vector<Discord::UserGuildSettings> &guildSettings,
                       int notificationSettingsFlags);
    void setNotificationSettingsFlags(int flags);

    void setGuildReadInfo(Snowflake guildId, const Platform::DateTime &joinedAt,
                          Discord::MessageNotificationLevel defaultMessageNotifications,
                          bool isCommunity);
    void updateGuildDefaults(Snowflake guildId, Discord::MessageNotificationLevel defaultMessageNotifications, bool isCommunity);
    void registerChannelGuild(Snowflake channelId, Snowflake guildId);
    void registerChannel(const Discord::Channel &channel, Snowflake guildId);
    void registerPrivateChannel(const Discord::Channel &channel);
    void removeGuild(Snowflake guildId);

    [[nodiscard]] ChannelReadState computeChannelReadState(Snowflake channelId, Snowflake guildId, Snowflake parentId) const;
    [[nodiscard]] ChannelReadState computeDMReadState(Snowflake channelId) const;
    [[nodiscard]] ChannelReadState computeThreadReadState(Snowflake threadId, Snowflake guildId,
                                                          Snowflake parentId, Snowflake categoryId,
                                                          bool joined) const;
    [[nodiscard]] ChannelReadState computeForumPostReadState(Snowflake postId, Snowflake guildId,
                                                             Snowflake forumId, Snowflake categoryId) const;

    bool isChannelUnread(Snowflake channelId, Snowflake channelLastMessageId, Snowflake guildId) const;
    [[nodiscard]] bool hasUnreadOrMentions(Snowflake channelId) const;
    int getMentionCount(Snowflake channelId) const;
    bool isChannelMuted(Snowflake channelId) const;
    bool isGuildMuted(Snowflake guildId) const;
    [[nodiscard]] bool isSuppressEveryone(Snowflake guildId) const;
    [[nodiscard]] bool isSuppressRoles(Snowflake guildId) const;
    Discord::MessageNotificationLevel resolveMessageNotifications(Snowflake guildId,
                                                                  Snowflake channelId,
                                                                  Snowflake parentId) const;
    [[nodiscard]] bool isMutedThroughParents(Snowflake channelId, Snowflake parentId, Snowflake categoryId, Snowflake guildId) const;
    [[nodiscard]] Snowflake activeChannel() const { return activeChannelId; }

    [[nodiscard]] bool isForumPostUnread(Snowflake threadId, Snowflake lastMessageId, bool archived) const;
    // should it be in channel list
    [[nodiscard]] bool isThreadRelevant(const Discord::Channel &thread) const;
    [[nodiscard]] bool isForumPostNew(Snowflake threadId, Snowflake forumId, Snowflake guildId, bool archived) const;
    void markForumPostAsRead(Snowflake threadId, Snowflake lastMessageId);
    [[nodiscard]] bool hasBeenRead(Snowflake channelId) const;
    [[nodiscard]] Snowflake effectiveAckId(Snowflake channelId, Snowflake guildId) const;

    void onMessageAck(const Discord::MessageAck &ack);
    void onUserGuildSettingsUpdate(const Discord::UserGuildSettings &settings);
    void onNotificationSettingsUpdate(const Discord::NotificationSettings &settings);

    void setActiveChannel(Snowflake channelId);
    void setActiveChannelAtBottom(bool atBottom);
    void markChannelAsRead(Snowflake channelId, Snowflake lastMessageId);
    void markChannelsAsRead(const std::vector<Snowflake> &channelIds);
    void handleMessageCreated(Snowflake channelId, Snowflake messageId, bool fromSelf, bool isMention);

    void updateChannelLastMessageId(Snowflake channelId, Snowflake messageId);
    [[nodiscard]] Snowflake getChannelLastMessageId(Snowflake channelId) const;

    static bool isMuteActive(bool muted, const Discord::MuteConfig *muteConfig);
    static int daysSinceDiscordEpoch();

    Platform::Signal<Snowflake> readStateUpdated;
    Platform::Signal<Snowflake> guildSettingsUpdated;
    Platform::Signal<Snowflake, Snowflake> ackRequested;
    Platform::Signal<const std::vector<std::pair<Snowflake, Snowflake>> &> bulkAckRequested;

private:
    Discord::ReadStateEntry &entryFor(Snowflake channelId);
    [[nodiscard]] Snowflake markAsReadMessageId(Snowflake channelId) const;
    void ackLocally(Snowflake channelId, Snowflake messageId);
    void ack(Snowflake channelId, Snowflake messageId, bool immediate);
    void flushOutgoingAck(Snowflake channelId);
    [[nodiscard]] bool canAutoAckActiveChannel() const;
    void tryAckActiveChannel();

    void rebuildChannelOverrideCache(Snowflake guildSettingsKey);

    [[nodiscard]] bool unreadCountsForGuild(Snowflake guildId, Snowflake channelId, Snowflake parentId) const;
    [[nodiscard]] bool canTrackUnreads(Snowflake channelId) const;
    [[nodiscard]] bool isOptInGuild(Snowflake guildId) const;
    [[nodiscard]] bool isChannelOptedIn(Snowflake channelId) const;
    [[nodiscard]] bool hasRecentlyVisitedAndRead(Snowflake channelId, Snowflake lastMessageId) const;
    Snowflake guildForChannel(Snowflake channelId) const;

    struct GuildReadInfo
    {
        std::int64_t joinedAtMs = 0;
        Discord::MessageNotificationLevel defaultMessageNotifications = Discord::MessageNotificationLevel::ALL_MESSAGES;
        bool isCommunity = false;
    };

    Snowflake accountId;
    PermissionManager *permissionManager;

    std::unordered_map<Snowflake, Discord::ReadStateEntry> channelReadStates;
    std::unordered_map<Snowflake, Snowflake> channelLastMessageIds;
    std::unordered_map<Snowflake, Snowflake> ackIdAtSelect;
    std::unordered_map<Snowflake, Discord::UserGuildSettings> guildSettingsMap; // Snowflake(0) for DMs

    std::unordered_map<Snowflake, Discord::ChannelOverride> channelOverrideCache;
    std::unordered_map<Snowflake, std::unordered_set<Snowflake>> guildOverrideChannels;

    std::unordered_map<Snowflake, GuildReadInfo> guildInfo;
    std::unordered_map<Snowflake, Snowflake> channelGuildMap;
    std::unordered_set<Snowflake> resourceChannels;
    std::unordered_set<Snowflake> voiceChannels;
    std::unordered_set<Snowflake> forumChannels;
    std::unordered_set<Snowflake> messageRequestChannels;
    bool useNewNotifications = false;

    Snowflake activeChannelId;
    bool activeChannelAtBottom = true;
    std::unordered_map<Snowflake, Snowflake> outgoingAcks;
};

} // namespace Core
} // namespace Styx
