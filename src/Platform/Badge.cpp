#include "Badge.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Styx::Platform::Badge {
#ifdef _WIN32
void drawLabelNative(std::vector<std::uint32_t> &pixels, std::string_view text, int size);
#endif

namespace {

constexpr std::uint32_t RED = 0xED4245;

#ifndef _WIN32
// A 3x5 pixel font: one row per entry, top row first, the left column in the highest of three bits.
struct Glyph
{
    char character;
    std::array<std::uint8_t, 5> rows;
};

constexpr std::array<Glyph, 11> GLYPHS = {{
    {'0', {0b111, 0b101, 0b101, 0b101, 0b111}},
    {'1', {0b010, 0b110, 0b010, 0b010, 0b111}},
    {'2', {0b111, 0b001, 0b111, 0b100, 0b111}},
    {'3', {0b111, 0b001, 0b111, 0b001, 0b111}},
    {'4', {0b101, 0b101, 0b111, 0b001, 0b001}},
    {'5', {0b111, 0b100, 0b111, 0b001, 0b111}},
    {'6', {0b111, 0b100, 0b111, 0b101, 0b111}},
    {'7', {0b111, 0b001, 0b001, 0b001, 0b001}},
    {'8', {0b111, 0b101, 0b111, 0b101, 0b111}},
    {'9', {0b111, 0b101, 0b111, 0b001, 0b111}},
    {'+', {0b000, 0b010, 0b111, 0b010, 0b000}},
}};

const Glyph *glyphFor(char character)
{
    for (const auto &glyph : GLYPHS)
        if (glyph.character == character)
            return &glyph;
    return nullptr;
}
#endif

} // namespace

std::string label(int count)
{
    if (count <= 0)
        return {};
    if (count > 9)
        return "9+";
    return std::to_string(count);
}

std::vector<std::uint32_t> render(std::string_view text, int size)
{
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(size) * size, 0);
    // The disc: each pixel's alpha is how much of it lies inside the circle, which smooths the edge.
    const double centre = size / 2.0;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double distance = std::hypot(x + 0.5 - centre, y + 0.5 - centre);
            const double coverage = std::clamp(centre - distance + 0.5, 0.0, 1.0);
            const auto alpha = static_cast<std::uint32_t>(std::lround(coverage * 255));
            if (alpha)
                pixels[static_cast<std::size_t>(y) * size + x] = (alpha << 24) | RED;
        }
    }

    const int count = static_cast<int>(text.size());
    if (count == 0)
        return pixels;
#ifdef _WIN32
    drawLabelNative(pixels, text, size);
#else
    const double scale = std::min(size * 0.60 / 5, size * 0.72 / (count * 4 - 1));
    const double left = (size - (count * 4 - 1) * scale) / 2;
    const double top = (size - 5 * scale) / 2;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int covered = 0;
            for (int sy = 0; sy < 4; ++sy) {
                for (int sx = 0; sx < 4; ++sx) {
                    const double gx = (x + (sx + 0.5) / 4 - left) / scale;
                    const double gy = (y + (sy + 0.5) / 4 - top) / scale;
                    if (gx < 0 || gy < 0 || gy >= 5)
                        continue;
                    const int index = int(gx) / 4;
                    const int column = int(gx) % 4;
                    if (index >= count || column >= 3)
                        continue;
                    const auto *glyph = glyphFor(text[index]);
                    if (glyph && (glyph->rows[int(gy)] & (0b100 >> column)))
                        ++covered;
                }
            }
            if (covered) {
                auto &pixel = pixels[static_cast<std::size_t>(y) * size + x];
                const auto channel = [covered](unsigned value) { return value + (255 - value) * covered / 16; };
                pixel = (pixel & 0xFF000000u) | (channel((RED >> 16) & 255) << 16) |
                    (channel((RED >> 8) & 255) << 8) | channel(RED & 255);
            }
        }
    }
#endif
    return pixels;
}

} // namespace Styx::Platform::Badge
