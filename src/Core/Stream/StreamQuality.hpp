#pragma once

#include "Platform/Native.hpp"
#include <string>
#include <vector>

namespace Styx {
namespace Core {
namespace Stream {

struct StreamQuality
{
    std::string id;
    Platform::Size size;
    int framerate = 30;
    int bitrate = 0;
    int maxBitrate = 0;
    bool needsNitro = false;

    [[nodiscard]] std::string label() const
    {
        std::string text = std::to_string(size.height) + "p " + std::to_string(framerate) + " fps";
        if (needsNitro)
            text += " (Nitro)";
        return text;
    }

    // Discord lets accounts without Nitro stream 720p30; the media server is the judge of
    // the rest, so higher presets are offered but labelled.
    static std::vector<StreamQuality> presets()
    {
        return {
            { "720p30", {1280, 720}, 30, 2'500'000, 4'000'000, false },
            { "720p60", {1280, 720}, 60, 4'000'000, 6'000'000, true },
            { "1080p30", {1920, 1080}, 30, 4'500'000, 6'500'000, true },
            { "1080p60", {1920, 1080}, 60, 7'000'000, 9'000'000, true },
        };
    }

    static StreamQuality byId(const std::string &id)
    {
        const auto all = presets();
        for (const auto &preset : all) {
            if (preset.id == id)
                return preset;
        }
        return all.front();
    }
};

} // namespace Stream
} // namespace Core
} // namespace Styx
