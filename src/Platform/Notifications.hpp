#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace Styx::Platform::Notifications {

struct Notification
{
    std::string title;
    std::string body;
    // A local image shown beside the text, such as the author's avatar. May be empty.
    std::filesystem::path image;
};

// Runs on the main thread when the user clicks the notification.
using ClickHandler = std::function<void()>;

// Whether this system can show notifications and the user hasn't turned them off for Styx.
[[nodiscard]] bool available();

// Shows a notification. Call it on the main thread. Returns false if it couldn't be shown.
bool show(const Notification &notification, ClickHandler onClick);

// The Windows toast XML for a notification, with its text escaped.
[[nodiscard]] std::string toastXml(const Notification &notification);

} // namespace Styx::Platform::Notifications
