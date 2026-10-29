#pragma once

#include <cstdint>
#include <string>
#include "Platform/Image.hpp"

namespace Styx::Core {

struct PendingAttachment
{
    std::string filePath;
    Platform::ImageBuffer image;
    std::string data;
    std::string filename;
    std::string description;
    std::string mimeType;
    std::int64_t size = 0;
    bool isSpoiler = false;
};

} // namespace Styx::Core
