#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

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

class Image {
public:
    // Decode image from memory (PNG, JPEG, GIF first frame, WebP, BMP) to RGBA8.
    static std::optional<ImageBuffer> decode(const uint8_t *data, size_t size);

    // Decode image from file path to RGBA8.
    static std::optional<ImageBuffer> decodeFile(const std::filesystem::path &path);

    // Scale image buffer to target dimensions using stb_image_resize2 linear filtering.
    static ImageBuffer scale(const ImageBuffer &src, int targetWidth, int targetHeight);
};

} // namespace Styx::Platform
