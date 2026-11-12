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
        const auto key = std::pair{std::clamp(count, 0, 10), size};
        if (auto found = icons.find(key); found != icons.end())
            return found->second;
        auto image = Image::scale(base, size, size);
        if (count > 0) {
            const int diameter = size / 2;
            const int inset = std::max(1, size / 32);
            const int inner = diameter - inset * 2;
            const auto badge = render(label(count), inner);
            for (int y = 0; y < diameter; ++y) {
                for (int x = 0; x < diameter; ++x) {
                    auto *pixel = image.pixels.data() + (static_cast<size_t>(y) * size + size - diameter + x) * 4;
                    const double coverage = std::clamp(diameter / 2.0 -
                        std::hypot(x + 0.5 - diameter / 2.0, y + 0.5 - diameter / 2.0) + 0.5, 0.0, 1.0);
                    blend(pixel, uint32_t(std::lround(coverage * 255)) << 24);
                    if (x >= inset && y >= inset && x < diameter - inset && y < diameter - inset)
                        blend(pixel, badge[static_cast<size_t>(y - inset) * inner + x - inset]);
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

void drawLabelNative(std::vector<uint32_t> &pixels, std::string_view text, int size)
{
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap)
        return;
    std::fill_n(static_cast<uint32_t *>(bits), size_t(size) * size, 0);
    HDC dc = CreateCompatibleDC(nullptr);
    const auto oldBitmap = SelectObject(dc, bitmap);
    const std::wstring label(text.begin(), text.end());
    int height = std::max(1, int(std::lround(size * 0.72)));
    HFONT font = nullptr;
    HGDIOBJ oldFont = nullptr;
    for (;;) {
        font = CreateFontW(-height, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        if (!font)
            break;
        oldFont = SelectObject(dc, font);
        SIZE extent{};
        GetTextExtentPoint32W(dc, label.data(), int(label.size()), &extent);
        if (extent.cx <= size - 2 || height == 1)
            break;
        SelectObject(dc, oldFont);
        DeleteObject(font);
        font = nullptr;
        --height;
    }
    if (font) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT rectangle{0, 0, size, size};
        DrawTextW(dc, label.data(), int(label.size()), &rectangle, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        GdiFlush();
        const auto *coverage = static_cast<const uint32_t *>(bits);
        for (size_t index = 0; index < pixels.size(); ++index) {
            const unsigned alpha = coverage[index] & 255;
            if (!alpha)
                continue;
            const auto channel = [alpha](unsigned value) { return value + (255 - value) * alpha / 255; };
            const auto pixel = pixels[index];
            pixels[index] = (pixel & 0xFF000000u) | (channel((pixel >> 16) & 255) << 16) |
                (channel((pixel >> 8) & 255) << 8) | channel(pixel & 255);
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
