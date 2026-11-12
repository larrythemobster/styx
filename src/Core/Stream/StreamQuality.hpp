#pragma once

#include "Platform/Native.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

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
    bool captureCursor = true;

    [[nodiscard]] std::string label() const
    {
        std::string text = size.height ? std::to_string(size.height) + "p" : "Source";
        text += " " + std::to_string(framerate) + " fps";
        if (needsNitro)
            text += " (Nitro)";
        return text;
    }

    static StreamQuality custom(int height, int fps)
    {
        StreamQuality quality;
        quality.id = height ? std::to_string(height) + "p" + std::to_string(fps) : "source" + std::to_string(fps);
        quality.size = height ? Platform::Size{height * 16 / 9, height} : Platform::Size{};
        quality.framerate = fps;
        quality.bitrate = height <= 720 && height != 0 ? (fps > 30 ? 4'000'000 : 2'500'000) :
            height <= 1080 && height != 0 ? (fps > 30 ? 7'000'000 : 4'500'000) :
            (fps > 30 ? 10'000'000 : 7'000'000);
        quality.maxBitrate = quality.bitrate + (fps > 30 ? 2'000'000 : 1'500'000);
        quality.needsNitro = height == 0 || height > 720 || fps > 30;
        return quality;
    }

    [[nodiscard]] StreamQuality resolved(Platform::Size source) const
    {
        StreamQuality quality = *this;
        if (size.empty()) {
            quality.size = {std::max(2, source.width & ~1), std::max(2, source.height & ~1)};
            const int64_t rate = int64_t(quality.size.width) * quality.size.height * framerate / 10;
            quality.bitrate = int(std::clamp<int64_t>(rate, 2'500'000, 14'000'000));
            quality.maxBitrate = quality.bitrate + 2'000'000;
        }
        return quality;
    }

    // Discord's media server enforces account entitlements for higher quality.
    static const std::array<StreamQuality, 15> &presets()
    {
        static const std::array<StreamQuality, 15> all = {
            custom(720, 30), custom(720, 60), custom(1080, 30), custom(1080, 60),
            custom(480, 15), custom(480, 30), custom(480, 60), custom(720, 15),
            custom(1080, 15), custom(1440, 15), custom(1440, 30), custom(1440, 60),
            custom(0, 15), custom(0, 30), custom(0, 60),
        };
        return all;
    }

    static StreamQuality byId(const std::string &id)
    {
        for (const auto &preset : presets()) {
            if (preset.id == id)
                return preset;
        }
        return presets().front();
    }
};

} // namespace Stream
} // namespace Core
} // namespace Styx
