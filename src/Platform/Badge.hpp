#pragma once

#include <cstdint>
#include <vector>

namespace Styx::Platform::Badge {

// A size x size notification mark, row by row, as 0xAARRGGBB with straight
// alpha: a red disc with a white dot, independent of the notification count.
[[nodiscard]] std::vector<std::uint32_t> render(int size);

// Positive counts show a notification marker; zero clears it. Windows draws a
// red-and-white dot and exposes the count to accessibility. window is an HWND.
void setCount(void *window, int count);

// Whether this platform can show the marker at all.
[[nodiscard]] bool supported();

} // namespace Styx::Platform::Badge
