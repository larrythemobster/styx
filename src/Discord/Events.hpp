#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <optional>

#include "Core/JsonUtils.hpp"
#include "Core/Snowflake.hpp"
#include "Entities.hpp"

namespace Styx {
namespace Discord {

struct MergedPresences : Core::JsonUtils::JsonObject
{
    Field<std::vector<Presence>, true> friends;
    Field<std::vector<std::vector<Presence>>, true> guilds;

    static MergedPresences fromJson(const nlohmann::json &obj)
    {
        MergedPresences merged;
        get(obj, "friends", merged.friends);
        get(obj, "guilds", merged.guilds);
        return merged;
    }
};

struct Ready : Core::JsonUtils::JsonObject
{
    Field<User> user;
    Field<std::vector<GatewayGuild>> guilds;
    Field<std::string> userSettingsProto;
    Field<std::vector<std::vector<Member>>, true> mergedMembers;
    Field<MergedPresences, true> mergedPresences;
    Field<std::vector<User>, true> users;
    Field<std::vector<Channel>, true> privateChannels;
    Field<std::vector<ReadStateEntry>, true> readState;
    Field<std::vector<UserGuildSettings>, true> userGuildSettings;
    Field<NotificationSettings, true> notificationSettings;
    Field<UserSettings, true> userSettings;
    Field<std::vector<Relationship>> relationships;
    Field<std::string> sessionId;
    Field<std::string> resumeGatewayUrl;
    Field<std::unordered_map<Core::Snowflake, std::string>> notes;

    static Ready fromJson(const nlohmann::json &obj)
    {
        Ready ready;
        get(obj, "user", ready.user);
        get(obj, "guilds", ready.guilds);
        get(obj, "user_settings_proto", ready.userSettingsProto);
        get(obj, "user_settings", ready.userSettings);
        get(obj, "notification_settings", ready.notificationSettings);
        get(obj, "merged_members", ready.mergedMembers);
        get(obj, "merged_presences", ready.mergedPresences);
        get(obj, "users", ready.users);
        get(obj, "private_channels", ready.privateChannels);
        get(obj, "relationships", ready.relationships);
        get(obj, "session_id", ready.sessionId);
        get(obj, "resume_gateway_url", ready.resumeGatewayUrl);
        get(obj, "notes", ready.notes);

        if (obj.contains("read_state")) {
            nlohmann::json rsObj = obj["read_state"];
            if (rsObj.contains("entries")) {
                nlohmann::json arr = rsObj["entries"];
                std::vector<ReadStateEntry> entries;
                entries.reserve(arr.size());
                for (const nlohmann::json &val : arr)
                    entries.push_back(ReadStateEntry::fromJson(val));
                ready.readState = entries;
            }
        }

        if (obj.contains("user_guild_settings")) {
            nlohmann::json ugsObj = obj["user_guild_settings"];
            if (ugsObj.contains("entries")) {
                nlohmann::json arr = ugsObj["entries"];
                std::vector<UserGuildSettings> entries;
                entries.reserve(arr.size());
                for (const nlohmann::json &val : arr)
                    entries.push_back(UserGuildSettings::fromJson(val));
                ready.userGuildSettings = entries;
            }
        }

        return ready;
    }
};

struct SupplementalGuild : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<std::vector<VoiceState>, true> voiceStates;

    static SupplementalGuild fromJson(const nlohmann::json &obj)
    {
        SupplementalGuild supplementalGuild;
        get(obj, "id", supplementalGuild.id);
        get(obj, "voice_states", supplementalGuild.voiceStates);
        return supplementalGuild;
    }
};

struct ReadySupplemental : Core::JsonUtils::JsonObject
{
    Field<std::vector<SupplementalGuild>> guilds;
    Field<std::vector<std::vector<Member>>> mergedMembers;
    Field<MergedPresences, true> mergedPresences;

    static ReadySupplemental fromJson(const nlohmann::json &obj)
    {
        ReadySupplemental readySupplemental;
        get(obj, "guilds", readySupplemental.guilds);
        get(obj, "merged_members", readySupplemental.mergedMembers);
        get(obj, "merged_presences", readySupplemental.mergedPresences);
        return readySupplemental;
    }
};

struct TypingStart : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> channelId;
    Field<Core::Snowflake, true> guildId;
    Field<Core::Snowflake> userId;
    Field<Platform::DateTime> timestamp;
    Field<Member, true> member;

