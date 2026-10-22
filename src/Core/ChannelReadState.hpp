#pragma once

namespace Styx {
namespace Core {

struct ChannelReadState
{
    bool isUnread = false;
    int mentionCount = 0;
    bool isMuted = false;
    bool countsForGuildUnread = false;

    bool operator==(const ChannelReadState &) const = default;
};

} // namespace Core
} // namespace Styx
