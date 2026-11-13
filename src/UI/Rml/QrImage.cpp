#include "QrImage.hpp"

#include "qrcodegen.hpp"

#include <cstdint>
#include <exception>
#include <vector>

namespace Styx::UI::Rml {

std::optional<Platform::ImageBuffer> qrImage(const std::string &text, int size)
{
    try {
        const auto qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::QUARTILE);
        const int modules = qr.getSize();
        constexpr int border = 4;
        const int side = modules + border * 2;
        Platform::ImageBuffer image{size, size, 4, std::vector<uint8_t>(static_cast<std::size_t>(size) * size * 4, 255)};
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                const int mx = x * side / size - border, my = y * side / size - border;
                if (mx >= 0 && my >= 0 && mx < modules && my < modules && qr.getModule(mx, my)) {
                    auto *pixel = image.pixels.data() + (static_cast<std::size_t>(y) * size + x) * 4;
                    pixel[0] = pixel[1] = pixel[2] = 0;
                }
            }
        return image;
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

} // namespace Styx::UI::Rml
