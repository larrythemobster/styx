#pragma once

#include "Platform/Image.hpp"
#include "Platform/Native.hpp"
#include <string>

#include <memory>

namespace Styx {
namespace Core {
namespace Apng {

class Reader
{
public:
    ~Reader();

    // null unless this ffmpeg is enabled and this is apng (not png)
    [[nodiscard]] static std::unique_ptr<Reader> open(const std::string &png);

    [[nodiscard]] Platform::Size canvasSize() const;
    [[nodiscard]] int frameCount() const;

    bool next(Platform::ImageBuffer &canvas, int &delayMs);

private:
    struct Impl;
    explicit Reader(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl;
};

} // namespace Apng
} // namespace Core
} // namespace Styx
