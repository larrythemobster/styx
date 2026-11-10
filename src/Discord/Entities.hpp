#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "Platform/Native.hpp"
#include "Platform/Image.hpp"


#include "Enums.hpp"

#include "Core/Snowflake.hpp"
#include "Core/JsonUtils.hpp"
#include "CdnUrls.hpp"

namespace Styx {
namespace Discord {

struct User : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<std::string> username;
    Field<std::string, false, true> globalName;
    Field<std::string, false, true> avatar;
    Field<bool, true> bot;
    Field<std::string, true, true> banner;
    Field<int, true, true> accentColor;
    Field<int, true> publicFlags;
    Field<PremiumType, true, true> premiumType;
    Field<std::string, true> discriminator;
    Field<bool, true> mfaEnabled;

    // Required identity fields may be omitted in private-channel user updates.
    unsigned char identityFieldsPresent = 7;

    static User fromJson(const nlohmann::json &obj)
    {
        User user;
        user.identityFieldsPresent = static_cast<unsigned char>(
            (obj.contains("username") ? 1 : 0) |
            (obj.contains("global_name") ? 2 : 0) |
            (obj.contains("avatar") ? 4 : 0));
        get(obj, "id", user.id);
        get(obj, "username", user.username);
        get(obj, "global_name", user.globalName);
        get(obj, "avatar", user.avatar);
        get(obj, "bot", user.bot);
        get(obj, "banner", user.banner);
        get(obj, "accent_color", user.accentColor);
        get(obj, "public_flags", user.publicFlags);
        get(obj, "premium_type", user.premiumType);
        get(obj, "discriminator", user.discriminator);
        get(obj, "mfa_enabled", user.mfaEnabled);
        return user;
    }

    std::string tag() const
    {
        if (discriminator.hasValue() && !discriminator->empty() && discriminator.get() != "0")
            return username.get() + '#' + discriminator.get();
        return username;
    }

    std::string getDisplayName() const
    {
        if (globalName.hasValue() && !globalName->empty())
            return globalName;
        return username;
    }
};

struct ActivityTimestamps : Core::JsonUtils::JsonObject
{
    // milliseconds
    Field<std::int64_t, true> start;
    Field<std::int64_t, true> end;

    static ActivityTimestamps fromJson(const nlohmann::json &obj)
    {
        ActivityTimestamps timestamps;
        get(obj, "start", timestamps.start);
        get(obj, "end", timestamps.end);
        return timestamps;
    }

    bool operator==(const ActivityTimestamps &) const = default;
};

struct ActivityEmoji : Core::JsonUtils::JsonObject
{
    Field<std::string, true> name;
    Field<Core::Snowflake, true, true> id;

    static ActivityEmoji fromJson(const nlohmann::json &obj)
    {
        ActivityEmoji emoji;
        get(obj, "name", emoji.name);
        get(obj, "id", emoji.id);
        return emoji;
    }

    bool isUnicode() const { return !id.hasValue(); }

    std::string nameText() const { return name.hasValue() ? name.get() : std::string(); }

    std::string stillImageUrl(int size) const
    {
        if (isUnicode())
            return {};
        return Cdn::emoji(*id, size, false);
    }

    bool operator==(const ActivityEmoji &) const = default;
};

struct ActivityParty : Core::JsonUtils::JsonObject
{
    int currentSize = 0;
    int maxSize = 0;

    static ActivityParty fromJson(const nlohmann::json &obj)
    {
        ActivityParty party;

        const nlohmann::json size = obj.value("size", nlohmann::json());
        if (size.is_array() && size.size() >= 2) {
            party.currentSize = size[0].get<int>();
            party.maxSize = size[1].get<int>();
        }

        return party;
    }

    bool hasSize() const { return currentSize > 0; }

    bool operator==(const ActivityParty &) const = default;
};

struct ActivityAssets : Core::JsonUtils::JsonObject
{
    Field<std::string, true> largeImage;
    Field<std::string, true> largeText;
    Field<std::string, true> smallImage;
    Field<std::string, true> smallText;

    static ActivityAssets fromJson(const nlohmann::json &obj)
    {
        ActivityAssets assets;
        get(obj, "large_image", assets.largeImage);
        get(obj, "large_text", assets.largeText);
        get(obj, "small_image", assets.smallImage);
        get(obj, "small_text", assets.smallText);
        return assets;
    }

    bool operator==(const ActivityAssets &) const = default;
};

struct Activity : Core::JsonUtils::JsonObject
{
    Field<std::string, true> name;
    Field<ActivityType, true> type;
    Field<ActivityTimestamps, true> timestamps;
    Field<Core::Snowflake, true> applicationId;
    Field<StatusDisplayType, true, true> statusDisplayType;
    Field<std::string, true, true> details;
    Field<std::string, true, true> state;
    Field<ActivityFlags, true> flags;
    Field<ActivityEmoji, true, true> emoji;
    Field<ActivityParty, true> party;
    Field<ActivityAssets, true> assets;
    Field<std::int64_t, true> createdAt;

    static Activity fromJson(const nlohmann::json &obj)
    {
        Activity activity;
        get(obj, "name", activity.name);
        get(obj, "type", activity.type);
        get(obj, "timestamps", activity.timestamps);
        get(obj, "application_id", activity.applicationId);
        get(obj, "status_display_type", activity.statusDisplayType);
        get(obj, "details", activity.details);
        get(obj, "state", activity.state);
        get(obj, "flags", activity.flags);
        get(obj, "emoji", activity.emoji);
        get(obj, "party", activity.party);
        get(obj, "assets", activity.assets);
        get(obj, "created_at", activity.createdAt);
        return activity;
    }

    ActivityType kind() const
    {
        return type.hasValue() ? type.get() : ActivityType::PLAYING;
    }

    bool isCustom() const { return kind() == ActivityType::CUSTOM; }

    std::string nameText() const { return name.hasValue() ? name.get() : std::string(); }

    std::string detailsText() const { return details.hasValue() ? details.get() : std::string(); }

    std::string stateText() const { return state.hasValue() ? state.get() : std::string(); }

    std::int64_t createdAtMs() const { return createdAt.hasValue() ? createdAt.get() : 0; }

    std::int64_t startMs() const
    {
        if (timestamps.hasValue() && timestamps->start.hasValue())
            return timestamps->start.get();
        return createdAtMs();
    }

    std::int64_t endMs() const
    {
        if (timestamps.hasValue() && timestamps->end.hasValue())
            return timestamps->end.get();
        return 0;
    }

    bool operator==(const Activity &) const = default;

    bool hasFlag(ActivityFlag flag) const
    {
        return flags.hasValue() && flags->testFlag(flag);
    }
};

struct ClientStatus : Core::JsonUtils::JsonObject
{
    Field<std::string, true> desktop;
    Field<std::string, true> mobile;
    Field<std::string, true> web;
    Field<std::string, true> vr;

    static ClientStatus fromJson(const nlohmann::json &obj)
    {
        ClientStatus status;
        get(obj, "desktop", status.desktop);
        get(obj, "mobile", status.mobile);
        get(obj, "web", status.web);
        get(obj, "vr", status.vr);
        return status;
    }

    bool isMobileOnly() const
    {
        auto isOnline = [](const auto &field) {
            return field.hasValue() && field.get() == "online";
        };
        return isOnline(mobile) && !isOnline(desktop) && !isOnline(vr);
    }
};

struct Presence : Core::JsonUtils::JsonObject
{
    // user might only hae an id
    Field<User, true> user;
    Field<Core::Snowflake, true> guildId;
    Field<std::string, true> status;
    Field<std::vector<Activity>, true> activities;
    Field<ClientStatus, true> clientStatus;

    static Presence fromJson(const nlohmann::json &obj)
    {
        Presence presence;
        get(obj, "user", presence.user);
        get(obj, "guild_id", presence.guildId);
        get(obj, "status", presence.status);
        get(obj, "activities", presence.activities);
        get(obj, "client_status", presence.clientStatus);

        if (!presence.user.hasValue() && obj.contains("user_id")) {
            User user;
            get(obj, "user_id", user.id);
            presence.user = user;
        }

        return presence;
    }

    Core::Snowflake userId() const
    {
        if (user.hasValue() && user->id.hasValue())
            return user->id.get();
        return Core::Snowflake();
    }

    StatusType statusType() const
    {
        return parseStatus(status.hasValue() ? status.get() : std::string());
    }
};

struct UserSession : Core::JsonUtils::JsonObject
{
    Field<std::string, true> sessionId;
    Field<std::string, true> status;
    Field<std::vector<Activity>, true> activities;
    Field<bool, true> active;

    static UserSession fromJson(const nlohmann::json &obj)
    {
        UserSession session;
        get(obj, "session_id", session.sessionId);
        get(obj, "status", session.status);
        get(obj, "activities", session.activities);
        get(obj, "active", session.active);
        return session;
    }

    bool isAggregate() const
    {
        return sessionId.hasValue() && sessionId.get() == "all";
    }

