#pragma once

#include "Core/Video/VideoTypes.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Styx::Core::Video {

struct CapturePoint {
    int x = 0;
    int y = 0;
    bool operator==(const CapturePoint &) const = default;
};

struct CaptureRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    [[nodiscard]] constexpr bool empty() const { return width <= 0 || height <= 0; }
    [[nodiscard]] constexpr bool isEmpty() const { return empty(); }
    [[nodiscard]] constexpr int left() const { return x; }
    [[nodiscard]] constexpr int top() const { return y; }
    [[nodiscard]] constexpr int right() const { return x + width; }
    [[nodiscard]] constexpr int bottom() const { return y + height; }
    [[nodiscard]] constexpr FrameSize size() const { return {width, height}; }
    [[nodiscard]] constexpr CapturePoint topLeft() const { return {x, y}; }
    [[nodiscard]] constexpr CapturePoint center() const { return {x + width / 2, y + height / 2}; }

    [[nodiscard]] constexpr bool contains(int px, int py) const {
        return px >= x && px < x + width && py >= y && py < y + height;
    }
    [[nodiscard]] constexpr bool contains(const CapturePoint &p) const {
        return contains(p.x, p.y);
    }
    [[nodiscard]] constexpr bool contains(const CaptureRect &other) const {
        return other.x >= x && other.right() <= right() && other.y >= y && other.bottom() <= bottom();
    }
    [[nodiscard]] constexpr bool intersects(const CaptureRect &other) const {
        if (empty() || other.empty()) return false;
        return x < other.right() && other.x < right() && y < other.bottom() && other.y < bottom();
    }
    [[nodiscard]] constexpr CaptureRect intersected(const CaptureRect &other) const {
        const int nx = (x > other.x) ? x : other.x;
        const int ny = (y > other.y) ? y : other.y;
        const int nr = (right() < other.right()) ? right() : other.right();
        const int nb = (bottom() < other.bottom()) ? bottom() : other.bottom();
        if (nr <= nx || nb <= ny) return {};
        return {nx, ny, nr - nx, nb - ny};
    }
    [[nodiscard]] constexpr CaptureRect operator&(const CaptureRect &other) const {
        return intersected(other);
    }
    [[nodiscard]] constexpr CaptureRect translated(int dx, int dy) const {
        return {x + dx, y + dy, width, height};
    }
    [[nodiscard]] constexpr CaptureRect translated(const CapturePoint &p) const {
        return {x + p.x, y + p.y, width, height};
    }
    [[nodiscard]] constexpr CaptureRect adjusted(int dx1, int dy1, int dx2, int dy2) const {
        return {x + dx1, y + dy1, width - dx1 + dx2, height - dy1 + dy2};
    }

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
