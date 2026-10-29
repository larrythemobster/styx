#include "Image.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

#include <avif/avif.h>
#include <lunasvg.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <webp/decode.h>
#include <webp/demux.h>

namespace Styx::Platform {

std::optional<ImageBuffer> Image::decode(const uint8_t *data, size_t size)
{
    if (!data || size == 0) {
        return std::nullopt;
    }
    std::string_view bytes(reinterpret_cast<const char *>(data), size);
    if (bytes.substr(0, std::min(size, size_t(1024))).find("<svg") != std::string_view::npos) {
        auto document = lunasvg::Document::loadFromData(reinterpret_cast<const char *>(data), size);
        if (document) {
            auto bitmap = document->renderToBitmap();
            if (!bitmap.isNull() && bitmap.width() > 0 && bitmap.height() > 0) {
                ImageBuffer result;
                result.width = static_cast<int>(bitmap.width());
                result.height = static_cast<int>(bitmap.height());
                result.pixels.resize(static_cast<size_t>(result.width) * result.height * 4);
                for (int y = 0; y < result.height; ++y) {
                    const uint8_t *source = bitmap.data() + static_cast<size_t>(y) * bitmap.stride();
                    uint8_t *target = result.pixels.data() + static_cast<size_t>(y) * result.stride();
                    for (int x = 0; x < result.width; ++x, source += 4, target += 4) {
                        const unsigned alpha = source[3];
                        target[0] = alpha ? static_cast<uint8_t>(std::min(255u, (unsigned(source[2]) * 255 + alpha / 2) / alpha)) : 0;
                        target[1] = alpha ? static_cast<uint8_t>(std::min(255u, (unsigned(source[1]) * 255 + alpha / 2) / alpha)) : 0;
                        target[2] = alpha ? static_cast<uint8_t>(std::min(255u, (unsigned(source[0]) * 255 + alpha / 2) / alpha)) : 0;
                        target[3] = static_cast<uint8_t>(alpha);
                    }
                }
                return result;
            }
        }
        return std::nullopt;
    }

    if (size >= 12 && bytes.substr(4, 4) == "ftyp" &&
        (bytes.substr(8, 4) == "avif" || bytes.substr(8, 4) == "avis" ||
         bytes.find("avif", 12) != std::string_view::npos || bytes.find("avis", 12) != std::string_view::npos)) {
        using Decoder = std::unique_ptr<avifDecoder, decltype(&avifDecoderDestroy)>;
        using AvifImage = std::unique_ptr<avifImage, decltype(&avifImageDestroy)>;
        Decoder decoder(avifDecoderCreate(), avifDecoderDestroy);
        AvifImage image(avifImageCreateEmpty(), avifImageDestroy);
        if (!decoder || !image || avifDecoderReadMemory(decoder.get(), image.get(), data, size) != AVIF_RESULT_OK ||
            image->width == 0 || image->height == 0 ||
            image->width > std::numeric_limits<int>::max() / 4 || image->height > std::numeric_limits<int>::max() ||
            static_cast<size_t>(image->width) > std::numeric_limits<size_t>::max() / 4 / image->height)
            return std::nullopt;
        ImageBuffer result;
        result.width = static_cast<int>(image->width);
        result.height = static_cast<int>(image->height);
        result.pixels.resize(static_cast<size_t>(result.width) * result.height * 4);
        avifRGBImage rgb;
        avifRGBImageSetDefaults(&rgb, image.get());
        rgb.format = AVIF_RGB_FORMAT_RGBA;
        rgb.depth = 8;
        rgb.pixels = result.pixels.data();
        rgb.rowBytes = static_cast<uint32_t>(result.stride());
        if (avifImageYUVToRGB(image.get(), &rgb) != AVIF_RESULT_OK)
        return result;
    }

    if (bytes.substr(0, 4) == "RIFF" && bytes.substr(8, 4) == "WEBP") {
        int webpWidth = 0, webpHeight = 0;
        if (WebPGetInfo(data, size, &webpWidth, &webpHeight) && webpWidth > 0 && webpHeight > 0) {
            ImageBuffer webp;
            webp.width = webpWidth;
            webp.height = webpHeight;
            webp.channels = 4;
            webp.pixels.resize(static_cast<size_t>(webpWidth) * webpHeight * 4);
            if (WebPDecodeRGBAInto(data, size, webp.pixels.data(), webp.pixels.size(), webpWidth * 4))
                return webp;
            }
        }
    }
#endif