    static TypingStart fromJson(const nlohmann::json &obj)
    {
        TypingStart typingStart;
        get(obj, "channel_id", typingStart.channelId);
        get(obj, "guild_id", typingStart.guildId);
        get(obj, "user_id", typingStart.userId);
        get(obj, "member", typingStart.member);
        typingStart.timestamp = Platform::DateTime::fromUnixMillis(obj["timestamp"].get<int64_t>() * 1000);
        return typingStart;
    }
};

struct ChannelCreate : Core::JsonUtils::JsonObject
{
    Field<Channel> channel;

    static ChannelCreate fromJson(const nlohmann::json &obj)
    {
        ChannelCreate event;
        event.channel = Channel::fromJson(obj);
        return event;
    }
};

struct ChannelUpdate : Core::JsonUtils::JsonObject
{
    Field<Channel> channel;

    static ChannelUpdate fromJson(const nlohmann::json &obj)
    {
        ChannelUpdate update;
        update.channel = Channel::fromJson(obj);
        return update;
    }
};

struct ChannelDelete : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<Core::Snowflake, true> guildId;

    static ChannelDelete fromJson(const nlohmann::json &obj)
    {
        ChannelDelete event;
        get(obj, "id", event.id);
        get(obj, "guild_id", event.guildId);
        return event;
    }
};

struct ChannelRecipient : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> channelId;
    Field<User> user;
    Field<std::string, true> nick;

    static ChannelRecipient fromJson(const nlohmann::json &obj)
    {
        ChannelRecipient event;
        get(obj, "channel_id", event.channelId);
        get(obj, "user", event.user);
        get(obj, "nick", event.nick);
        return event;
    }
};

struct ThreadDelete : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<Core::Snowflake, true, true> parentId;
    Field<Core::Snowflake, true> guildId;

    static ThreadDelete fromJson(const nlohmann::json &obj)
    {
        ThreadDelete event;
        get(obj, "id", event.id);
        get(obj, "parent_id", event.parentId);
        get(obj, "guild_id", event.guildId);
        return event;
    }
};

struct ForumUnread : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> threadId;
    Field<int, true> count;

    static ForumUnread fromJson(const nlohmann::json &obj)
    {
        ForumUnread unread;
        get(obj, "thread_id", unread.threadId);
        get(obj, "count", unread.count);
        return unread;
    }
};

struct ForumUnreads : Core::JsonUtils::JsonObject
{
    Field<std::vector<ForumUnread>, true> threads;
    Field<bool, true> permissionDenied;

    static ForumUnreads fromJson(const nlohmann::json &obj)
    {
        ForumUnreads event;
        get(obj, "threads", event.threads);
        get(obj, "permission_denied", event.permissionDenied);
        return event;
    }
};

struct ThreadListSync : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> guildId;
    Field<std::vector<Core::Snowflake>, true> channelIds;
    Field<std::vector<Channel>, true> threads;
    Field<std::vector<ThreadMember>, true> members;

    static ThreadListSync fromJson(const nlohmann::json &obj)
    {
        ThreadListSync event;
        get(obj, "guild_id", event.guildId);
        get(obj, "channel_ids", event.channelIds);
        get(obj, "threads", event.threads);
        get(obj, "members", event.members);
        return event;
    }
};

struct ThreadMemberUpdate : Core::JsonUtils::JsonObject
{
    ThreadMember member;
    Field<Core::Snowflake, true> userId;
    Field<Core::Snowflake, true> guildId;

    static ThreadMemberUpdate fromJson(const nlohmann::json &obj)
    {
        ThreadMemberUpdate event;
        event.member = ThreadMember::fromJson(obj);
        get(obj, "user_id", event.userId);
        get(obj, "guild_id", event.guildId);
        return event;
    }
};

