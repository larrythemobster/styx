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
void drawLabelNative(std::vector<std::uint32_t> &pixels, std::string_view text, int width, int height);
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

std::vector<std::uint32_t> render(std::string_view text, int width, int height)
{
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * height, 0);
    const double radius = std::min(width, height) / 2.0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const double dx = std::max(std::abs(x + 0.5 - width / 2.0) - (width / 2.0 - radius), 0.0);
            const double dy = std::max(std::abs(y + 0.5 - height / 2.0) - (height / 2.0 - radius), 0.0);
            const double coverage = std::clamp(radius - std::hypot(dx, dy) + 0.5, 0.0, 1.0);
            const auto alpha = static_cast<std::uint32_t>(std::lround(coverage * 255));
            if (alpha)
                pixels[static_cast<std::size_t>(y) * width + x] = (alpha << 24) | RED;
        }
    }

    const int count = static_cast<int>(text.size());
    if (count == 0)
        return pixels;
#ifdef _WIN32
    drawLabelNative(pixels, text, width, height);
#else
    const double scale = std::min(height * 0.60 / 5, width * 0.70 / (count * 4 - 1));
    const double left = (width - (count * 4 - 1) * scale) / 2;
    const double top = (height - 5 * scale) / 2;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
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
                auto &pixel = pixels[static_cast<std::size_t>(y) * width + x];
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
