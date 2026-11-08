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

namespace {

constexpr std::uint32_t RED = 0xED4245;
constexpr std::uint32_t WHITE = 0xFFFFFFFF;

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

    const int scale = std::max(1, size / 8);
    const int count = static_cast<int>(text.size());
    const int width = count * 3 * scale + std::max(0, count - 1) * scale;
    const int left = (size - width) / 2;
    const int top = (size - 5 * scale) / 2;
    for (int index = 0; index < count; ++index) {
        const Glyph *glyph = glyphFor(text[index]);
        if (!glyph)
            continue;
        const int glyphLeft = left + index * 4 * scale;
        for (int row = 0; row < 5; ++row) {
            for (int column = 0; column < 3; ++column) {
                if (!(glyph->rows[row] & (0b100 >> column)))
                    continue;
                for (int dy = 0; dy < scale; ++dy) {
                    for (int dx = 0; dx < scale; ++dx) {
                        const int x = glyphLeft + column * scale + dx;
                        const int y = top + row * scale + dy;
                        if (x >= 0 && x < size && y >= 0 && y < size)
                            pixels[static_cast<std::size_t>(y) * size + x] = WHITE;
                    }
                }
            }
        }
    }
    return pixels;
}

} // namespace Styx::Platform::Badge
