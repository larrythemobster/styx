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
};

std::string fileFilter();
PreparedImage prepareCropped(const std::string &path, int maxWidth, int maxHeight);
PreparedImage prepareRaw(const std::string &path, int64_t maxBytes);
std::string dataUri(std::string_view bytes, std::string_view mimeType);
std::string md5Hex(std::string_view bytes);
std::string encodePng(const Platform::ImageBuffer &image);

} // namespace Styx::UI::ImageUpload
