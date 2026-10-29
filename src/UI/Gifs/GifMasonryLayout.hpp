#pragma once

#include "Platform/Native.hpp"
#include <vector>

namespace Styx {
namespace UI {
namespace GifMasonry {

inline constexpr int FewestColumns = 2;
inline constexpr int MostColumns = 6;
inline constexpr int RoomiestGutter = 12;
inline constexpr int TightestGutter = 4;
inline constexpr int DenseStillWidth = 128;
inline constexpr int RoomyStillWidth = 256;

struct Arrangement
{
    std::vector<Platform::Rect> tiles;
    int contentHeight = 0;
    int gutter = RoomiestGutter;
};

[[nodiscard]] int gutterFor(int columns);
[[nodiscard]] Platform::Size stillSizeFor(const Platform::Size &naturalSize, int tileWidth);
[[nodiscard]] Arrangement arrange(const std::vector<Platform::Size> &naturalSizes, int availableWidth, int columns);

} // namespace GifMasonry
} // namespace UI
} // namespace Styx
