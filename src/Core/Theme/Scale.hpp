#pragma once

#include <array>
#include <span>

namespace Styx::Core::Theme {

// Discord's zoom levels and chat text sizes, in percent and pixels at 100% zoom.
inline constexpr std::array<int, 11> zoomLevels{50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200};
inline constexpr std::array<int, 7> chatTextSizes{12, 14, 15, 16, 18, 20, 24};
inline constexpr int defaultZoom = 100;
inline constexpr int defaultChatTextSize = 16;

// The stop closest to value; ties go to the smaller stop.
int nearestStop(std::span<const int> stops, double value);
// The next stop above (direction > 0) or below the closest stop to current, clamped to the ends.
int stepStop(std::span<const int> stops, int current, int direction);

// The saved interface zoom in percent, snapped to a zoom level.
int savedZoom();
// Saves the zoom and writes the settings file; returns whether it was written.
bool saveZoom(int percent);
// Chat text size in pixels derived from the message font.
int chatTextSize();
// Sets the message font for a chat text size; the caller applies and saves the theme.
void setChatTextSize(int pixels);
// Earlier versions scaled interface and code font sizes separately. Interface size is now the zoom
// level and code follows chat text, so a saved interface size becomes the nearest zoom level and both
// font overrides are removed. Returns whether anything changed.
bool migrateFontScaling();

} // namespace Styx::Core::Theme