struct ThreadMembersUpdate : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id; // thread id
    Field<Core::Snowflake, true> guildId;
    Field<int, true> memberCount;
    Field<std::vector<ThreadMember>, true> addedMembers;
    Field<std::vector<Core::Snowflake>, true> removedMemberIds;

    static ThreadMembersUpdate fromJson(const nlohmann::json &obj)
    {
        ThreadMembersUpdate event;
        get(obj, "id", event.id);
        get(obj, "guild_id", event.guildId);
        get(obj, "member_count", event.memberCount);
        get(obj, "added_members", event.addedMembers);
        get(obj, "removed_member_ids", event.removedMemberIds);
        return event;
    }
};

struct GuildMembersChunk : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> guildId;
    Field<std::vector<Member>> members;
    Field<int> chunkIndex;
    Field<int> chunkCount;
    Field<std::vector<Core::Snowflake>, true> notFound;
    Field<std::vector<Presence>, true> presences;

    static GuildMembersChunk fromJson(const nlohmann::json &obj)
    {
        GuildMembersChunk chunk;
        get(obj, "guild_id", chunk.guildId);
        get(obj, "members", chunk.members);
        get(obj, "presences", chunk.presences);
        get(obj, "chunk_index", chunk.chunkIndex);
        get(obj, "chunk_count", chunk.chunkCount);
        get(obj, "not_found", chunk.notFound);
        return chunk;
    }
};

struct GuildMemberUpdate : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> guildId;
    Field<Member> member;

    static GuildMemberUpdate fromJson(const nlohmann::json &obj)
    {
        GuildMemberUpdate event;
        get(obj, "guild_id", event.guildId);
        event.member = Member::fromJson(obj);
        return event;
    }
};

struct GuildMemberAdd : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> guildId;
    Field<Member> member;

    static GuildMemberAdd fromJson(const nlohmann::json &obj)
    {
        GuildMemberAdd event;
        get(obj, "guild_id", event.guildId);
        event.member = Member::fromJson(obj);
        return event;
    }
};

struct GuildMemberRemove : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> guildId;
    Field<User> user;

    static GuildMemberRemove fromJson(const nlohmann::json &obj)
    {
        GuildMemberRemove event;
        get(obj, "guild_id", event.guildId);
        get(obj, "user", event.user);
        return event;
    }
};

struct GuildBanEvent : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> guildId;
    Field<User> user;

    static GuildBanEvent fromJson(const nlohmann::json &obj)
    {
        GuildBanEvent event;
        get(obj, "guild_id", event.guildId);
        get(obj, "user", event.user);
        return event;
    }
};

struct GuildStickersUpdate : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> guildId;
    Field<std::vector<Sticker>> stickers;

    static GuildStickersUpdate fromJson(const nlohmann::json &obj)
    {
        GuildStickersUpdate event;
        get(obj, "guild_id", event.guildId);
        get(obj, "stickers", event.stickers);
        return event;
    }
};

struct GuildPruneUpdate : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> guildId;
    Field<int, true> days;
    Field<int, true> pruneCount;
    Field<bool, true> isPreview;
    Field<bool, true> isFinished;
    Field<std::vector<Core::Snowflake>, true> includeRoles;

    static GuildPruneUpdate fromJson(const nlohmann::json &obj)
    {
        GuildPruneUpdate event;
        get(obj, "guild_id", event.guildId);
        const nlohmann::json prune = obj.value("prune", nlohmann::json());
        get(prune, "days", event.days);
        get(prune, "prune_count", event.pruneCount);
        get(prune, "is_preview", event.isPreview);
        get(prune, "is_finished", event.isFinished);
        get(prune, "include_roles", event.includeRoles);
        return event;
    }
};

struct MessageDelete : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<Core::Snowflake> channelId;
    Field<Core::Snowflake, true> guildId;

    static MessageDelete fromJson(const nlohmann::json &obj)
    {
        MessageDelete event;
        get(obj, "id", event.id);
        get(obj, "channel_id", event.channelId);
        get(obj, "guild_id", event.guildId);
        return event;
    }
};

struct GuildRoleCreate : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> guildId;
    Field<Role> role;

    static GuildRoleCreate fromJson(const nlohmann::json &obj)
    {
        GuildRoleCreate event;
        get(obj, "guild_id", event.guildId);
        get(obj, "role", event.role);
        return event;
    }
};