    StatusType statusType() const
    {
        return parseStatus(status.hasValue() ? status.get() : std::string());
    }
};

struct Member : Core::JsonUtils::JsonObject
{
    Field<User, true> user;
    Field<std::string, true, true> nick;
    Field<std::string, true, true> avatar;
    Field<std::vector<Core::Snowflake>, true> roles;
    Field<Platform::DateTime, true> joinedAt;
    Field<Platform::DateTime, true, true> premiumSince;
    Field<bool, true> deaf;
    Field<bool, true> mute;
    Field<int, true> flags;
    Field<bool, true> pending;
    Field<Platform::DateTime, true, true> communicationDisabledUntil;
    Field<Platform::DateTime, true, true> unusualDmActivityUntil;
    Field<Core::Snowflake, true> userId; // supplemental
    Field<Presence, true> presence;

    static constexpr int FlagQuarantinedName = 1 << 7;

    static Member fromJson(const nlohmann::json &obj)
    {
        Member member;
        get(obj, "user", member.user);
        get(obj, "nick", member.nick);
        get(obj, "avatar", member.avatar);
        get(obj, "roles", member.roles);
        get(obj, "joined_at", member.joinedAt);
        get(obj, "premium_since", member.premiumSince);
        get(obj, "deaf", member.deaf);
        get(obj, "mute", member.mute);
        get(obj, "flags", member.flags);
        get(obj, "pending", member.pending);
        get(obj, "communication_disabled_until", member.communicationDisabledUntil);
        get(obj, "unusual_dm_activity_until", member.unusualDmActivityUntil);
        get(obj, "user_id", member.userId);
        get(obj, "presence", member.presence);
        return member;
    }
};

struct MentionedUser : User
{
    Field<Member, true> member;

    static MentionedUser fromJson(const nlohmann::json &obj)
    {
        MentionedUser mentioned;
        static_cast<User &>(mentioned) = User::fromJson(obj);
        get(obj, "member", mentioned.member);
        return mentioned;
    }
};

struct RoleColors : Core::JsonUtils::JsonObject
{
    Field<int> primaryColor;
    Field<int, false, true> secondaryColor;
    Field<int, false, true> tertiaryColor;

    static RoleColors fromJson(const nlohmann::json &obj)
    {
        RoleColors colors;
        get(obj, "primary_color", colors.primaryColor);
        get(obj, "secondary_color", colors.secondaryColor);
        get(obj, "tertiary_color", colors.tertiaryColor);
        return colors;
    }

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "primary_color", primaryColor);
        insert(obj, "secondary_color", secondaryColor);
        insert(obj, "tertiary_color", tertiaryColor);
        return obj;
    }

    static RoleColors solid(int color)
    {
        RoleColors colors;
        colors.primaryColor = color;
        colors.secondaryColor = nullptr;
        colors.tertiaryColor = nullptr;
        return colors;
    }
};

struct RoleTags : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake, true> botId;
    Field<Core::Snowflake, true> integrationId;
    Field<Core::Snowflake, true> subscriptionListingId;
    bool premiumSubscriber = false;
    bool guildConnections = false;

    static RoleTags fromJson(const nlohmann::json &obj)
    {
        RoleTags tags;
        get(obj, "bot_id", tags.botId);
        get(obj, "integration_id", tags.integrationId);
        get(obj, "subscription_listing_id", tags.subscriptionListingId);
        tags.premiumSubscriber = obj.contains("premium_subscriber");
        tags.guildConnections = obj.contains("guild_connections");
        return tags;
    }
};

struct Role : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<std::string> name;
    Field<Permissions> permissions;
    Field<int> position;
    Field<int, true> color;
    Field<RoleColors, true, true> colors;
    Field<bool, true> hoist;
    Field<std::string, true, true> icon;
    Field<std::string, true, true> unicodeEmoji;
    Field<bool, true> managed;
    Field<bool, true> mentionable;
    Field<int, true> flags;
    Field<RoleTags, true> tags;
    std::string tagsJson;

    static Role fromJson(const nlohmann::json &obj)
    {
        Role role;
        get(obj, "id", role.id);
        get(obj, "name", role.name);
        if (const auto it = obj.find("permissions"); it != obj.end() && !it->is_null())
            role.permissions = Permissions::fromInt(it->is_string() ? std::stoull(it->get<std::string>()) : it->get<std::uint64_t>());
        get(obj, "position", role.position);
        get(obj, "color", role.color);
        get(obj, "colors", role.colors);
        get(obj, "hoist", role.hoist);
        get(obj, "icon", role.icon);
        get(obj, "unicode_emoji", role.unicodeEmoji);
        get(obj, "managed", role.managed);
        get(obj, "mentionable", role.mentionable);
        get(obj, "flags", role.flags);
        get(obj, "tags", role.tags);
        if (obj.contains("tags"))
            role.tagsJson = obj.value("tags", nlohmann::json()).dump();
        return role;
    }

    [[nodiscard]] bool isManaged() const { return managed.valueOr(false); }

    bool hasColor() const
    {
        return color.hasValue() && color.get() != 0;
    }

    Platform::Color getColor() const
    {
        return hasColor() ? Platform::Color::rgb(color.get()) : Platform::Color();
    }
};

struct PermissionOverwrite : Core::JsonUtils::JsonObject
{
    enum class Type {
        Role = 0,
        Member = 1,
    };

    Field<Core::Snowflake> id;
    Field<Type> type;
    Field<Permissions> allow;
    Field<Permissions> deny;

    static PermissionOverwrite fromJson(const nlohmann::json &obj)
    {
        PermissionOverwrite overwrite;
        get(obj, "id", overwrite.id);
        get(obj, "type", overwrite.type);
        if (const auto it = obj.find("allow"); it != obj.end() && !it->is_null())
            overwrite.allow = Permissions::fromInt(it->is_string() ? std::stoull(it->get<std::string>()) : it->get<std::uint64_t>());
        if (const auto it = obj.find("deny"); it != obj.end() && !it->is_null())
            overwrite.deny = Permissions::fromInt(it->is_string() ? std::stoull(it->get<std::string>()) : it->get<std::uint64_t>());
        return overwrite;
    }
};

struct ThreadMetadata : Core::JsonUtils::JsonObject
{
    Field<bool> archived;
    Field<int, true> autoArchiveDuration; // minutes
    Field<Platform::DateTime, true> archiveTimestamp; // when the archive status last changed

    static ThreadMetadata fromJson(const nlohmann::json &obj)
    {
        ThreadMetadata meta;
        get(obj, "archived", meta.archived);
        get(obj, "auto_archive_duration", meta.autoArchiveDuration);
        get(obj, "archive_timestamp", meta.archiveTimestamp);
        return meta;
    }
};

struct ForumTag : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<std::string> name;
    Field<Core::Snowflake, false, true> emojiId;
    Field<std::string, false, true> emojiName;

    static ForumTag fromJson(const nlohmann::json &obj)
    {
        ForumTag tag;
        get(obj, "id", tag.id);
        get(obj, "name", tag.name);
        get(obj, "emoji_id", tag.emojiId);
        get(obj, "emoji_name", tag.emojiName);
        return tag;
    }
};

struct ThreadMember : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake, true> id; // thread id
    Field<Core::Snowflake, true> userId;
    Field<Platform::DateTime> joinTimestamp;

    static ThreadMember fromJson(const nlohmann::json &obj)
    {
        ThreadMember member;
        get(obj, "id", member.id);
        get(obj, "user_id", member.userId);
        get(obj, "join_timestamp", member.joinTimestamp);
        return member;
    }
};

