#pragma once
#include <functional>
#include <string>
#include <string_view>

namespace Styx::Core {

// Looks up a display name for an id found in message text; returns "" when unknown.
struct MentionNames
{
    std::function<std::string(std::string_view userId)> user;
    std::function<std::string(std::string_view channelId)> channel;
    std::function<std::string(std::string_view roleId)> role;
};

// A message's text as a desktop notification shows it.
[[nodiscard]] std::string notificationText(std::string_view content, int attachments, int stickers,
                                           const MentionNames &names);

} // namespace Styx::Core