struct GuildRoleUpdate : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> guildId;
    Field<Role> role;

    static GuildRoleUpdate fromJson(const nlohmann::json &obj)
    {
        GuildRoleUpdate event;
        get(obj, "guild_id", event.guildId);
        get(obj, "role", event.role);
        return event;
    }
};

struct GuildRoleDelete : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> guildId;
    Field<Core::Snowflake> roleId;

    static GuildRoleDelete fromJson(const nlohmann::json &obj)
    {
        GuildRoleDelete event;
        get(obj, "guild_id", event.guildId);
        get(obj, "role_id", event.roleId);
        return event;
    }
};

struct GuildEmojisUpdate : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> guildId;
    Field<std::vector<Emoji>> emojis;

    static GuildEmojisUpdate fromJson(const nlohmann::json &obj)
    {
        GuildEmojisUpdate event;
        get(obj, "guild_id", event.guildId);
        get(obj, "emojis", event.emojis);
        return event;
    }
};

struct UserSettingsProtoUpdate : Core::JsonUtils::JsonObject
{
    Field<UserSettingsProtoType> type;
    Field<std::string> proto; // base64
    Field<bool, true> partial;

    static UserSettingsProtoUpdate fromJson(const nlohmann::json &obj)
    {
        UserSettingsProtoUpdate event;
        const nlohmann::json settings = obj["settings"];
        get(settings, "type", event.type);
        get(settings, "proto", event.proto);
        get(obj, "partial", event.partial);
        return event;
    }
};

struct GuildDelete : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<bool, true> unavailable;

    [[nodiscard]] bool userRemoved() const { return !(unavailable.hasValue() && unavailable.get()); }

    static GuildDelete fromJson(const nlohmann::json &obj)
    {
        GuildDelete event;
        get(obj, "id", event.id);
        get(obj, "unavailable", event.unavailable);
        return event;
    }
};

struct MessageAck : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> channelId;
    Field<Core::Snowflake> messageId;
    Field<int, true> mentionCount;
    Field<bool, true> manual;
    Field<int, true> flags;
    Field<int, true> version;

    static MessageAck fromJson(const nlohmann::json &obj)
    {
        MessageAck ack;
        get(obj, "channel_id", ack.channelId);
        get(obj, "message_id", ack.messageId);
        get(obj, "mention_count", ack.mentionCount);
        get(obj, "manual", ack.manual);
        get(obj, "flags", ack.flags);
        get(obj, "version", ack.version);
        return ack;
    }
};

struct MessagePollVote : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> userId;
    Field<Core::Snowflake> channelId;
    Field<Core::Snowflake> messageId;
    Field<Core::Snowflake, true> guildId;
    Field<int> answerId;

    static MessagePollVote fromJson(const nlohmann::json &obj)
    {
        MessagePollVote vote;
        get(obj, "user_id", vote.userId);
        get(obj, "channel_id", vote.channelId);
        get(obj, "message_id", vote.messageId);
        get(obj, "guild_id", vote.guildId);
        get(obj, "answer_id", vote.answerId);
        return vote;
    }
};

struct MessageReactionAdd : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> userId;
    Field<Core::Snowflake> channelId;
    Field<Core::Snowflake> messageId;
    Field<Core::Snowflake, true> messageAuthorId;
    Field<Core::Snowflake, true> guildId;
    Field<Emoji> emoji;
    Field<int, true> type; // 0 = normal, 1 = burst
    Field<std::vector<std::string>, true> burstColors;

    static MessageReactionAdd fromJson(const nlohmann::json &obj)
    {
        MessageReactionAdd event;
        get(obj, "user_id", event.userId);
        get(obj, "channel_id", event.channelId);
        get(obj, "message_id", event.messageId);
        get(obj, "message_author_id", event.messageAuthorId);
        get(obj, "guild_id", event.guildId);
        get(obj, "emoji", event.emoji);
        get(obj, "type", event.type);
        get(obj, "burst_colors", event.burstColors);
        return event;
    }
};

struct DebouncedReaction : Core::JsonUtils::JsonObject
{
    Field<std::vector<Core::Snowflake>> users;
    Field<Emoji> emoji;