struct Channel : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<ChannelType> type;
    Field<std::string, true, true> name;
    Field<std::string, true, true> topic;
    Field<int, true> position;
    Field<Core::Snowflake, true> guildId;
    Field<Core::Snowflake, true, true> parentId;
    Field<std::vector<PermissionOverwrite>, true> permissionOverwrites;
    Field<std::vector<User>, true> recipients;
    Field<std::vector<Core::Snowflake>, true> recipientIds;
    Field<Core::Snowflake, true, true> lastMessageId;
    Field<std::string, true, true> icon;
    Field<Core::Snowflake, true> ownerId;
    Field<int, true> rateLimitPerUser;
    Field<int, true> userLimit;
    Field<bool, true> isMessageRequest;
    Field<bool, true> isSpam;

    // forum/thread specific
    Field<ThreadMetadata, true> threadMetadata;
    Field<ThreadMember, true> member;
    Field<std::vector<ForumTag>, true> availableTags;
    Field<std::vector<Core::Snowflake>, true> appliedTags;
    Field<int, true> messageCount;
    Field<int, true> memberCount;
    Field<int, true> totalMessageSent;
    Field<ChannelFlags, true> flags;
    Field<int, true, true> defaultSortOrder;

    // available_tags persistence
    std::string availableTagsJson;

    static Channel fromJson(const nlohmann::json &obj)
    {
        Channel channel;
        get(obj, "id", channel.id);
        get(obj, "type", channel.type);
        get(obj, "name", channel.name);
        get(obj, "topic", channel.topic);
        get(obj, "position", channel.position);
        get(obj, "guild_id", channel.guildId);
        get(obj, "parent_id", channel.parentId);
        get(obj, "permission_overwrites", channel.permissionOverwrites);
        get(obj, "recipients", channel.recipients);
        get(obj, "recipient_ids", channel.recipientIds);
        get(obj, "last_message_id", channel.lastMessageId);
        get(obj, "icon", channel.icon);
        get(obj, "owner_id", channel.ownerId);
        get(obj, "rate_limit_per_user", channel.rateLimitPerUser);
        get(obj, "user_limit", channel.userLimit);
        get(obj, "is_message_request", channel.isMessageRequest);
        get(obj, "is_spam", channel.isSpam);
        get(obj, "thread_metadata", channel.threadMetadata);
        get(obj, "member", channel.member);
        get(obj, "available_tags", channel.availableTags);
        get(obj, "applied_tags", channel.appliedTags);
        get(obj, "message_count", channel.messageCount);
        get(obj, "member_count", channel.memberCount);
        get(obj, "total_message_sent", channel.totalMessageSent);
        get(obj, "flags", channel.flags);
        get(obj, "default_sort_order", channel.defaultSortOrder);

        if (obj.contains("available_tags")) {
            nlohmann::json doc(obj.value("available_tags", nlohmann::json()));
            channel.availableTagsJson = doc.dump();
        }

        return channel;
    }

    Core::Snowflake effectiveLastMessageId() const
    {
        if (lastMessageId.hasValue() && lastMessageId->isValid())
            return lastMessageId.get();
        return id.get();
    }

    bool isArchived() const
    {
        return threadMetadata.hasValue() && threadMetadata->archived.hasValue() && threadMetadata->archived.get();
    }

    bool isPinned() const { return flags.hasValue() && flags->testFlag(ChannelFlag::PINNED); }

    bool isThread() const { return type.hasValue() && isThreadType(type.get()); }

    bool isVoice() const { return type.hasValue() && isVoiceType(type.get()); }

    bool isPendingMessageRequest() const
    {
        return (isMessageRequest.hasValue() && isMessageRequest.get()) || (isSpam.hasValue() && isSpam.get());
    }
};

struct Emoji : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake, false, true> id; // null for unicode emojis
    Field<std::string> name;
    Field<bool, true> animated;
    Field<std::vector<Core::Snowflake>, true> roles;
    Field<bool, true> managed;
    Field<bool, true> available;
    Field<User, true> user;

    static Emoji fromJson(const nlohmann::json &obj)
    {
        Emoji emoji;
        get(obj, "id", emoji.id);
        get(obj, "name", emoji.name);
        get(obj, "animated", emoji.animated);
        get(obj, "roles", emoji.roles);
        get(obj, "managed", emoji.managed);
        get(obj, "available", emoji.available);
        get(obj, "user", emoji.user);
        return emoji;
    }

    static Emoji unicode(const std::string &surrogates)
    {
        Emoji emoji;
        emoji.id = nullptr;
        emoji.name = surrogates;
        return emoji;
    }

    static Emoji custom(Core::Snowflake id, const std::string &name, bool animated)
    {
        Emoji emoji;
        emoji.id = id;
        emoji.name = name;
        emoji.animated = animated;
        return emoji;
    }

    bool isUnicode() const { return !id.hasValue(); }

    bool isAnimated() const { return animated.hasValue() && *animated; }

    [[nodiscard]] bool sameEmoji(const Emoji &other) const
    {
        if (isUnicode() != other.isUnicode())
            return false;
        return isUnicode() ? name.get() == other.name.get() : id.get() == other.id.get();
    }

    std::string reactionKey() const
    {
        return isUnicode() ? name.get() : name.get() + ":" + id->toString();
    }

    std::string getImageUrl(bool animatedFrames = false) const
    {
        if (isUnicode())
            return {};
        return Cdn::emoji(*id, 48, animatedFrames);
    }
};

struct GuildPremiumFeatures : Core::JsonUtils::JsonObject
{
    Field<int, true> additionalEmojiSlots;
    Field<int, true> additionalStickerSlots;

    static GuildPremiumFeatures fromJson(const nlohmann::json &obj)
    {
        GuildPremiumFeatures features;
        get(obj, "additional_emoji_slots", features.additionalEmojiSlots);
        get(obj, "additional_sticker_slots", features.additionalStickerSlots);
        return features;
    }
};

struct GuildIncidentsData : Core::JsonUtils::JsonObject
{
    Field<Platform::DateTime, true, true> invitesDisabledUntil;
    Field<Platform::DateTime, true, true> dmsDisabledUntil;
    Field<int, true, true> lockdownDurationHours;

    static GuildIncidentsData fromJson(const nlohmann::json &obj)
    {
        GuildIncidentsData data;
        get(obj, "invites_disabled_until", data.invitesDisabledUntil);
        get(obj, "dms_disabled_until", data.dmsDisabledUntil);
        get(obj, "lockdown_duration_hours", data.lockdownDurationHours);
        return data;
    }
};

struct Guild : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<std::string> name;
    Field<std::string> icon;
    Field<Core::Snowflake> ownerId;
    Field<std::vector<Role>, true> roles;
    Field<PremiumTier, true> premiumTier;
    Field<int, true> premiumSubscriptionCount;
    Field<Core::Snowflake, false, true> rulesChannelId;
    Field<MessageNotificationLevel, true> defaultMessageNotifications;
    Field<std::vector<std::string>, true> features;
    Field<std::string, true, true> description;
    Field<std::string, true, true> banner;
    Field<std::string, true, true> splash;
    Field<Core::Snowflake, true, true> afkChannelId;
    Field<int, true> afkTimeout;
    Field<Core::Snowflake, true, true> systemChannelId;
    Field<SystemChannelFlags, true> systemChannelFlags;
    Field<VerificationLevel, true> verificationLevel;
    Field<ExplicitContentFilter, true> explicitContentFilter;
    Field<MfaLevel, true> mfaLevel;
    Field<bool, true> premiumProgressBarEnabled;
    Field<GuildPremiumFeatures, true, true> premiumFeatures;
    Field<GuildIncidentsData, true, true> incidentsData;

    static Guild fromJson(const nlohmann::json &obj)
    {
        Guild guild;
        get(obj, "id", guild.id);
        get(obj, "name", guild.name);
        get(obj, "icon", guild.icon);
        get(obj, "owner_id", guild.ownerId);
        get(obj, "roles", guild.roles);
        get(obj, "premium_tier", guild.premiumTier);
        get(obj, "premium_subscription_count", guild.premiumSubscriptionCount);
        get(obj, "rules_channel_id", guild.rulesChannelId);
        get(obj, "default_message_notifications", guild.defaultMessageNotifications);
        get(obj, "features", guild.features);
        get(obj, "description", guild.description);
        get(obj, "banner", guild.banner);
        get(obj, "splash", guild.splash);
        get(obj, "afk_channel_id", guild.afkChannelId);
        get(obj, "afk_timeout", guild.afkTimeout);
        get(obj, "system_channel_id", guild.systemChannelId);
        get(obj, "system_channel_flags", guild.systemChannelFlags);
        get(obj, "verification_level", guild.verificationLevel);
        get(obj, "explicit_content_filter", guild.explicitContentFilter);
        get(obj, "mfa_level", guild.mfaLevel);
        get(obj, "premium_progress_bar_enabled", guild.premiumProgressBarEnabled);
        get(obj, "premium_features", guild.premiumFeatures);
        get(obj, "incidents_data", guild.incidentsData);
        return guild;
    }

    [[nodiscard]] bool hasFeature(const std::string &feature) const
    {
        return features.hasValue() && std::find(features->begin(), features->end(), feature) != features->end();
    }

    [[nodiscard]] bool invitesPaused() const
    {
        if (hasFeature("INVITES_DISABLED"))
            return true;
        return incidentsData.hasValue() &&
               incidentsData->invitesDisabledUntil.hasValue() &&
               incidentsData->invitesDisabledUntil.get() > Platform::DateTime::now();
    }
};

struct Sticker : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<std::string> name;
    Field<std::string, true, true> description;
    Field<std::string, true> tags;
    Field<StickerFormatType> formatType;
    Field<bool, true> available;
    Field<Core::Snowflake, true> guildId;
    Field<User, true> user;

    static Sticker fromJson(const nlohmann::json &obj)
    {
        Sticker sticker;
        get(obj, "id", sticker.id);
        get(obj, "name", sticker.name);
        get(obj, "description", sticker.description);
        get(obj, "tags", sticker.tags);
        get(obj, "format_type", sticker.formatType);
        get(obj, "available", sticker.available);
        get(obj, "guild_id", sticker.guildId);
        get(obj, "user", sticker.user);
        return sticker;
    }
};

