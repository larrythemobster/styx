#pragma once

#include "Platform/Image.hpp"
#include "Platform/Native.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace Styx::Core::Media {

class ClipDecoder {
public:
    virtual ~ClipDecoder() = default;
    [[nodiscard]] static std::unique_ptr<ClipDecoder> open(const std::string &path);
    [[nodiscard]] virtual Platform::Size nativeSize() const = 0;
    virtual bool nextFrameOfLoop(Platform::ImageBuffer &frame, int64_t &startMs, Platform::Size frameSize) = 0;
    [[nodiscard]] virtual int64_t finishedLoopMs() const = 0;
    virtual bool rewind() = 0;
};

} // namespace Styx::Core::Media
