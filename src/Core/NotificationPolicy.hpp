#pragma once

#include "Discord/Enums.hpp"

namespace Styx::Core {

// Everything the notification decision depends on, resolved by the caller from the message,
// the channel and the user's notification settings.
struct NotificationFacts
{
    bool fromSelf = false;
    bool authorBlocked = false;
    // A DM or group DM. Their only setting is mute.
    bool privateChannel = false;
    // The channel, its parent, its category or its server is muted, with expired mutes already
    // treated as unmuted.
    bool muted = false;
    // The resolved level for this channel. INHERIT is treated as ALL_MESSAGES.
    Discord::MessageNotificationLevel level = Discord::MessageNotificationLevel::ALL_MESSAGES;
    // The user is in the message's mentions list. A reply that pings its author lists them here.
    bool mentionsMe = false;
    bool mentionsEveryone = false;
    bool mentionsMyRole = false;
    bool suppressEveryone = false;
    bool suppressRoles = false;
    // The channel is open in the window and the window has focus.
    bool viewingChannel = false;
    // The user's status is Do Not Disturb.
    bool doNotDisturb = false;
};

// Whether the message mentions the user, by name, role or @everyone, after suppression settings.
[[nodiscard]] bool isMentioned(const NotificationFacts &facts);
[[nodiscard]] bool shouldNotify(const NotificationFacts &facts);

} // namespace Styx::Core