struct Ban : Core::JsonUtils::JsonObject
{
    Field<User> user;
    Field<std::string, false, true> reason;

    static Ban fromJson(const nlohmann::json &obj)
    {
        Ban ban;
        get(obj, "user", ban.user);
        get(obj, "reason", ban.reason);
        return ban;
    }
};

struct Invite : Core::JsonUtils::JsonObject
{
    Field<std::string> code;
    Field<Core::Snowflake, true> guildId;
    Field<Core::Snowflake, true> channelId;
    Field<Channel, true, true> channel;
    Field<User, true> inviter;
    Field<int, true> uses;
    Field<int, true> maxUses;
    Field<int, true> maxAge;
    Field<bool, true> temporary;
    Field<Platform::DateTime, true> createdAt;
    Field<Platform::DateTime, true, true> expiresAt;

    static Invite fromJson(const nlohmann::json &obj)
    {
        Invite invite;
        get(obj, "code", invite.code);
        get(obj, "guild_id", invite.guildId);
        get(obj, "channel_id", invite.channelId);
        get(obj, "channel", invite.channel);
        get(obj, "inviter", invite.inviter);
        get(obj, "uses", invite.uses);
        get(obj, "max_uses", invite.maxUses);
        get(obj, "max_age", invite.maxAge);
        get(obj, "temporary", invite.temporary);
        get(obj, "created_at", invite.createdAt);
        get(obj, "expires_at", invite.expiresAt);
        return invite;
    }

    [[nodiscard]] Core::Snowflake resolvedChannelId() const
    {
        if (channelId.hasValue())
            return channelId.get();
        if (channel.hasValue())
            return channel->id.get();
        return {};
    }

    [[nodiscard]] Platform::DateTime expiryTime() const
    {
        if (expiresAt.hasValue() && expiresAt->valid())
            return expiresAt.get();
        if (maxAge.valueOr(0) > 0 && createdAt.hasValue())
            return createdAt->addSeconds(maxAge.get());
        return {};
    }

    [[nodiscard]] bool hasExpired(const Platform::DateTime &now) const
    {
        const Platform::DateTime expiry = expiryTime();
        return expiry.valid() && expiry <= now;
    }
};

struct VoiceState : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake, true> guildId;
    Field<Core::Snowflake, false, true> channelId;
    Field<Core::Snowflake> userId;
    Field<std::string> sessionId;
    Field<bool> deaf;
    Field<bool> mute;
    Field<bool> selfDeaf;
    Field<bool> selfMute;
    Field<bool, true> selfStream;
    Field<bool> selfVideo;
    Field<bool> suppress;
    Field<std::string, true> requestToSpeakTimestamp;

    static VoiceState fromJson(const nlohmann::json &obj)
    {
        VoiceState state;
        get(obj, "guild_id", state.guildId);
        get(obj, "channel_id", state.channelId);
        get(obj, "user_id", state.userId);
        get(obj, "session_id", state.sessionId);
        get(obj, "deaf", state.deaf);
        get(obj, "mute", state.mute);
        get(obj, "self_deaf", state.selfDeaf);
        get(obj, "self_mute", state.selfMute);
        get(obj, "self_stream", state.selfStream);
        get(obj, "self_video", state.selfVideo);
        get(obj, "suppress", state.suppress);
        get(obj, "request_to_speak_timestamp", state.requestToSpeakTimestamp);
        return state;
    }
};

struct GatewayGuild : Core::JsonUtils::JsonObject
{
    Field<Guild> properties;
    Field<std::vector<Channel>> channels;
    // active/joined
    Field<std::vector<Channel>, true> threads;
    Field<std::vector<Role>, true> roles;
    Field<std::vector<Emoji>, true> emojis;
    Field<std::vector<Member>, true> members;
    // only sometimes
    Field<std::vector<Presence>, true> presences;
    Field<Platform::DateTime, true> joinedAt;
    Field<bool, true> unavailable;

    static GatewayGuild fromJson(const nlohmann::json &obj)
    {
        GatewayGuild guild;
        get(obj, "properties", guild.properties);
        get(obj, "channels", guild.channels);
        get(obj, "threads", guild.threads);
        get(obj, "roles", guild.roles);
        get(obj, "emojis", guild.emojis);
        get(obj, "members", guild.members);
        get(obj, "presences", guild.presences);
        get(obj, "joined_at", guild.joinedAt);
        get(obj, "unavailable", guild.unavailable);
        if (obj.contains("premium_subscription_count"))
            get(obj, "premium_subscription_count", guild.properties->premiumSubscriptionCount);
        return guild;
    }

    Guild asGuild() const
    {
        Guild guild = properties;
        guild.roles = roles;
        return guild;
    }
};

struct EmbedFooter : Core::JsonUtils::JsonObject
{
    Field<std::string> text;
    Field<std::string, true> iconUrl;
    Field<std::string, true> proxyIconUrl;

    static EmbedFooter fromJson(const nlohmann::json &obj)
    {
        EmbedFooter footer;
        get(obj, "text", footer.text);
        get(obj, "icon_url", footer.iconUrl);
        get(obj, "proxy_icon_url", footer.proxyIconUrl);
        return footer;
    }
};

struct EmbedMedia : Core::JsonUtils::JsonObject
{
    static constexpr int IsAnimatedFlag = 1 << 5;

    Field<std::string> url;
    Field<std::string, true> proxyUrl;
    Field<int, true> width;
    Field<int, true> height;
    Field<std::string, true> contentType;
    Field<int, true> flags;

    static EmbedMedia fromJson(const nlohmann::json &obj)
    {
        EmbedMedia image;
        get(obj, "url", image.url);
        get(obj, "proxy_url", image.proxyUrl);
        get(obj, "width", image.width);
        get(obj, "height", image.height);
        get(obj, "content_type", image.contentType);
        get(obj, "flags", image.flags);
        return image;
    }

    [[nodiscard]] bool isFlaggedAnimated() const { return flags.hasValue() && (*flags & IsAnimatedFlag); }
    [[nodiscard]] bool hasSize() const { return width.hasValue() && height.hasValue() && *width > 0 && *height > 0; }
};

struct EmbedProvider : Core::JsonUtils::JsonObject
{
    Field<std::string, true> name;
    Field<std::string, true> url;

    static EmbedProvider fromJson(const nlohmann::json &obj)
    {
        EmbedProvider provider;
        get(obj, "name", provider.name);
        get(obj, "url", provider.url);
        return provider;
    }
};

struct EmbedAuthor : Core::JsonUtils::JsonObject
{
    Field<std::string> name;
    Field<std::string, true> url;
    Field<std::string, true> iconUrl;
    Field<std::string, true> proxyIconUrl;

    static EmbedAuthor fromJson(const nlohmann::json &obj)
    {
        EmbedAuthor author;
        get(obj, "name", author.name);
        get(obj, "url", author.url);
        get(obj, "icon_url", author.iconUrl);
        get(obj, "proxy_icon_url", author.proxyIconUrl);
        return author;
    }
};

struct EmbedField : Core::JsonUtils::JsonObject
{
    Field<std::string> name;
    Field<std::string> value;
    Field<bool, true> isInline;

    static EmbedField fromJson(const nlohmann::json &obj)
    {
        EmbedField field;
        get(obj, "name", field.name);
        get(obj, "value", field.value);
        get(obj, "inline", field.isInline);
        return field;
    }
};

struct Embed : Core::JsonUtils::JsonObject
{
    Field<std::string, true> title;
    Field<std::string, true> type;
    Field<std::string, true> description;
    Field<std::string, true> url;
    Field<Platform::DateTime, true> timestamp;
    Field<int, true> color;
    Field<EmbedFooter, true> footer;
    Field<EmbedMedia, true> image;
    Field<EmbedMedia, true> thumbnail;
    Field<EmbedMedia, true> video;
    Field<EmbedProvider, true> provider;
    Field<EmbedAuthor, true> author;
    Field<std::vector<EmbedField>, true> fields;

    static Embed fromJson(const nlohmann::json &obj)
    {
        Embed embed;
        get(obj, "title", embed.title);
        get(obj, "type", embed.type);
        get(obj, "description", embed.description);
        get(obj, "url", embed.url);
        get(obj, "timestamp", embed.timestamp);
        get(obj, "color", embed.color);
        get(obj, "footer", embed.footer);
        get(obj, "image", embed.image);
        get(obj, "thumbnail", embed.thumbnail);
        get(obj, "video", embed.video);
        get(obj, "provider", embed.provider);
        get(obj, "author", embed.author);
        get(obj, "fields", embed.fields);
        return embed;
    }