    static DebouncedReaction fromJson(const nlohmann::json &obj)
    {
        DebouncedReaction reaction;
        get(obj, "users", reaction.users);
        get(obj, "emoji", reaction.emoji);
        return reaction;
    }
};

struct MessageReactionAddMany : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> channelId;
    Field<Core::Snowflake> messageId;
    Field<Core::Snowflake, true> guildId;
    Field<std::vector<DebouncedReaction>> reactions;

    static MessageReactionAddMany fromJson(const nlohmann::json &obj)
    {
        MessageReactionAddMany event;
        get(obj, "channel_id", event.channelId);
        get(obj, "message_id", event.messageId);
        get(obj, "guild_id", event.guildId);
        get(obj, "reactions", event.reactions);
        return event;
    }
};

struct MessageReactionRemove : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> userId;
    Field<Core::Snowflake> channelId;
    Field<Core::Snowflake> messageId;
    Field<Core::Snowflake, true> guildId;
    Field<Emoji> emoji;
    Field<int, true> type;

    static MessageReactionRemove fromJson(const nlohmann::json &obj)
    {
        MessageReactionRemove event;
        get(obj, "user_id", event.userId);
        get(obj, "channel_id", event.channelId);
        get(obj, "message_id", event.messageId);
        get(obj, "guild_id", event.guildId);
        get(obj, "emoji", event.emoji);
        get(obj, "type", event.type);
        return event;
    }
};

struct MessageReactionRemoveAll : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> channelId;
    Field<Core::Snowflake> messageId;
    Field<Core::Snowflake, true> guildId;

    static MessageReactionRemoveAll fromJson(const nlohmann::json &obj)
    {
        MessageReactionRemoveAll event;
        get(obj, "channel_id", event.channelId);
        get(obj, "message_id", event.messageId);
        get(obj, "guild_id", event.guildId);
        return event;
    }
};

struct MessageReactionRemoveEmoji : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> channelId;
    Field<Core::Snowflake> messageId;
    Field<Core::Snowflake, true> guildId;
    Field<Emoji> emoji;

    static MessageReactionRemoveEmoji fromJson(const nlohmann::json &obj)
    {
        MessageReactionRemoveEmoji event;
        get(obj, "channel_id", event.channelId);
        get(obj, "message_id", event.messageId);
        get(obj, "guild_id", event.guildId);
        get(obj, "emoji", event.emoji);
        return event;
    }
};

struct GuildMemberListUpdate : Core::JsonUtils::JsonObject
{
    struct Group : Core::JsonUtils::JsonObject
    {
        Field<std::string> id; // role snowflake string, "online", or "offline"
        Field<int> count;

        static Group fromJson(const nlohmann::json &obj)
        {
            Group group;
            get(obj, "id", group.id);
            get(obj, "count", group.count);
            return group;
        }
    };

    struct SyncItem : Core::JsonUtils::JsonObject
    {
        Field<Group, true> group;
        Field<Member, true> member;

        static SyncItem fromJson(const nlohmann::json &obj)
        {
            SyncItem item;
            if (obj.contains("group"))
                item.group = Group::fromJson(obj["group"]);
            if (obj.contains("member"))
                item.member = Member::fromJson(obj["member"]);
            return item;
        }
    };

    struct ListOp : Core::JsonUtils::JsonObject
    {
        Field<std::string> op;
        Field<std::pair<int, int>, true> range;
        Field<std::vector<SyncItem>, true> items;
        Field<int, true> index;
        Field<SyncItem, true> item;

        static ListOp fromJson(const nlohmann::json &obj)
        {
            ListOp listOp;
            get(obj, "op", listOp.op);
            get(obj, "index", listOp.index);

            if (obj.contains("range")) {
                nlohmann::json rangeArr = obj["range"];
                if (rangeArr.size() == 2)
                    listOp.range = std::pair<int, int>(rangeArr[0].get<int>(), rangeArr[1].get<int>());
            }

            if (obj.contains("items")) {
                nlohmann::json itemsArr = obj["items"];
                std::vector<SyncItem> syncItems;
                syncItems.reserve(itemsArr.size());
                for (const nlohmann::json &val : itemsArr)
                    syncItems.push_back(SyncItem::fromJson(val));
                listOp.items = syncItems;
            }

            if (obj.contains("item"))
                listOp.item = SyncItem::fromJson(obj["item"]);

            return listOp;
        }
    };

