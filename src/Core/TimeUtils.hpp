#pragma once

#include "Platform/Native.hpp"

#include <algorithm>
#include <string>

namespace Styx {
namespace Core {
namespace TimeUtils {

inline std::string relativeTime(const Platform::DateTime &dt)
{
    if (!dt.valid())
        return {};
    const auto secs = std::max<int64_t>(0, (Platform::DateTime::now().unixMillis() - dt.unixMillis()) / 1000);
    if (secs < 60)
        return "just now";
    if (secs < 3600)
        return std::to_string(secs / 60) + "m ago";
    if (secs < 86400)
        return std::to_string(secs / 3600) + "h ago";
    if (secs < 2592000)
        return std::to_string(secs / 86400) + "d ago";
    auto month = dt.format("%b");
    auto day = dt.format("%d");
    if (day.size() == 2 && day[0] == '0')
        day.erase(0, 1);
    return month + " " + day;
}

inline std::string absoluteTime(const Platform::DateTime &dt)
{
    if (!dt.valid())
        return {};
    return dt.format("%x %X");
}

} // namespace TimeUtils
} // namespace Core
} // namespace Styx