    [[nodiscard]] bool isBareMedia() const
    {
        const std::string kind = type.hasValue() ? *type : std::string();
        const bool isGifv = kind == "gifv";
        const bool isImage = kind == "image";
        if (!isGifv && !isImage)
            return false;

        const bool sizedThumbnail = thumbnail.hasValue() && thumbnail->hasSize();
        const bool sizedVideo = video.hasValue() && video->hasSize();
        const bool videoIsProxied = sizedVideo && video->proxyUrl.hasValue();
        const bool showsImage = (image.hasValue() && image->hasSize()) || (isImage && sizedThumbnail);
        const bool videoHasPoster = (isGifv && sizedThumbnail) || videoIsProxied;
        const bool showsVideo = sizedVideo && videoHasPoster && (videoIsProxied || video->url->starts_with("https:"));
        if (!showsImage && !showsVideo)
            return false;

        return isGifv || (!author.hasValue() && !title.hasValue());
    }
};

struct Attachment : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<std::string> filename;
    Field<std::string, true> contentType;
    Field<std::int64_t> size;
    Field<std::string> url;
    Field<std::string> proxyUrl;
    Field<int, true> width;
    Field<int, true> height;
    Field<double, true> durationSecs;
    Field<AttachmentFlags, true> flags;

    // local preview of pasted bitmaps
    Platform::ImageBuffer localPreview;

    static Attachment fromJson(const nlohmann::json &obj)
    {
        Attachment att;
        get(obj, "id", att.id);
        get(obj, "filename", att.filename);
        get(obj, "content_type", att.contentType);
        get(obj, "size", att.size);
        get(obj, "url", att.url);
        get(obj, "proxy_url", att.proxyUrl);
        get(obj, "width", att.width);
        get(obj, "height", att.height);
        get(obj, "duration_secs", att.durationSecs);
        get(obj, "flags", att.flags);
        return att;
    }

    bool isImage() const
    {
        if (!contentType.hasValue())
            return false;
        return contentType->starts_with("image/");
    }

    bool isSpoiler() const
    {
        return flags.hasValue() && flags->testFlag(AttachmentFlag::IS_SPOILER);
    }

    [[nodiscard]] bool isFlaggedAnimated() const
    {
        return flags.hasValue() && flags->testFlag(AttachmentFlag::IS_ANIMATED);
    }
};

struct ReactionCountDetails : Core::JsonUtils::JsonObject
{
    Field<int> burst;
    Field<int> normal;

    static ReactionCountDetails fromJson(const nlohmann::json &obj)
    {
        ReactionCountDetails details;
        get(obj, "burst", details.burst);
        get(obj, "normal", details.normal);
        return details;
    }
};

struct StickerItem : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<std::string> name;
    Field<StickerFormatType> formatType;

    static StickerItem fromJson(const nlohmann::json &obj)
    {
        StickerItem sticker;
        get(obj, "id", sticker.id);
        get(obj, "name", sticker.name);
        get(obj, "format_type", sticker.formatType);
        return sticker;
    }
};

struct Reaction : Core::JsonUtils::JsonObject
{
    Field<Emoji> emoji;
    Field<int> count;
    Field<ReactionCountDetails, true> countDetails;
    Field<bool> me;
    Field<bool, true> meBurst;
    Field<int, true> burstCount;
    Field<std::vector<std::string>, true> burstColors;

    static Reaction fromJson(const nlohmann::json &obj)
    {
        Reaction reaction;
        get(obj, "emoji", reaction.emoji);
        get(obj, "count", reaction.count);
        get(obj, "count_details", reaction.countDetails);
        get(obj, "me", reaction.me);
        get(obj, "me_burst", reaction.meBurst);
        get(obj, "burst_count", reaction.burstCount);
        get(obj, "burst_colors", reaction.burstColors);
        return reaction;
    }

    [[nodiscard]] int normalCount() const { return countDetails.hasValue() ? countDetails->normal.get() : count.get(); }
    [[nodiscard]] int superCount() const { return countDetails.hasValue() ? countDetails->burst.get() : 0; }

    Platform::Color getBrightestBurstColor() const
    {
        if (!burstColors.hasValue() || burstColors->empty())
            return {};

        Platform::Color best;
        float maxSaturation = -1;

        for (const std::string &hex : *burstColors) {
            Platform::Color color = Platform::Color::parse(hex);
            if (!color.valid)
                continue;
            const auto [minChannel, maxChannel] = std::minmax({color.r, color.g, color.b});
            const float saturation = maxChannel == 0 ? 0.0f : float(maxChannel - minChannel) / maxChannel;
            if (saturation > maxSaturation) {
                maxSaturation = saturation;
                best = color;
            }
        }

        return best;
    }
};

struct MessageReference : Core::JsonUtils::JsonObject
{
    Field<MessageReferenceType, true> type;
    Field<Core::Snowflake, true> messageId;
    Field<Core::Snowflake, true> channelId;
    Field<Core::Snowflake, true> guildId;

    static MessageReference fromJson(const nlohmann::json &obj)
    {
        MessageReference ref;
        get(obj, "type", ref.type);
        get(obj, "message_id", ref.messageId);
        get(obj, "channel_id", ref.channelId);
        get(obj, "guild_id", ref.guildId);
        return ref;
    }
};

struct ComponentSelectOption : Core::JsonUtils::JsonObject
{
    Field<std::string> label;
    Field<std::string> value;
    Field<std::string, true> description;
    Field<Emoji, true> emoji;
    Field<bool, true> isDefault;

    static ComponentSelectOption fromJson(const nlohmann::json &obj)
    {
        ComponentSelectOption option;
        get(obj, "label", option.label);
        get(obj, "value", option.value);
        get(obj, "description", option.description);
        get(obj, "emoji", option.emoji);
        get(obj, "default", option.isDefault);
        return option;
    }
};

struct ComponentSelectDefaultValue : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<std::string> type;

    static ComponentSelectDefaultValue fromJson(const nlohmann::json &obj)
    {
        ComponentSelectDefaultValue defaultValue;
        get(obj, "id", defaultValue.id);
        get(obj, "type", defaultValue.type);
        return defaultValue;
    }
};

struct ComponentMedia : Core::JsonUtils::JsonObject
{
    Field<std::string> url;
    Field<Core::Snowflake, true> id;
    Field<Core::Snowflake, true> attachmentId;
    Field<std::string, true> proxyUrl;
    Field<std::string, true> contentType;
    Field<std::string, true> placeholder;
    Field<int, true, true> width;
    Field<int, true, true> height;
    Field<int, true> flags;
    Field<int, true> loadingState;
    Field<int, true> placeholderVersion;

    static ComponentMedia fromJson(const nlohmann::json &obj)
    {
        ComponentMedia media;
        get(obj, "url", media.url);
        get(obj, "id", media.id);
        get(obj, "attachment_id", media.attachmentId);
        get(obj, "proxy_url", media.proxyUrl);
        get(obj, "content_type", media.contentType);
        get(obj, "placeholder", media.placeholder);
        get(obj, "width", media.width);
        get(obj, "height", media.height);
        get(obj, "flags", media.flags);
        get(obj, "loading_state", media.loadingState);
        get(obj, "placeholder_version", media.placeholderVersion);
        return media;
    }
};

struct ComponentGalleryItem : Core::JsonUtils::JsonObject
{
    Field<ComponentMedia> media;
    Field<std::string, true, true> description;
    Field<bool, true> spoiler;

    static ComponentGalleryItem fromJson(const nlohmann::json &obj)
    {
        ComponentGalleryItem item;
        get(obj, "media", item.media);
        get(obj, "description", item.description);
        get(obj, "spoiler", item.spoiler);
        return item;
    }
};

struct MessageComponent : Core::JsonUtils::JsonObject
{
    Field<MessageComponentType> type;
    Field<uint32_t, true> id;
    Field<std::vector<MessageComponent>, true> components;
    std::shared_ptr<const MessageComponent> accessory;
    Field<int, true> style;
    Field<std::string, true> label;
    Field<std::string, true> customId;
    Field<std::string, true> url;
    Field<Core::Snowflake, true> skuId;
    Field<Emoji, true> emoji;
    Field<bool, true> disabled;
    Field<std::vector<ComponentSelectOption>, true> options;
    Field<std::string, true> placeholder;
    Field<int, true> minValues;
    Field<int, true> maxValues;
    Field<std::vector<ComponentSelectDefaultValue>, true> defaultValues;
    Field<std::vector<ChannelType>, true> channelTypes;
    Field<std::string, true> content;
    Field<ComponentMedia, true> media;
    Field<ComponentMedia, true> file;
    Field<std::string, true, true> description;
    Field<bool, true> spoiler;
    Field<std::vector<ComponentGalleryItem>, true> items;
    Field<std::string, true> name;
    Field<int64_t, true> size;
    Field<bool, true> divider;
    Field<int, true> spacing;
    Field<uint32_t, true, true> accentColor;

