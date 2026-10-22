#pragma once

namespace Styx::Core::Video {

struct FrameSize {
    int width = 0;
    int height = 0;

    bool operator==(const FrameSize &) const = default;
};

} // namespace Styx::Core::Video