    Field<std::string> id; // member list id
    Field<Core::Snowflake> guildId;
    Field<std::vector<Group>> groups;
    Field<std::vector<ListOp>> ops;
    Field<int> memberCount;
    Field<int> onlineCount;

    static GuildMemberListUpdate fromJson(const nlohmann::json &obj)
    {
        GuildMemberListUpdate event;
        get(obj, "id", event.id);
        get(obj, "guild_id", event.guildId);
        get(obj, "member_count", event.memberCount);
        get(obj, "online_count", event.onlineCount);

        if (obj.contains("groups")) {
            nlohmann::json groupsArr = obj["groups"];
            std::vector<Group> groups;
            groups.reserve(groupsArr.size());
            for (const nlohmann::json &val : groupsArr)
                groups.push_back(Group::fromJson(val));
            event.groups = groups;
        }

        if (obj.contains("ops")) {
            nlohmann::json opsArr = obj["ops"];
            std::vector<ListOp> ops;
            ops.reserve(opsArr.size());
            for (const nlohmann::json &val : opsArr)
                ops.push_back(ListOp::fromJson(val));
            event.ops = ops;
        }

        return event;
    }
};

struct VoiceServerUpdate : Core::JsonUtils::JsonObject
{
    Field<std::string> token;
    Field<Core::Snowflake> guildId;
    Field<std::string, false, true> endpoint;

    static VoiceServerUpdate fromJson(const nlohmann::json &obj)
    {
        VoiceServerUpdate event;
        get(obj, "token", event.token);
        get(obj, "guild_id", event.guildId);
        get(obj, "endpoint", event.endpoint);
        return event;
    }
};

// The call in a DM or group DM, as CALL_CREATE and CALL_UPDATE describe it.
struct CallDescription : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> channelId;
    Field<Core::Snowflake, true, true> messageId;
    // Null and absent both mean the call has no voice region yet.
    std::optional<std::string> region;
    // `ongoing_rings`: who is being rung, mapped to who rang them.
    std::unordered_map<Core::Snowflake, Core::Snowflake> ongoingRings;

protected:
    void read(const nlohmann::json &obj)
    {
        get(obj, "channel_id", channelId);
        get(obj, "message_id", messageId);

        const auto regionValue = obj.find("region");
        if (regionValue != obj.end() && regionValue->is_string())
            region = regionValue->get<std::string>();

        const auto rings = obj.find("ongoing_rings");
        if (rings != obj.end() && rings->is_object()) {
            for (auto it = rings->begin(); it != rings->end(); ++it) {
                const auto ringer = it.value().is_string()
                        ? Core::JsonUtils::detail::snowflake(it.value().get_ref<const std::string &>())
                        : Core::Snowflake::Invalid;
                ongoingRings.emplace(Core::JsonUtils::detail::snowflake(it.key()), ringer);
            }
        }
    }
};

struct CallCreate : CallDescription
{
    // Everyone already in the call. These never carry a guild id.
    Field<std::vector<VoiceState>, true> voiceStates;

    static CallCreate fromJson(const nlohmann::json &obj)
    {
        CallCreate event;
        event.read(obj);
        get(obj, "voice_states", event.voiceStates);
        return event;
    }
};

struct CallUpdate : CallDescription
{
    static CallUpdate fromJson(const nlohmann::json &obj)
    {
        CallUpdate event;
        event.read(obj);
        return event;
    }
};

struct CallDelete : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> channelId;
    Field<bool, true> unavailable;

    static CallDelete fromJson(const nlohmann::json &obj)
    {
        CallDelete event;
        get(obj, "channel_id", event.channelId);
        get(obj, "unavailable", event.unavailable);
        return event;
    }
};