    static MessageComponent fromJson(const nlohmann::json &obj)
    {
        MessageComponent component;
        get(obj, "type", component.type);
        get(obj, "id", component.id);

        switch (component.type.get()) {
        case MessageComponentType::ACTION_ROW:
            get(obj, "components", component.components);
            break;
        case MessageComponentType::BUTTON:
            get(obj, "style", component.style);
            get(obj, "label", component.label);
            get(obj, "custom_id", component.customId);
            get(obj, "url", component.url);
            get(obj, "sku_id", component.skuId);
            get(obj, "emoji", component.emoji);
            get(obj, "disabled", component.disabled);
            break;
        case MessageComponentType::STRING_SELECT:
        case MessageComponentType::USER_SELECT:
        case MessageComponentType::ROLE_SELECT:
        case MessageComponentType::MENTIONABLE_SELECT:
        case MessageComponentType::CHANNEL_SELECT: {
            get(obj, "custom_id", component.customId);
            get(obj, "placeholder", component.placeholder);
            get(obj, "min_values", component.minValues);
            get(obj, "max_values", component.maxValues);
            get(obj, "disabled", component.disabled);
            if (component.type == MessageComponentType::STRING_SELECT) {
                get(obj, "options", component.options);
            } else {
                get(obj, "default_values", component.defaultValues);
                if (component.type == MessageComponentType::CHANNEL_SELECT)
                    get(obj, "channel_types", component.channelTypes);
            }
            break;
        }
        case MessageComponentType::SECTION: {
            get(obj, "components", component.components);
            const auto it = obj.find("accessory");
            if (it != obj.end() && it->is_object())
                component.accessory = std::make_shared<MessageComponent>(fromJson(*it));
            break;
        }
        case MessageComponentType::TEXT_DISPLAY:
            get(obj, "content", component.content);
            break;
        case MessageComponentType::THUMBNAIL:
            get(obj, "media", component.media);
            get(obj, "description", component.description);
            get(obj, "spoiler", component.spoiler);
            break;
        case MessageComponentType::MEDIA_GALLERY:
            get(obj, "items", component.items);
            break;
        case MessageComponentType::FILE:
            get(obj, "file", component.file);
            get(obj, "spoiler", component.spoiler);
            get(obj, "name", component.name);
            get(obj, "size", component.size);
            break;
        case MessageComponentType::SEPARATOR:
            get(obj, "divider", component.divider);
            get(obj, "spacing", component.spacing);
            break;
        case MessageComponentType::CONTAINER:
            get(obj, "components", component.components);
            get(obj, "accent_color", component.accentColor);
            get(obj, "spoiler", component.spoiler);
            break;
        default:
            break;
        }

        return component;
    }
};

// The question or one answer of a poll. Questions only carry text; answers may add an emoji.
struct PollMedia : Core::JsonUtils::JsonObject
{
    Field<std::string, true> text;
    Field<Emoji, true> emoji;

    static PollMedia fromJson(const nlohmann::json &obj)
    {
        PollMedia media;
        get(obj, "text", media.text);
        get(obj, "emoji", media.emoji);
        return media;
    }
};

struct PollAnswer : Core::JsonUtils::JsonObject
{
    Field<int> answerId;
    Field<PollMedia> pollMedia;

    static PollAnswer fromJson(const nlohmann::json &obj)
    {
        PollAnswer answer;
        get(obj, "answer_id", answer.answerId);
        get(obj, "poll_media", answer.pollMedia);
        return answer;
    }
};

struct PollAnswerCount : Core::JsonUtils::JsonObject
{
    Field<int> id;
    Field<int> count;
    Field<bool> meVoted;

    static PollAnswerCount fromJson(const nlohmann::json &obj)
    {
        PollAnswerCount answerCount;
        get(obj, "id", answerCount.id);
        get(obj, "count", answerCount.count);
        get(obj, "me_voted", answerCount.meVoted);
        return answerCount;
    }
};

struct PollResults : Core::JsonUtils::JsonObject
{
    // False while the poll runs and until Discord's final exact count after it ends.
    Field<bool> isFinalized;
    // Answers missing here have no votes.
    Field<std::vector<PollAnswerCount>> answerCounts;

    static PollResults fromJson(const nlohmann::json &obj)
    {
        PollResults results;
        get(obj, "is_finalized", results.isFinalized);
        get(obj, "answer_counts", results.answerCounts);
        return results;
    }
};

struct Poll : Core::JsonUtils::JsonObject
{
    Field<PollMedia> question;
    Field<std::vector<PollAnswer>> answers;
    // Null would mean a poll that never ends; Discord doesn't make those yet.
    Field<Platform::DateTime, false, true> expiry;
    Field<bool> allowMultiselect;
    Field<int, true> layoutType;
    // Missing means Discord didn't load the counts for this response ("unknown"), not
    // "no votes"; Message::applyUpdate keeps the counts it already had.
    Field<PollResults, true> results;

    static Poll fromJson(const nlohmann::json &obj)
    {
        Poll poll;
        get(obj, "question", poll.question);
        get(obj, "answers", poll.answers);
        get(obj, "expiry", poll.expiry);
        get(obj, "allow_multiselect", poll.allowMultiselect);
        get(obj, "layout_type", poll.layoutType);
        get(obj, "results", poll.results);
        return poll;
    }
};