    if (size > static_cast<size_t>(std::numeric_limits<int>::max()))
        return std::nullopt;
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

std::optional<Size> Image::sizeFile(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return std::nullopt;
    auto length = file.tellg();
    if (length <= 0 || static_cast<uint64_t>(length) > std::numeric_limits<int>::max()) return std::nullopt;
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(length))) return std::nullopt;
    int width = 0, height = 0, components = 0;
    if (stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &components) ||
        WebPGetInfo(bytes.data(), bytes.size(), &width, &height))
        return Size{width, height};
    std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    if (text.substr(0, std::min(text.size(), size_t(1024))).find("<svg") != std::string_view::npos) {
        auto document = lunasvg::Document::loadFromData(text.data(), text.size());
        if (document && document->width() > 0 && document->height() > 0)
            return Size{static_cast<int>(document->width()), static_cast<int>(document->height())};
    }
    using Decoder = std::unique_ptr<avifDecoder, decltype(&avifDecoderDestroy)>;
    Decoder decoder(avifDecoderCreate(), avifDecoderDestroy);
    if (decoder && avifDecoderSetIOMemory(decoder.get(), bytes.data(), bytes.size()) == AVIF_RESULT_OK &&
        avifDecoderParse(decoder.get()) == AVIF_RESULT_OK && decoder->image &&
        decoder->image->width <= std::numeric_limits<int>::max() && decoder->image->height <= std::numeric_limits<int>::max())
        return Size{static_cast<int>(decoder->image->width), static_cast<int>(decoder->image->height)};
    return std::nullopt;
}

std::string Image::encodePng(const ImageBuffer &image)
{
    if (image.empty() || image.width > std::numeric_limits<int>::max() / 4 ||
        image.pixels.size() < image.stride() * static_cast<size_t>(image.height))
        return {};
    std::string result;
    auto append = [](void *context, void *data, int size) {
        static_cast<std::string *>(context)->append(static_cast<const char *>(data), static_cast<size_t>(size));
    };
    if (!stbi_write_png_to_func(append, &result, image.width, image.height, 4, image.pixels.data(), static_cast<int>(image.stride())))
        return {};
    return result;
}

bool Image::isAnimated(const uint8_t *data, size_t size)
{
    if (!data || size < 12) return false;
    std::string_view bytes(reinterpret_cast<const char *>(data), size);
    if (bytes.substr(0, 6) == "GIF87a" || bytes.substr(0, 6) == "GIF89a") {
        if (size < 13) return false;
        size_t pos = 13;
        if (data[10] & 0x80) pos += 3u << ((data[10] & 7) + 1);
        int frames = 0;
        auto skipBlocks = [&]() {
            while (pos < size) {
                size_t n = data[pos++];
                if (!n) return true;
                if (n > size - pos) return false;
                pos += n;
            }
            return false;
        };
        while (pos < size) {
            uint8_t marker = data[pos++];
            if (marker == 0x3b) break;
            if (marker == 0x21) {
                if (pos >= size) return false;
                ++pos;
                if (!skipBlocks()) return false;
            } else if (marker == 0x2c) {
                if (size - pos < 9) return false;
                uint8_t packed = data[pos + 8];
                pos += 9;
                if (packed & 0x80) pos += 3u << ((packed & 7) + 1);
                if (pos >= size) return false;
                ++pos;
                if (!skipBlocks()) return false;
                if (++frames > 1) return true;
            } else return false;
        }
        return false;
    }
    if (bytes.substr(0, 8) == std::string_view("\x89PNG\r\n\x1a\n", 8)) {
        size_t pos = 8;
        while (pos <= size - 12) {
            uint32_t length = (uint32_t(data[pos]) << 24) | (uint32_t(data[pos+1]) << 16) |
                              (uint32_t(data[pos+2]) << 8) | data[pos+3];
            if (length > size - pos - 12) return false;
            if (bytes.substr(pos + 4, 4) == "acTL" && length >= 8)
                return data[pos+8] || data[pos+9] || data[pos+10] || data[pos+11] > 1;
            pos += static_cast<size_t>(length) + 12;
        }
        return false;
    }
    if (bytes.substr(0, 4) == "RIFF" && bytes.substr(8, 4) == "WEBP") {
        WebPData source{data, size};
        WebPDemuxer *demux = WebPDemux(&source);
        if (!demux) return false;
        bool animated = WebPDemuxGetI(demux, WEBP_FF_FRAME_COUNT) > 1;
        WebPDemuxDelete(demux);
        return animated;
    }
    if (bytes.substr(4, 4) == "ftyp") {
        using Decoder = std::unique_ptr<avifDecoder, decltype(&avifDecoderDestroy)>;
        Decoder decoder(avifDecoderCreate(), avifDecoderDestroy);
        return decoder && avifDecoderSetIOMemory(decoder.get(), data, size) == AVIF_RESULT_OK &&
               avifDecoderParse(decoder.get()) == AVIF_RESULT_OK && decoder->imageCount > 1;
    }
    return false;
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
