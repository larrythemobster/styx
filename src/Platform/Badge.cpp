#include "Badge.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace Styx::Platform::Badge {

std::vector<std::uint32_t> render(int size)
{
    constexpr std::uint32_t red = 0xED4245;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(size) * size, 0);
    const double centre = size / 2.0;
    const double whiteRadius = size * 0.23;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double distance = std::hypot(x + 0.5 - centre, y + 0.5 - centre);
            const auto alpha = static_cast<std::uint32_t>(std::lround(
                std::clamp(centre - distance + 0.5, 0.0, 1.0) * 255));
            if (!alpha)
                continue;
            const double white = std::clamp(whiteRadius - distance + 0.5, 0.0, 1.0);
            const auto channel = [white](unsigned value) {
                return static_cast<std::uint32_t>(std::lround(value + (255 - value) * white));
            };
            pixels[static_cast<std::size_t>(y) * size + x] = (alpha << 24) |
                (channel((red >> 16) & 255) << 16) | (channel((red >> 8) & 255) << 8) | channel(red & 255);
        }
    }
    return pixels;
}

} // namespace Styx::Platform::Badge
