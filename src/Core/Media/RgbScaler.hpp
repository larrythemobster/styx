// Ported from ouwou/Acheron video-streams commit 71b4907 (GPL-3.0-or-later).
#pragma once

#include "Core/Video/VideoTypes.hpp"
#include "Platform/Image.hpp"

extern "C" {
#include <libavutil/frame.h>
}

struct SwsContext;

namespace Styx::Core::Media {

// Untagged video is taken as BT.709 at HD sizes and BT.601 below, as players do.
[[nodiscard]] bool usesBt709(const AVFrame *frame);
[[nodiscard]] bool isFullRange(const AVFrame *frame);

// Scales decoded pictures into 32-bit RGBA image buffers.
//
// swscale converts every picture as BT.601 at video range unless told otherwise, whatever the
// frame itself says, so this passes on the frame's own colour description. It follows a
// stream whose size, format or colours change part way through.
class RgbScaler {
public:
    RgbScaler() = default;
    ~RgbScaler();

    RgbScaler(const RgbScaler &) = delete;
    RgbScaler &operator=(const RgbScaler &) = delete;

    // An image buffer for scale() to write into: aligned for swscale's vector code, with room after
    // the last row, which that code can run past.
    [[nodiscard]] static Platform::ImageBuffer allocate(const Video::FrameSize &size);

    // Fills `into`, which came from allocate(), at its own size.
    bool scale(const AVFrame *frame, Platform::ImageBuffer &into);

private:
    struct Source {
        int width = 0;
        int height = 0;
        int format = -1;
        bool bt709 = false;
        bool fullRange = false;
        Video::FrameSize scaledTo;

        bool operator==(const Source &) const = default;
    };

    SwsContext *context = nullptr;
    Source configuredFor;
};

} // namespace Styx::Core::Media
