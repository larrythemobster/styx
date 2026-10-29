#include "UI/Gifs/GifMasonryLayout.hpp"


#include <algorithm>
#include <cmath>
#include <vector>

namespace Styx {
namespace UI {
namespace GifMasonry {

int gutterFor(int columns)
{
    return std::clamp(FewestColumns * RoomiestGutter / std::max(1, columns), TightestGutter, RoomiestGutter);
}

Platform::Size stillSizeFor(const Platform::Size &naturalSize, int tileWidth)
{
    const int width = tileWidth <= DenseStillWidth ? DenseStillWidth : RoomyStillWidth;
    const bool sized = naturalSize.width > 0 && naturalSize.height > 0;
    return {width, sized ? std::max(1, int(std::lround(width * double(naturalSize.height) / naturalSize.width))) : width};
}

Arrangement arrange(const std::vector<Platform::Size> &naturalSizes, int availableWidth, int requestedColumns)
{
    const int columns = std::max(1, requestedColumns);
    const int gutter = gutterFor(columns);
    const int columnWidth = std::max(1, (availableWidth - 2 * gutter - gutter * (columns - 1)) / columns);

    Arrangement arrangement;
    arrangement.gutter = gutter;
    arrangement.tiles.reserve(naturalSizes.size());
    std::vector<int> columnBottoms(size_t(columns), gutter);

    for (const Platform::Size &natural : naturalSizes) {
        const bool sized = natural.width > 0 && natural.height > 0;
        const int height = sized ? std::max(1, int(std::lround(columnWidth * double(natural.height) / natural.width))) : columnWidth;

        const auto shortest = std::min_element(columnBottoms.begin(), columnBottoms.end());
        const int column = int(shortest - columnBottoms.begin());
        arrangement.tiles.push_back({float(gutter + column * (columnWidth + gutter)), float(*shortest), float(columnWidth), float(height)});
        *shortest += height + gutter;
    }

    arrangement.contentHeight = *std::max_element(columnBottoms.begin(), columnBottoms.end());
    return arrangement;
}

} // namespace GifMasonry
} // namespace UI
} // namespace Styx