struct Message : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<std::string, true> nonce;
    Field<Core::Snowflake> channelId;
    Field<User> author;
    Field<std::string> content;
    Field<Platform::DateTime> timestamp;
    Field<Platform::DateTime, false, true> editedTimestamp;
    Field<MessageType> type;
    Field<MessageFlags> flags;
    Field<std::vector<Attachment>, true> attachments;
    Field<std::vector<Embed>, true> embeds;
    Field<Member, true> member;
    Field<std::vector<MentionedUser>, true> mentions;
    Field<std::vector<Core::Snowflake>, true> mentionRoles;
    Field<bool, true> mentionEveryone;
    Field<std::vector<Reaction>, true> reactions;
    Field<std::vector<StickerItem>, true> stickerItems;
    Field<std::vector<MessageComponent>, true> components;
    Field<Poll, true> poll;
    // Set on interaction replies and app-owned webhooks; a bot's own messages leave it out
    // and its user ID doubles as the application ID.
    Field<Core::Snowflake, true> applicationId;

    Field<MessageReference, true> messageReference;

    // tri-state for referenced_message:
    //   nullptr + referencedMessageNull=false  -> backend didn't fetch (unknown)
    //   nullptr + referencedMessageNull=true   -> referenced message was deleted
    //   non-null                               -> referenced message is present
    std::shared_ptr<Message> referencedMessage;
    bool referencedMessageNull = false;

    std::shared_ptr<Message> snapshotMessage;

    // TRANSIENT for MESSAGE_UPDATE
    Field<Core::Snowflake, true> guildId;
    Field<ChannelType, true> channelType;

    // cached data
    std::string parsedContentCached;
    bool contentIsSingleLink = false;
    std::string embedsJson;
    std::string reactionsJson;
    std::string stickersJson;
    std::string snapshotJson;
    std::string componentsJson;
    // The poll as received (compact JSON), for the message cache; empty without a poll.
    std::string pollJson;

    // sent
    bool isPendingOutbound = false;

    // track for MESSAGE_UPDATE
    std::unordered_set<std::string> presentKeys;

    [[nodiscard]] bool isEphemeral() const
    {
        return flags.get().testFlag(MessageFlag::EPHEMERAL);
    }

    [[nodiscard]] bool isForwarded() const
    {
        return messageReference.hasValue() &&
               messageReference->type.hasValue() &&
               messageReference->type.get() == MessageReferenceType::FORWARD;
    }

    [[nodiscard]] const Message &contentMessage() const
    {
        return isForwarded() && snapshotMessage ? *snapshotMessage : *this;
    }

    [[nodiscard]] bool hidesLinkBehindItsEmbed() const
    {
        return contentIsSingleLink && embeds.hasValue() && embeds->size() == 1 && embeds->front().isBareMedia();
    }

    void setSnapshot(const nlohmann::json &snapshotObj)
    {
        snapshotMessage = std::make_shared<Message>(fromJson(snapshotObj));
        snapshotJson = snapshotObj.dump();
    }

    static std::string compactArrayJson(const nlohmann::json &obj, const std::string &key)
    {
        const auto found = obj.find(key);
        if (found == obj.end())
            return {};
        return found->is_array() ? found->dump() : "[]";
    }

    static std::string compactObjectJson(const nlohmann::json &obj, const std::string &key)
    {
        const auto found = obj.find(key);
        if (found == obj.end() || !found->is_object())
            return {};
        return found->dump();
    }

    static Message fromJson(const nlohmann::json &obj)
    {
        Message message;

        if (obj.is_object())
            for (auto it = obj.begin(); it != obj.end(); ++it)
                message.presentKeys.insert(it.key());

        get(obj, "id", message.id);
        if (const auto nonce = obj.find("nonce"); nonce != obj.end() && nonce->is_number_integer())
            message.nonce = nonce->dump();
        else
            get(obj, "nonce", message.nonce);
        get(obj, "channel_id", message.channelId);
        get(obj, "author", message.author);
        get(obj, "content", message.content);
        get(obj, "application_id", message.applicationId);
        get(obj, "timestamp", message.timestamp);
        get(obj, "edited_timestamp", message.editedTimestamp);
        get(obj, "type", message.type);
        get(obj, "flags", message.flags);
        get(obj, "attachments", message.attachments);
        get(obj, "embeds", message.embeds);
        get(obj, "member", message.member);
        get(obj, "mentions", message.mentions);
        get(obj, "mention_roles", message.mentionRoles);
        get(obj, "mention_everyone", message.mentionEveryone);
        get(obj, "reactions", message.reactions);
        get(obj, "sticker_items", message.stickerItems);
        get(obj, "components", message.components);
        get(obj, "poll", message.poll);
        get(obj, "message_reference", message.messageReference);
        get(obj, "guild_id", message.guildId);
        get(obj, "channel_type", message.channelType);

        const auto snapshots = obj.find("message_snapshots");
        if (snapshots != obj.end() && snapshots->is_array() && !snapshots->empty() && snapshots->front().is_object()) {
            const auto snapshot = snapshots->front().find("message");
            if (snapshot != snapshots->front().end() && snapshot->is_object())
                message.setSnapshot(*snapshot);
        }

        // referenced_message: manually handle tri-state (absent / null / object)
        auto refIt = obj.find("referenced_message");
        if (refIt != obj.end()) {
            if (refIt->is_null()) {
                message.referencedMessageNull = true;
            } else if (refIt->is_object()) {
                message.referencedMessage =
                        std::make_shared<Message>(fromJson(refIt.value()));
            }
        }

        message.embedsJson = compactArrayJson(obj, "embeds");
        message.reactionsJson = compactArrayJson(obj, "reactions");
        message.stickersJson = compactArrayJson(obj, "sticker_items");
        message.componentsJson = compactArrayJson(obj, "components");
        message.pollJson = compactObjectJson(obj, "poll");

        return message;
    }

    void applyUpdate(const Message &update)
    {
        const std::unordered_set<std::string> &present = update.presentKeys;

        if (present.contains("content"))
            content = update.content;
        if (present.contains("edited_timestamp"))
            editedTimestamp = update.editedTimestamp;
        if (present.contains("author"))
            author = update.author;
        if (present.contains("timestamp"))
            timestamp = update.timestamp;
        if (present.contains("type"))
            type = update.type;
        if (present.contains("flags"))
            flags = update.flags;
        if (present.contains("mentions"))
            mentions = update.mentions;
        if (present.contains("mention_roles"))
            mentionRoles = update.mentionRoles;
        if (present.contains("mention_everyone"))
            mentionEveryone = update.mentionEveryone;
        if (present.contains("attachments"))
            attachments = update.attachments;
        if (present.contains("message_reference"))
            messageReference = update.messageReference;
        if (present.contains("referenced_message")) {
            referencedMessage = update.referencedMessage;
            referencedMessageNull = update.referencedMessageNull;
        }

        if (present.contains("message_snapshots")) {
            snapshotMessage = update.snapshotMessage;
            snapshotJson = update.snapshotJson;
        }

        if (present.contains("embeds")) {
            embeds = update.embeds;
            embedsJson = update.embedsJson;
        }

        if (present.contains("components")) {
            components = update.components;
            componentsJson = update.componentsJson;
        }

        if (present.contains("reactions")) {
            reactions = update.reactions;
            reactionsJson = update.reactionsJson;
        }

        if (present.contains("sticker_items")) {
            stickerItems = update.stickerItems;
            stickersJson = update.stickersJson;
        }

        if (present.contains("poll")) {
            const bool keepResults = poll.hasValue() && poll->results.hasValue()
                && update.poll.hasValue() && !update.poll->results.hasValue();
            const auto prevResults = poll.hasValue() ? poll->results : decltype(poll->results){};
            const std::string prevPollJson = pollJson;

            poll = update.poll;
            pollJson = update.pollJson;

            if (keepResults) {
                // Discord omits results when they are not loaded ("unknown"), not "no votes".
                // Retain previously known results when the incoming update does not carry them.
                poll->results = prevResults;
                if (!prevPollJson.empty() && !pollJson.empty()) {
                    auto prev = nlohmann::json::parse(prevPollJson, nullptr, false);
                    auto next = nlohmann::json::parse(pollJson, nullptr, false);
                    if (!prev.is_discarded() && !next.is_discarded() && prev.contains("results")) {
                        next["results"] = prev["results"];
                        pollJson = next.dump();
                    }
                }
            }
        }
    }
};

struct NotificationSettings : Core::JsonUtils::JsonObject
{
    Field<int, true> flags;

    static NotificationSettings fromJson(const nlohmann::json &obj)
    {
        NotificationSettings settings;
        get(obj, "flags", settings.flags);
        return settings;
    }
};

struct MuteConfig : Core::JsonUtils::JsonObject
{
    Field<std::string, true, true> endTime;
    Field<int, true> selectedTimeWindow;

    static MuteConfig fromJson(const nlohmann::json &obj)
    {
        MuteConfig config;
        get(obj, "end_time", config.endTime);
        get(obj, "selected_time_window", config.selectedTimeWindow);
        return config;
    }
};

struct ChannelOverride : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> channelId;
    Field<bool, true> collapsed;
    Field<int, true> flags;
    Field<MessageNotificationLevel, true> messageNotifications;
    Field<bool, true> muted;
    Field<MuteConfig, true, true> muteConfig;

    static ChannelOverride fromJson(const nlohmann::json &obj)
    {
        ChannelOverride co;
        get(obj, "channel_id", co.channelId);
        get(obj, "collapsed", co.collapsed);
        get(obj, "flags", co.flags);
        get(obj, "message_notifications", co.messageNotifications);
        get(obj, "muted", co.muted);
        get(obj, "mute_config", co.muteConfig);
        return co;
    }
};

struct UserGuildSettings : Core::JsonUtils::JsonObject
{
    Field<std::vector<ChannelOverride>, true> channelOverrides;
    Field<int, true> flags;
    Field<Core::Snowflake, false, true> guildId;
    Field<bool, true> hideMutedChannels;
    Field<MessageNotificationLevel, true> messageNotifications;
    Field<bool, true> mobilePush;
    Field<bool, true> muteScheduledEvents;
    Field<bool, true> muted;
    Field<MuteConfig, true, true> muteConfig;
    Field<int, true> notifyHighlights;
    Field<bool, true> suppressEveryone;
    Field<bool, true> suppressRoles;
    Field<int, true> version;

    static UserGuildSettings fromJson(const nlohmann::json &obj)
    {
        UserGuildSettings s;
        get(obj, "channel_overrides", s.channelOverrides);
        get(obj, "flags", s.flags);
        get(obj, "guild_id", s.guildId);
        get(obj, "hide_muted_channels", s.hideMutedChannels);
        get(obj, "message_notifications", s.messageNotifications);
        get(obj, "mobile_push", s.mobilePush);
        get(obj, "mute_scheduled_events", s.muteScheduledEvents);
        get(obj, "muted", s.muted);
        get(obj, "mute_config", s.muteConfig);
        get(obj, "notify_highlights", s.notifyHighlights);
        get(obj, "suppress_everyone", s.suppressEveryone);
        get(obj, "suppress_roles", s.suppressRoles);
        get(obj, "version", s.version);
        return s;
    }
};

struct GuildFolderEntry : Core::JsonUtils::JsonObject
{
    Field<std::vector<Core::Snowflake>> guildIds;
    Field<std::int64_t, false, true> id;
    Field<std::string, false, true> name;
    Field<std::int64_t, false, true> color;

    static GuildFolderEntry fromJson(const nlohmann::json &obj)
    {
        GuildFolderEntry entry;
        get(obj, "guild_ids", entry.guildIds);
        get(obj, "id", entry.id);
        get(obj, "name", entry.name);
        get(obj, "color", entry.color);
        return entry;
    }
};

struct UserSettings : Core::JsonUtils::JsonObject
{
    Field<std::vector<GuildFolderEntry>, true> guildFolders;

    static UserSettings fromJson(const nlohmann::json &obj)
    {
        UserSettings settings;
        get(obj, "guild_folders", settings.guildFolders);
        return settings;
    }
};

struct Relationship : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<RelationshipType> type;
    Field<User> user;
    Field<std::string, false, true> nickname;
    Field<bool> userIgnored;
    Field<bool, true> isSpamRequest;
    Field<bool, true> strangerRequest;
    Field<Platform::DateTime, true> since;

    static Relationship fromJson(const nlohmann::json &obj)
    {
        Relationship r;
        get(obj, "id", r.id);
        get(obj, "type", r.type);
        get(obj, "user", r.user);
        get(obj, "nickname", r.nickname);
        get(obj, "user_ignored", r.userIgnored);
        get(obj, "is_spam_request", r.isSpamRequest);
        get(obj, "stranger_request", r.strangerRequest);
        get(obj, "since", r.since);
        return r;
    }
};

