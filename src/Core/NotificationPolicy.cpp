#include "NotificationPolicy.hpp"

namespace Styx::Core {

bool shouldNotify(const NotificationFacts &facts)
{
    using Level = Discord::MessageNotificationLevel;

    if (facts.fromSelf || facts.authorBlocked || facts.viewingChannel || facts.muted)
        return false;
    // DMs and group DMs only have mute; the notification level is a server setting.
    if (facts.privateChannel)
        return true;

    const bool mentioned = facts.mentionsMe ||
        (facts.mentionsEveryone && !facts.suppressEveryone) ||
        (facts.mentionsMyRole && !facts.suppressRoles);

    switch (facts.level) {
    case Level::NO_MESSAGES:
        return false;
    case Level::ONLY_MENTIONS:
        return mentioned;
    case Level::ALL_MESSAGES:
    case Level::INHERIT:
        return true;
    }

    return mentioned;
}

} // namespace Styx::Core
