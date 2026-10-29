#pragma once

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Core/ImageManager.hpp"

namespace Styx::UI {

template <typename Target>
class AvatarRequestTracker
{
public:
    Platform::ImageBuffer fetch(Core::ImageManager *imageManager, const std::string &url,
                                Platform::Size size, const Target &target,
                                Core::Snowflake accountId,
                                Core::PinGroup pin = Core::PinGroup::None)
    {
        auto image = imageManager->get(url, size, accountId, pin);
        if (!imageManager->isCached(url, size))
            track(url, target);
        return image;
    }

    void track(const std::string &url, const Target &target)
    {
        auto &targets = pending[url];
        if (std::find(targets.begin(), targets.end(), target) == targets.end())
            targets.push_back(target);
    }

    template <typename RefreshFn>
    void notify(const std::string &url, RefreshFn &&refresh)
    {
        auto it = pending.find(url);
        if (it == pending.end())
            return;
        auto targets = std::move(it->second);
        pending.erase(it);
        for (const auto &target : targets)
            refresh(target);
    }

    void clear() { pending.clear(); }

private:
    std::unordered_map<std::string, std::vector<Target>> pending;
};

} // namespace Styx::UI
