#include "PrivateChannels.hpp"

#include <algorithm>
#include <vector>

namespace Styx::Core {
namespace {

void takeRecipientIds(Discord::Channel &channel, const std::vector<Discord::User> &users)
{
    channel.recipientIds = std::vector<Snowflake>{};
    auto &ids = channel.recipientIds.get();
    ids.reserve(users.size());
    for (const auto &user : users)
        ids.push_back(user.id.get());
}

void normalizeMembership(Discord::Channel &channel)
{
    if (channel.recipientIds.hasValue()) {
        auto &ids = channel.recipientIds.get();
        auto output = ids.begin();
        for (auto input = ids.begin(); input != ids.end(); ++input) {
            if (input->isValid() && *input != Snowflake(0) && std::find(ids.begin(), output, *input) == output)
                *output++ = *input;
        }
        ids.erase(output, ids.end());
    }
    channel.recipients.undefine();
}

} // namespace

Discord::Channel mergePrivateChannel(Discord::Channel baseline, const Discord::Channel &update)
{
    if (!update.name.isUndefined()) baseline.name = update.name;
    if (!update.topic.isUndefined()) baseline.topic = update.topic;
    if (!update.position.isUndefined()) baseline.position = update.position;
    if (!update.guildId.isUndefined()) baseline.guildId = update.guildId;
    if (!update.parentId.isUndefined()) baseline.parentId = update.parentId;
    if (!update.permissionOverwrites.isUndefined()) baseline.permissionOverwrites = update.permissionOverwrites;
    if (!update.icon.isUndefined()) baseline.icon = update.icon;
    if (!update.ownerId.isUndefined()) baseline.ownerId = update.ownerId;
    if (!update.rateLimitPerUser.isUndefined()) baseline.rateLimitPerUser = update.rateLimitPerUser;
    if (!update.userLimit.isUndefined()) baseline.userLimit = update.userLimit;
    if (!update.isMessageRequest.isUndefined()) baseline.isMessageRequest = update.isMessageRequest;
    if (!update.isSpam.isUndefined()) baseline.isSpam = update.isSpam;
    if (!update.threadMetadata.isUndefined()) baseline.threadMetadata = update.threadMetadata;
    if (!update.member.isUndefined()) baseline.member = update.member;
    if (!update.availableTags.isUndefined()) {
        baseline.availableTags = update.availableTags;
        baseline.availableTagsJson = update.availableTagsJson;
    }
    if (!update.appliedTags.isUndefined()) baseline.appliedTags = update.appliedTags;
    if (!update.messageCount.isUndefined()) baseline.messageCount = update.messageCount;
    if (!update.memberCount.isUndefined()) baseline.memberCount = update.memberCount;
    if (!update.totalMessageSent.isUndefined()) baseline.totalMessageSent = update.totalMessageSent;
    if (!update.flags.isUndefined()) baseline.flags = update.flags;
    if (!update.defaultSortOrder.isUndefined()) baseline.defaultSortOrder = update.defaultSortOrder;

    if (update.lastMessageId.hasValue()) {
        if (!baseline.lastMessageId.hasValue() || update.lastMessageId.get() > baseline.lastMessageId.get())
            baseline.lastMessageId = update.lastMessageId;
    } else if (update.lastMessageId.isNull() && !baseline.lastMessageId.hasValue()) {
        baseline.lastMessageId = nullptr;
    }

    if (update.recipients.hasValue())
        takeRecipientIds(baseline, update.recipients.get());
    else if (update.recipientIds.hasValue())
        baseline.recipientIds = update.recipientIds;
    else if (!baseline.recipientIds.hasValue() && baseline.recipients.hasValue())
        takeRecipientIds(baseline, baseline.recipients.get());
    normalizeMembership(baseline);
    return baseline;
}

Discord::User mergePrivateUser(Discord::User baseline, const Discord::User &update)
{
    if ((update.identityFieldsPresent & 1) != 0) baseline.username = update.username;
    if ((update.identityFieldsPresent & 2) != 0) baseline.globalName = update.globalName;
    if ((update.identityFieldsPresent & 4) != 0) baseline.avatar = update.avatar;
    if (!update.bot.isUndefined()) baseline.bot = update.bot;
    if (!update.banner.isUndefined()) baseline.banner = update.banner;
    if (!update.accentColor.isUndefined()) baseline.accentColor = update.accentColor;
    if (!update.publicFlags.isUndefined()) baseline.publicFlags = update.publicFlags;
    if (!update.premiumType.isUndefined()) baseline.premiumType = update.premiumType;
    if (!update.discriminator.isUndefined()) baseline.discriminator = update.discriminator;
    if (!update.mfaEnabled.isUndefined()) baseline.mfaEnabled = update.mfaEnabled;
    baseline.identityFieldsPresent |= update.identityFieldsPresent;
    return baseline;
}

bool applyPrivateRecipient(Discord::Channel &channel, Snowflake userId, bool added)
{
    if (!userId.isValid() || userId == Snowflake(0))
        return false;
    if (channel.recipients.hasValue())
        takeRecipientIds(channel, channel.recipients.get());
    if (!channel.recipientIds.hasValue())
        channel.recipientIds = std::vector<Snowflake>{};
    normalizeMembership(channel);
    auto &ids = channel.recipientIds.get();
    const auto found = std::find(ids.begin(), ids.end(), userId);
    if (added) {
        if (found != ids.end())
            return false;
        ids.push_back(userId);
    } else {
        if (found == ids.end())
            return false;
        ids.erase(found);
    }
    return true;
}

} // namespace Styx::Core