// Go Live. A stream key is "guild:<guild>:<channel>:<user>" or "call:<channel>:<user>".
struct StreamCreate : Core::JsonUtils::JsonObject
{
    Field<std::string> streamKey;
    Field<Core::Snowflake> rtcServerId;
    Field<std::vector<Core::Snowflake>, true> viewerIds;
    Field<bool, true> paused;

    static StreamCreate fromJson(const nlohmann::json &obj)
    {
        StreamCreate event;
        get(obj, "stream_key", event.streamKey);
        get(obj, "rtc_server_id", event.rtcServerId);
        get(obj, "viewer_ids", event.viewerIds);
        get(obj, "paused", event.paused);
        return event;
    }
};

struct StreamUpdate : Core::JsonUtils::JsonObject
{
    Field<std::string> streamKey;
    Field<std::vector<Core::Snowflake>, true> viewerIds;
    Field<bool, true> paused;

    static StreamUpdate fromJson(const nlohmann::json &obj)
    {
        StreamUpdate event;
        get(obj, "stream_key", event.streamKey);
        get(obj, "viewer_ids", event.viewerIds);
        get(obj, "paused", event.paused);
        return event;
    }
};

struct StreamServerUpdate : Core::JsonUtils::JsonObject
{
    Field<std::string> streamKey;
    Field<std::string> token;
    Field<std::string, false, true> endpoint;

    static StreamServerUpdate fromJson(const nlohmann::json &obj)
    {
        StreamServerUpdate event;
        get(obj, "stream_key", event.streamKey);
        get(obj, "token", event.token);
        get(obj, "endpoint", event.endpoint);
        return event;
    }
};

struct StreamDelete : Core::JsonUtils::JsonObject
{
    Field<std::string> streamKey;
    Field<std::string, true, true> reason;
    Field<bool, true> unavailable;

    static StreamDelete fromJson(const nlohmann::json &obj)
    {
        StreamDelete event;
        get(obj, "stream_key", event.streamKey);
        get(obj, "reason", event.reason);
        get(obj, "unavailable", event.unavailable);
        return event;
    }
};

struct VoiceStateUpdateBatch : Core::JsonUtils::JsonObject
{
    Field<std::vector<VoiceState>, true> voiceStates;

    static VoiceStateUpdateBatch fromJson(const nlohmann::json &obj)
    {
        VoiceStateUpdateBatch event;
        get(obj, "voice_states", event.voiceStates);
        return event;
    }
};

struct RelationshipPartial : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<RelationshipType> type;
    Field<std::string, false, true> nickname;
    Field<Platform::DateTime, true> since;
    Field<bool, true> strangerRequest;
    Field<bool> userIgnored;

    static RelationshipPartial fromJson(const nlohmann::json &obj)
    {
        RelationshipPartial r;
        get(obj, "id", r.id);
        get(obj, "type", r.type);
        get(obj, "nickname", r.nickname);
        get(obj, "since", r.since);
        get(obj, "stranger_request", r.strangerRequest);
        get(obj, "user_ignored", r.userIgnored);
        return r;
    }
};

struct UserNoteUpdate : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<std::string> note;

    static UserNoteUpdate fromJson(const nlohmann::json &obj)
    {
        UserNoteUpdate n;
        get(obj, "id", n.id);
        get(obj, "note", n.note);
        return n;
    }
};

struct InteractionCreate : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<std::string, true> nonce;

    static InteractionCreate fromJson(const nlohmann::json &obj)
    {
        InteractionCreate event;
        get(obj, "id", event.id);
        get(obj, "nonce", event.nonce);
        return event;
    }
};

struct InteractionSuccess : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<std::string, false, true> nonce;

    static InteractionSuccess fromJson(const nlohmann::json &obj)
    {
        InteractionSuccess event;
        get(obj, "id", event.id);
        get(obj, "nonce", event.nonce);
        return event;
    }
};

struct InteractionFailure : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> id;
    Field<std::string, true> nonce;
    Field<int> reasonCode;

    static InteractionFailure fromJson(const nlohmann::json &obj)
    {
        InteractionFailure event;
        get(obj, "id", event.id);
        get(obj, "nonce", event.nonce);
        get(obj, "reason_code", event.reasonCode);
        return event;
    }
};

} // namespace Discord
} // namespace Styx
