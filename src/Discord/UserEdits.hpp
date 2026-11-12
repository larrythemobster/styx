#pragma once

#include "Core/JsonUtils.hpp"
#include "Entities.hpp"
#include <string>

namespace Styx {
namespace Discord {

struct CurrentUserEdit : Core::JsonUtils::JsonObject
{
    Field<std::string, true, true> globalName;
    Field<std::string, true, true> avatar;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "global_name", globalName);
        insert(obj, "avatar", avatar);
        return obj;
    }

    bool empty() const
    {
        return globalName.isUndefined() && avatar.isUndefined();
    }
};

struct UserProfileEdit : Core::JsonUtils::JsonObject
{
    Field<std::string, true, true> pronouns;
    Field<std::string, true, true> bio;
    Field<std::string, true, true> banner;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "pronouns", pronouns);
        insert(obj, "bio", bio);
        insert(obj, "banner", banner);
        return obj;
    }

    bool empty() const
    {
        return pronouns.isUndefined() && bio.isUndefined() && banner.isUndefined();
    }
};

struct UserGuildSettingsEdit : Core::JsonUtils::JsonObject
{
    Field<bool, true> muted;
    Field<MuteConfig, true, true> muteConfig;
    Field<MessageNotificationLevel, true> messageNotifications;
    Field<bool, true> hideMutedChannels;
    Field<bool, true> suppressEveryone;
    Field<bool, true> suppressRoles;
    Field<bool, true> mobilePush;
    Field<bool, true> muteScheduledEvents;
    Field<int, true> notifyHighlights;
    Field<int, true> flags;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "muted", muted);
        insert(obj, "mute_config", muteConfig);
        insert(obj, "message_notifications", messageNotifications);
        insert(obj, "hide_muted_channels", hideMutedChannels);
        insert(obj, "suppress_everyone", suppressEveryone);
        insert(obj, "suppress_roles", suppressRoles);
        insert(obj, "mobile_push", mobilePush);
        insert(obj, "mute_scheduled_events", muteScheduledEvents);
        insert(obj, "notify_highlights", notifyHighlights);
        insert(obj, "flags", flags);
        return obj;
    }
};

struct CurrentMemberEdit : Core::JsonUtils::JsonObject
{
    Field<std::string, true, true> nick;
    Field<std::string, true, true> avatar;
    Field<std::string, true, true> banner;
    Field<std::string, true, true> bio;
    Field<std::string, true, true> pronouns;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "nick", nick);
        insert(obj, "avatar", avatar);
        insert(obj, "banner", banner);
        insert(obj, "bio", bio);
        insert(obj, "pronouns", pronouns);
        return obj;
    }
};

} // namespace Discord
} // namespace Styx
