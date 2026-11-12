#include "Badge.hpp"
#include "Image.hpp"
#include "Native.hpp"

#include <windows.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace Styx::Platform::Badge {
namespace {

ITaskbarList3 *taskbar()
{
    static ITaskbarList3 *instance = [] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ITaskbarList3 *list = nullptr;
        if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&list))))
            return static_cast<ITaskbarList3 *>(nullptr);
        if (FAILED(list->HrInit())) {
            list->Release();
            return static_cast<ITaskbarList3 *>(nullptr);
        }
        return list;
    }();
    return instance;
}

HICON makeIcon(const ImageBuffer &image)
{
    const int size = image.width;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP colour = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!colour)
        return nullptr;
    auto *destination = static_cast<uint32_t *>(bits);
    const int maskStride = ((size + 15) / 16) * 2;
    std::vector<uint8_t> maskBits(static_cast<size_t>(maskStride) * size, 0);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const size_t index = static_cast<size_t>(y) * size + x;
            const auto *rgba = image.pixels.data() + index * 4;
            const unsigned alpha = rgba[3];
            destination[index] = (alpha << 24) | ((rgba[0] * alpha / 255) << 16) |
                ((rgba[1] * alpha / 255) << 8) | (rgba[2] * alpha / 255);
            if (!alpha)
                maskBits[static_cast<size_t>(y) * maskStride + x / 8] |= uint8_t(0x80 >> (x % 8));
        }
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());
    ICONINFO icon{};
    icon.fIcon = TRUE;
    icon.hbmMask = mask;
    icon.hbmColor = colour;
    HICON result = CreateIconIndirect(&icon);
    DeleteObject(mask);
    DeleteObject(colour);
    return result;
}

void blend(uint8_t *destination, uint32_t source)
{
    const unsigned alpha = source >> 24;
    const unsigned background = destination[3] * (255 - alpha) / 255;
    const unsigned outAlpha = alpha + background;
    if (!outAlpha)
        return;
    for (int channel = 0; channel < 3; ++channel) {
        const unsigned value = (source >> (16 - channel * 8)) & 255;
        destination[channel] = uint8_t((value * alpha + destination[channel] * background) / outAlpha);
    }
    destination[3] = uint8_t(outAlpha);
}

struct IconCache {
    ImageBuffer base;
    std::map<std::pair<int, int>, HICON> icons;
    HICON accessibleOverlay = nullptr;

    IconCache()
    {
        if (auto image = Image::decodeFile(Resources::path("app/styx.svg")))
            base = std::move(*image);
    }
    ~IconCache()
    {
        for (const auto &[key, icon] : icons)
            DestroyIcon(icon);
        if (accessibleOverlay)
            DestroyIcon(accessibleOverlay);
    }
    HICON overlay()
    {
        if (!accessibleOverlay)
            accessibleOverlay = makeIcon(ImageBuffer{16, 16, 4, std::vector<uint8_t>(16 * 16 * 4, 0)});
        return accessibleOverlay;
    }
    HICON get(int count, int size)
    {
        if (base.empty())
            return nullptr;
        const int shownCount = count <= 0 ? 0 : size < 24 ? 1 : std::min(count, 10);
        const auto key = std::pair{shownCount, size};
        if (auto found = icons.find(key); found != icons.end())
            return found->second;
        auto image = Image::scale(base, size, size);
        if (count > 0) {
            const std::string text = size >= 24 ? label(count) : "";
            const int height = std::max(5, int(std::lround(size * 0.34)));
            const int width = text.size() > 1 ? int(std::lround(height * 1.45)) : height;
            const int inset = std::max(1, size / 48);
            const int outerWidth = width + inset * 2;
            const int outerHeight = height + inset * 2;
            const auto badge = render(text, width, height);
            for (int y = 0; y < outerHeight; ++y) {
                for (int x = 0; x < outerWidth; ++x) {
                    auto *pixel = image.pixels.data() + (static_cast<size_t>(y) * size + size - outerWidth + x) * 4;
                    const double dx = std::max(std::abs(x + 0.5 - outerWidth / 2.0) -
                        (outerWidth - outerHeight) / 2.0, 0.0);
                    const double coverage = std::clamp(outerHeight / 2.0 -
                        std::hypot(dx, y + 0.5 - outerHeight / 2.0) + 0.5, 0.0, 1.0);
                    blend(pixel, uint32_t(std::lround(coverage * 255)) << 24);
                    if (x >= inset && y >= inset && x < outerWidth - inset && y < outerHeight - inset)
                        blend(pixel, badge[static_cast<size_t>(y - inset) * width + x - inset]);
                }
            }
        }
        HICON icon = makeIcon(image);
        if (icon)
            icons.emplace(key, icon);
        return icon;
    }
};

} // namespace

