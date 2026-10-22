#include "Image.hpp"

#include <fstream>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

#if __has_include(<webp/decode.h>)
#include <webp/decode.h>
#define STYX_HAS_LIBWEBP 1
#endif

namespace Styx::Platform {

std::optional<ImageBuffer> Image::decode(const uint8_t *data, size_t size)
{
    if (!data || size == 0) {
        return std::nullopt;
    }

#if defined(STYX_HAS_LIBWEBP)
    // Check for WebP RIFF header
    if (size >= 12 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
        data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P') {
        int w = 0, h = 0;
        if (WebPGetInfo(data, size, &w, &h) && w > 0 && h > 0) {
            ImageBuffer buf;
            buf.width = w;
            buf.height = h;
            buf.channels = 4;
            buf.pixels.resize(static_cast<size_t>(w) * h * 4);
            if (WebPDecodeRGBAInto(data, size, buf.pixels.data(), buf.pixels.size(), w * 4)) {
                return buf;
            }
        }
    }
#endif

    int w = 0, h = 0, comp = 0;
    // req_comp = 4 forces conversion to RGBA
    unsigned char *decoded = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &comp, 4);
    if (!decoded) {
        return std::nullopt;
    }

    ImageBuffer buf;
    buf.width = w;
    buf.height = h;
    buf.channels = 4;
    size_t totalBytes = static_cast<size_t>(w) * h * 4;
    buf.pixels.assign(decoded, decoded + totalBytes);
    stbi_image_free(decoded);
    return buf;
}

std::optional<ImageBuffer> Image::decodeFile(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::streamsize fileSize = file.tellg();
    if (fileSize <= 0) {
        return std::nullopt;
    }
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(static_cast<size_t>(fileSize));
    if (!file.read(reinterpret_cast<char *>(buffer.data()), fileSize)) {
        return std::nullopt;
    }
    return decode(buffer.data(), buffer.size());
}

ImageBuffer Image::scale(const ImageBuffer &src, int targetWidth, int targetHeight)
{
    if (src.empty() || targetWidth <= 0 || targetHeight <= 0) {
        return {};
    }

    ImageBuffer dst;
    dst.width = targetWidth;
    dst.height = targetHeight;
    dst.channels = 4;
    dst.pixels.resize(static_cast<size_t>(targetWidth) * targetHeight * 4);

    stbir_resize_uint8_linear(
        src.pixels.data(), src.width, src.height, static_cast<int>(src.stride()),
        dst.pixels.data(), targetWidth, targetHeight, static_cast<int>(dst.stride()),
        STBIR_RGBA);

    return dst;
}

} // namespace Styx::Platform