struct ReadStateEntry : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<int, true> readStateType;
    Field<Core::Snowflake, true, true> lastMessageId;
    Field<int, true> mentionCount;
    Field<int, true> flags;
    Field<int, true, true> lastViewed;
    Field<std::string, true, true> lastPinTimestamp;

    static ReadStateEntry fromJson(const nlohmann::json &obj)
    {
        ReadStateEntry entry;
        get(obj, "id", entry.id);
        get(obj, "read_state_type", entry.readStateType);
        get(obj, "last_message_id", entry.lastMessageId);
        get(obj, "mention_count", entry.mentionCount);
        get(obj, "flags", entry.flags);
        get(obj, "last_viewed", entry.lastViewed);
        get(obj, "last_pin_timestamp", entry.lastPinTimestamp);
        return entry;
    }
};

struct ConnectedAccount : Core::JsonUtils::JsonObject
{
    Field<std::string> id;
    Field<std::string> type;
    Field<std::string> name;
    Field<bool> verified;

    static ConnectedAccount fromJson(const nlohmann::json &obj)
    {
        ConnectedAccount c;
        get(obj, "id", c.id);
        get(obj, "type", c.type);
        get(obj, "name", c.name);
        get(obj, "verified", c.verified);
        return c;
    }
};

struct MutualGuildEntry : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<std::string, false, true> nick;

    static MutualGuildEntry fromJson(const nlohmann::json &obj)
    {
        MutualGuildEntry m;
        get(obj, "id", m.id);
        get(obj, "nick", m.nick);
        return m;
    }
};

struct UserProfileBadge : Core::JsonUtils::JsonObject
{
    Field<std::string> id;
    Field<std::string> description;
    Field<std::string> icon;
    Field<std::string, true> link;

    static UserProfileBadge fromJson(const nlohmann::json &obj)
    {
        UserProfileBadge b;
        get(obj, "id", b.id);
        get(obj, "description", b.description);
        get(obj, "icon", b.icon);
        get(obj, "link", b.link);
        return b;
    }
};

struct UserProfileData : Core::JsonUtils::JsonObject
{
    Field<std::string, true> bio;
    Field<int, true, true> accentColor;
    Field<std::string, true, true> banner;
    Field<std::string> pronouns;

    static UserProfileData fromJson(const nlohmann::json &obj)
    {
        UserProfileData p;
        get(obj, "bio", p.bio);
        get(obj, "accent_color", p.accentColor);
        get(obj, "banner", p.banner);
        get(obj, "pronouns", p.pronouns);
        return p;
    }
};

struct GuildMemberProfile : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake, true> guildId;
    Field<std::string, true> bio;
    Field<int, true, true> accentColor;
    Field<std::string, true, true> banner;
    Field<std::string> pronouns;

    static GuildMemberProfile fromJson(const nlohmann::json &obj)
    {
        GuildMemberProfile p;
        get(obj, "guild_id", p.guildId);
        get(obj, "bio", p.bio);
        get(obj, "accent_color", p.accentColor);
        get(obj, "banner", p.banner);
        get(obj, "pronouns", p.pronouns);
        return p;
    }
};

struct UserProfile : Core::JsonUtils::JsonObject
{
    Field<User> user;
    Field<UserProfileData> userProfile;
    Field<PremiumType, false, true> premiumType;
    Field<Platform::DateTime, false, true> premiumSince;
    Field<Platform::DateTime, false, true> premiumGuildSince;
    Field<std::vector<ConnectedAccount>> connectedAccounts;
    Field<std::vector<MutualGuildEntry>, true> mutualGuilds;
    Field<std::vector<User>, true> mutualFriends;
    Field<int, true> mutualFriendsCount;
    Field<Member, true> guildMember;
    Field<GuildMemberProfile, true> guildMemberProfile;
    Field<std::vector<UserProfileBadge>> badges;
    Field<std::string, true, true> legacyUsername;

    static UserProfile fromJson(const nlohmann::json &obj)
    {
        UserProfile p;
        get(obj, "user", p.user);
        get(obj, "user_profile", p.userProfile);
        get(obj, "premium_type", p.premiumType);
        get(obj, "premium_since", p.premiumSince);
        get(obj, "premium_guild_since", p.premiumGuildSince);
        get(obj, "connected_accounts", p.connectedAccounts);
        get(obj, "mutual_guilds", p.mutualGuilds);
        get(obj, "mutual_friends", p.mutualFriends);
        get(obj, "mutual_friends_count", p.mutualFriendsCount);
        get(obj, "guild_member", p.guildMember);
        get(obj, "guild_member_profile", p.guildMemberProfile);
        get(obj, "badges", p.badges);
        get(obj, "legacy_username", p.legacyUsername);
        return p;
    }
};

struct AuditLogChange
{
    std::string key;
    std::optional<nlohmann::json> oldValue;
    std::optional<nlohmann::json> newValue;
};

struct AuditLogEntry : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<AuditLogAction> actionType;
    Field<Core::Snowflake, false, true> userId;
    Field<Core::Snowflake, false, true> targetId;
    Field<std::string, true, true> reason;
    std::vector<AuditLogChange> changes;
    nlohmann::json options;

    static AuditLogEntry fromJson(const nlohmann::json &obj)
    {
        AuditLogEntry entry;
        get(obj, "id", entry.id);
        get(obj, "action_type", entry.actionType);
        get(obj, "user_id", entry.userId);
        get(obj, "target_id", entry.targetId);
        get(obj, "reason", entry.reason);
        if (const auto changes = obj.find("changes"); changes != obj.end() && changes->is_array()) {
            for (const auto &change : *changes) {
                if (!change.is_object())
                    continue;
                AuditLogChange parsed;
                if (const auto key = change.find("key"); key != change.end() && key->is_string())
                    parsed.key = key->get<std::string>();
                if (const auto oldValue = change.find("old_value"); oldValue != change.end())
                    parsed.oldValue = *oldValue;
                if (const auto newValue = change.find("new_value"); newValue != change.end())
                    parsed.newValue = *newValue;
                entry.changes.push_back(std::move(parsed));
            }
        }
        if (const auto options = obj.find("options"); options != obj.end() && options->is_object())
            entry.options = *options;
        else
            entry.options = nlohmann::json::object();
        return entry;
    }
};

struct AuditLog
{
    std::vector<AuditLogEntry> entries;
    std::vector<User> users;
    nlohmann::json integrations;
    nlohmann::json webhooks;
    nlohmann::json guildScheduledEvents;
    nlohmann::json autoModerationRules;
    nlohmann::json threads;
    nlohmann::json applicationCommands;

    static AuditLog fromJson(const nlohmann::json &obj)
    {
        AuditLog log;
        if (const auto entries = obj.find("audit_log_entries"); entries != obj.end() && entries->is_array())
            for (const auto &value : *entries)
                if (value.is_object())
                    log.entries.push_back(AuditLogEntry::fromJson(value));
        if (const auto users = obj.find("users"); users != obj.end() && users->is_array())
            for (const auto &value : *users)
                if (value.is_object())
                    log.users.push_back(User::fromJson(value));
        const auto array = [&obj](const char *key) {
            const auto it = obj.find(key);
            return it != obj.end() && it->is_array() ? *it : nlohmann::json::array();
        };
        log.integrations = array("integrations");
        log.webhooks = array("webhooks");
        log.guildScheduledEvents = array("guild_scheduled_events");
        log.autoModerationRules = array("auto_moderation_rules");
        log.threads = array("threads");
        log.applicationCommands = array("application_commands");
        return log;
    }
};

enum class JoinSourceType {
    UNSPECIFIED = 0,
    BOT = 1,
    INTEGRATION = 2,
    DISCOVERY = 3,
    HUB = 4,
    INVITE = 5,
    VANITY_URL = 6,
    MANUAL_MEMBER_VERIFICATION = 7,
    LINKED_CHANNEL = 8,
};

struct MemberSearchResult : Core::JsonUtils::JsonObject
{
    Field<Member> member;
    Field<std::string, true, true> sourceInviteCode;
    Field<JoinSourceType, true, true> joinSourceType;
    Field<Core::Snowflake, true, true> inviterId;

    static MemberSearchResult fromJson(const nlohmann::json &obj)
    {
        MemberSearchResult result;
        get(obj, "member", result.member);
        get(obj, "source_invite_code", result.sourceInviteCode);
        get(obj, "join_source_type", result.joinSourceType);
        get(obj, "inviter_id", result.inviterId);
        return result;
    }

    [[nodiscard]] JoinSourceType resolvedJoinSource() const
    {
        const JoinSourceType type = joinSourceType.valueOr(JoinSourceType::UNSPECIFIED);
        if (type == JoinSourceType::UNSPECIFIED && sourceInviteCode.hasValue() && !sourceInviteCode->empty())
            return JoinSourceType::INVITE;
        return type;
    }
};

} // namespace Discord
} // namespace Styx
