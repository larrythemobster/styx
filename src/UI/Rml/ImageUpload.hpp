#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "Platform/Image.hpp"

namespace Styx::UI::ImageUpload {

struct PreparedImage
{
    std::string dataUri;
    std::string originalMd5;
    std::string error;
    bool animated = false;
    // What will be uploaded, for showing before it's saved: RmlUi can't load the data URI. For
    // an animation it's the first frame; empty when the image can't be decoded here.
    Platform::ImageBuffer preview;
};

std::string fileFilter();
PreparedImage prepareCropped(const std::string &path, int maxWidth, int maxHeight);
PreparedImage prepareRaw(const std::string &path, int64_t maxBytes);
std::string dataUri(std::string_view bytes, std::string_view mimeType);
std::string md5Hex(std::string_view bytes);

} // namespace Styx::UI::ImageUpload
