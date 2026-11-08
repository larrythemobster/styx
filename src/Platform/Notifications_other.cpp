#include "Notifications.hpp"

namespace Styx::Platform::Notifications {

// No desktop notification backend on this platform yet.
bool available()
{
    return false;
}

bool show(const Notification &, ClickHandler)
{
    return false;
}

} // namespace Styx::Platform::Notifications
