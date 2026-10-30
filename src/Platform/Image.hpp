#pragma once

#include "Native.hpp"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <memory>

namespace Styx::Platform {

struct ImageBuffer {
    int width{0};
    int height{0};
    int channels{4}; // RGBA8
    std::vector<uint8_t> pixels; // size = width * height * 4

    bool empty() const { return pixels.empty() || width <= 0 || height <= 0; }
    size_t byteSize() const { return pixels.size(); }
    size_t stride() const { return static_cast<size_t>(width) * 4; }
};

struct GifFrames {
    struct Free { void operator()(void *data) const noexcept; };
    Size size;
    int count = 0;
    std::unique_ptr<uint8_t[], Free> pixels;
    std::unique_ptr<int[], Free> delays;
};

class Image {
public:
    // Decode image from memory (PNG, JPEG, GIF first frame, WebP, BMP) to RGBA8.
    static std::optional<ImageBuffer> decode(const uint8_t *data, size_t size);
    static std::optional<GifFrames> decodeGif(const uint8_t *data, size_t size);

    // Decode image from file path to RGBA8.
    static std::optional<ImageBuffer> decodeFile(const std::filesystem::path &path);
    // Read dimensions without decoding pixels.
    static std::optional<Size> sizeFile(const std::filesystem::path &path);

    static std::string encodePng(const ImageBuffer &image);
    static bool isAnimated(const uint8_t *data, size_t size);

    // Scale image buffer to target dimensions using stb_image_resize2 linear filtering.
    static ImageBuffer scale(const ImageBuffer &src, int targetWidth, int targetHeight);
};

} // namespace Styx::Platform
