#pragma once

namespace Styx::Core::Video {

struct FrameSize {
    int width = 0;
    int height = 0;
    [[nodiscard]] constexpr bool empty() const { return width <= 0 || height <= 0; }
    [[nodiscard]] constexpr bool isEmpty() const { return empty(); }
    bool operator==(const FrameSize &) const = default;
};

} // namespace Styx::Core::Video
