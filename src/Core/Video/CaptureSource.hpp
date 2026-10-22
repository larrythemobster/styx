#pragma once

#include "Core/Video/VideoTypes.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Styx::Core::Video {

struct CaptureRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    [[nodiscard]] constexpr bool empty() const { return width <= 0 || height <= 0; }
    bool operator==(const CaptureRect &) const = default;
};

// A desktop source offered for sharing. Coordinates are native desktop pixels.
struct CaptureSource {
    enum class Kind { Screen, Window };

    std::string id;
    Kind kind = Kind::Screen;
    std::string name;
    CaptureRect geometry;
    std::uint32_t processId = 0;
};

using CaptureSources = std::vector<CaptureSource>;

} // namespace Styx::Core::Video
