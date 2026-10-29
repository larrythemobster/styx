#include "ImageUpload.hpp"

#include "Platform/Base64.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <sstream>
#include <system_error>

#include <avif/avif.h>
#include <openssl/evp.h>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

namespace Styx::UI::ImageUpload {
namespace {

std::filesystem::path filePath(const std::string &path)
{
    return std::filesystem::u8path(path);
}

bool readFile(const std::string &path, std::string &bytes)
{
    std::ifstream file(filePath(path), std::ios::binary);
    if (!file)
        return false;
    bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !file.bad();
}

uint32_t be32(const unsigned char *p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint32_t le32(const unsigned char *p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

std::string_view mimeType(std::string_view bytes)
{
    if (bytes.starts_with("\x89PNG\r\n\x1a\n")) return "image/png";
    if (bytes.starts_with("\xff\xd8\xff")) return "image/jpeg";
    if (bytes.starts_with("GIF87a") || bytes.starts_with("GIF89a")) return "image/gif";
    if (bytes.size() >= 12 && bytes.substr(0, 4) == "RIFF" && bytes.substr(8, 4) == "WEBP") return "image/webp";
    if (bytes.size() >= 12 && bytes.substr(4, 4) == "ftyp") {
        const auto brands = bytes.substr(8, std::min<size_t>(bytes.size() - 8, 32));
        if (brands.find("avif") != std::string_view::npos || brands.find("avis") != std::string_view::npos)
            return "image/avif";
    }
    if (bytes.starts_with("BM")) return "image/bmp";
    if (bytes.find("<svg") != std::string_view::npos) return "image/svg+xml";
    return "application/octet-stream";
}

bool gifAnimated(std::string_view bytes)
{
    const auto *p = reinterpret_cast<const unsigned char *>(bytes.data());
    const size_t size = bytes.size();
    if (size < 13) return false;
    size_t pos = 13;
    if (p[10] & 0x80) pos += 3u << ((p[10] & 7) + 1);
    int frames = 0;
    while (pos < size) {
        const unsigned marker = p[pos++];
        if (marker == 0x3b) break;
        if (marker == 0x2c) {
            if (size - pos < 9) return false;
            const unsigned flags = p[pos + 8];
            pos += 9;
            if (flags & 0x80) pos += 3u << ((flags & 7) + 1);
            if (pos >= size) return false;
            ++pos; // LZW minimum code size
            if (++frames > 1) return true;
        } else if (marker == 0x21) {
            if (pos >= size) return false;
            ++pos; // extension label
        } else {
            return false;
        }
        while (pos < size) {
            const size_t length = p[pos++];
            if (!length) break;
            if (length > size - pos) return false;
            pos += length;
        }
    }
    return false;
}

bool isAnimation(std::string_view bytes, std::string_view mime)
{
    const auto *p = reinterpret_cast<const unsigned char *>(bytes.data());
    if (mime == "image/gif") return gifAnimated(bytes);
    if (mime == "image/png") {
        size_t pos = 8;
        while (pos <= bytes.size() && bytes.size() - pos >= 12) {
            const size_t length = be32(p + pos);
            if (length > bytes.size() - pos - 12) break;
            if (bytes.substr(pos + 4, 4) == "acTL" && length >= 8)
                return be32(p + pos + 8) > 1;
            pos += length + 12;
        }
    } else if (mime == "image/webp") {
        size_t pos = 12;
        int frames = 0;
        while (pos <= bytes.size() && bytes.size() - pos >= 8) {
            const size_t length = le32(p + pos + 4);
            if (length > bytes.size() - pos - 8) break;
            if (bytes.substr(pos, 4) == "ANMF" && ++frames > 1) return true;
            const size_t padded = length + (length & 1);
            if (padded > bytes.size() - pos - 8) break;
            pos += 8 + padded;
        }
    } else if (mime == "image/avif") {
        std::unique_ptr<avifDecoder, decltype(&avifDecoderDestroy)> decoder(avifDecoderCreate(), avifDecoderDestroy);
        if (decoder && avifDecoderSetIOMemory(decoder.get(), p, bytes.size()) == AVIF_RESULT_OK &&
            avifDecoderParse(decoder.get()) == AVIF_RESULT_OK)
            return decoder->imageCount > 1;
    }
    return false;
}

std::string sizeLabel(int64_t bytes)
{
    if (bytes < 1024) return std::to_string(bytes) + " bytes";
    static constexpr const char *units[] = {"KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    do { value /= 1024; } while (value >= 1024 && ++unit < 3);
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << value << ' ' << units[unit];
    return out.str();
}

} // namespace

std::string fileFilter()
{
    return "Images (*.jpg *.jpeg *.jfif *.png *.gif *.webp *.avif)";
}

std::string dataUri(std::string_view bytes, std::string_view mime)
{
    return "data:" + std::string(mime) + ";base64," + Platform::toBase64(bytes);
}

std::string md5Hex(std::string_view bytes)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), digest, &length, EVP_md5(), nullptr) != 1)
        return {};
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(length * 2);
    for (unsigned int i = 0; i < length; ++i) {
        result.push_back(hex[digest[i] >> 4]);
        result.push_back(hex[digest[i] & 15]);
    }
    return result;
}

