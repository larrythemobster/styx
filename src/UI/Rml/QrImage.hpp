#pragma once

#include "Platform/Image.hpp"

#include <optional>
#include <string>

namespace Styx::UI::Rml {

// A size x size black-on-white QR code for text with a four-module quiet zone, or nothing
// when the text doesn't fit in a QR code.
[[nodiscard]] std::optional<Platform::ImageBuffer> qrImage(const std::string &text, int size);

} // namespace Styx::UI::Rml