void drawLabelNative(std::vector<uint32_t> &pixels, std::string_view text, int width, int height)
{
    constexpr int scale = 4;
    const int canvasWidth = width * scale;
    const int canvasHeight = height * scale;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = canvasWidth;
    info.bmiHeader.biHeight = -canvasHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap)
        return;
    HDC dc = CreateCompatibleDC(nullptr);
    const auto oldBitmap = SelectObject(dc, bitmap);
    const std::wstring label(text.begin(), text.end());
    auto *coverage = static_cast<uint32_t *>(bits);
    int fontHeight = std::max(1, int(std::lround(canvasHeight * 0.9)));
    HFONT font = nullptr;
    HGDIOBJ oldFont = nullptr;
    RECT ink{};
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    for (;;) {
        font = CreateFontW(-fontHeight, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        if (!font)
            break;
        oldFont = SelectObject(dc, font);
        SIZE extent{};
        GetTextExtentPoint32W(dc, label.data(), int(label.size()), &extent);
        std::fill_n(coverage, size_t(canvasWidth) * canvasHeight, 0);
        ink = {canvasWidth, canvasHeight, 0, 0};
        if (extent.cx < canvasWidth) {
            TextOutW(dc, 0, 0, label.data(), int(label.size()));
            GdiFlush();
            for (int y = 0; y < canvasHeight; ++y) {
                for (int x = 0; x < canvasWidth; ++x) {
                    if (!(coverage[size_t(y) * canvasWidth + x] & 255))
                        continue;
                    ink.left = std::min(ink.left, LONG(x));
                    ink.top = std::min(ink.top, LONG(y));
                    ink.right = std::max(ink.right, LONG(x + 1));
                    ink.bottom = std::max(ink.bottom, LONG(y + 1));
                }
            }
        }
        const double radius = std::min(canvasWidth, canvasHeight) / 2.0;
        const double dx = std::max((ink.right - ink.left) / 2.0 - (canvasWidth / 2.0 - radius), 0.0);
        const double dy = std::max((ink.bottom - ink.top) / 2.0 - (canvasHeight / 2.0 - radius), 0.0);
        if ((ink.right > ink.left && ink.bottom > ink.top &&
             ink.bottom - ink.top <= canvasHeight * 0.64 &&
             std::hypot(dx, dy) <= radius - scale) || fontHeight == 1)
            break;
        SelectObject(dc, oldFont);
        DeleteObject(font);
        font = nullptr;
        --fontHeight;
    }
    if (font) {
        // Centre visible ink rather than the font's line box and side bearings.
        const int left = (canvasWidth - (ink.right - ink.left)) / 2;
        const int top = (canvasHeight - (ink.bottom - ink.top)) / 2;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                unsigned sum = 0;
                for (int sy = 0; sy < scale; ++sy) {
                    const int sourceY = y * scale + sy + ink.top - top;
                    if (sourceY < ink.top || sourceY >= ink.bottom)
                        continue;
                    for (int sx = 0; sx < scale; ++sx) {
                        const int sourceX = x * scale + sx + ink.left - left;
                        if (sourceX >= ink.left && sourceX < ink.right)
                            sum += coverage[size_t(sourceY) * canvasWidth + sourceX] & 255;
                    }
                }
                const unsigned alpha = (sum + scale * scale / 2) / (scale * scale);
                if (!alpha)
                    continue;
                auto &pixel = pixels[size_t(y) * width + x];
                const auto channel = [alpha](unsigned value) { return value + (255 - value) * alpha / 255; };
                pixel = (pixel & 0xFF000000u) | (channel((pixel >> 16) & 255) << 16) |
                    (channel((pixel >> 8) & 255) << 8) | channel(pixel & 255);
            }
        }
        SelectObject(dc, oldFont);
        DeleteObject(font);
    }
    SelectObject(dc, oldBitmap);
    DeleteDC(dc);
    DeleteObject(bitmap);
}

void setCount(void *window, int count)
{
    if (!window)
        return;
    auto handle = static_cast<HWND>(window);
    static IconCache cache;
    // Draw the visible badge once, in the top-right corner. A transparent shell
    // overlay retains the unread description for assistive technology.
    if (auto *list = taskbar()) {
        const std::wstring description = count > 0 ? std::to_wstring(count) +
            (count == 1 ? L" unread mention" : L" unread mentions") : L"";
        list->SetOverlayIcon(handle, count > 0 ? cache.overlay() : nullptr, description.c_str());
    }
    const UINT dpi = GetDpiForWindow(handle);
    const int largeSize = GetSystemMetricsForDpi(SM_CXICON, dpi);
    const int smallSize = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    if (HICON icon = cache.get(count, largeSize))
        SendMessageW(handle, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
    if (HICON icon = cache.get(count, smallSize))
        SendMessageW(handle, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
}

} // namespace Styx::Platform::Badge