std::string encodePng(const Platform::ImageBuffer &image)
{
    if (image.empty()) return {};
    std::string result;
    auto write = [](void *context, void *data, int size) {
        static_cast<std::string *>(context)->append(static_cast<const char *>(data), static_cast<size_t>(size));
    };
    if (!stbi_write_png_to_func(write, &result, image.width, image.height, 4, image.pixels.data(), static_cast<int>(image.stride())))
        return {};
    return result;
}

PreparedImage prepareCropped(const std::string &path, int maxWidth, int maxHeight)
{
    PreparedImage prepared;
    std::string bytes;
    if (!readFile(path, bytes)) {
        prepared.error = "Unable to process image";
        return prepared;
    }
    prepared.originalMd5 = md5Hex(bytes);
    const auto mime = mimeType(bytes);
    if (isAnimation(bytes, mime)) {
        prepared.animated = true;
        prepared.dataUri = dataUri(bytes, mime);
        return prepared;
    }
    auto image = Platform::Image::decode(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size());
    if (!image || image->empty() || maxWidth <= 0 || maxHeight <= 0) {
        if (mime == "image/avif" && maxWidth > 0 && maxHeight > 0)
            prepared.dataUri = dataUri(bytes, mime);
        else
            prepared.error = "Unable to process image";
        return prepared;
    }
    const double aspect = static_cast<double>(maxWidth) / maxHeight;
    int width = image->width, height = image->height;
    if (static_cast<double>(width) / height > aspect)
        width = std::max(1, static_cast<int>(std::round(height * aspect)));
    else
        height = std::max(1, static_cast<int>(std::round(width / aspect)));
    const int x = (image->width - 1) / 2 - (width - 1) / 2;
    const int y = (image->height - 1) / 2 - (height - 1) / 2;
    Platform::ImageBuffer cropped;
    cropped.width = width;
    cropped.height = height;
    cropped.pixels.resize(static_cast<size_t>(width) * height * 4);
    for (int row = 0; row < height; ++row)
        std::memcpy(cropped.pixels.data() + row * cropped.stride(),
                    image->pixels.data() + static_cast<size_t>(y + row) * image->stride() + static_cast<size_t>(x) * 4,
                    cropped.stride());
    if (width > maxWidth || height > maxHeight) {
        const double scale = std::min(static_cast<double>(maxWidth) / width, static_cast<double>(maxHeight) / height);
        cropped = Platform::Image::scale(cropped, std::max(1, static_cast<int>(std::round(width * scale))),
                                        std::max(1, static_cast<int>(std::round(height * scale))));
    }
    const auto png = encodePng(cropped);
    if (png.empty()) prepared.error = "Unable to process image";
    else prepared.dataUri = dataUri(png, "image/png");
    return prepared;
}

PreparedImage prepareRaw(const std::string &path, int64_t maxBytes)
{
    PreparedImage prepared;
    std::error_code ec;
    const auto size = std::filesystem::file_size(filePath(path), ec);
    if (!ec && (maxBytes < 0 || size > static_cast<uintmax_t>(maxBytes))) {
        prepared.error = "Max file size is " + sizeLabel(maxBytes) + " please.";
        return prepared;
    }
    std::string bytes;
    if (!readFile(path, bytes)) {
        prepared.error = "Unable to process image";
        return prepared;
    }
    prepared.originalMd5 = md5Hex(bytes);
    const auto mime = mimeType(bytes);
    prepared.animated = isAnimation(bytes, mime);
    prepared.dataUri = dataUri(bytes, mime);
    return prepared;
}

} // namespace Styx::UI::ImageUpload
