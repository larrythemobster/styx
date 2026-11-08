#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Styx::Platform::Badge {

// The text a badge shows for a mention count: empty for none, "1" to "9", then "9+".
[[nodiscard]] std::string label(int count);

// A size x size badge, row by row from the top, as 0xAARRGGBB with straight (not
// premultiplied) alpha: a red disc with the label in white.
[[nodiscard]] std::vector<std::uint32_t> render(std::string_view label, int size);

// Shows a mention count on Styx's taskbar or dock entry, or clears it for 0. window is the
// native window handle (an HWND on Windows); other platforms ignore it.
void setCount(void *window, int count);

} // namespace Styx::Platform::Badge
