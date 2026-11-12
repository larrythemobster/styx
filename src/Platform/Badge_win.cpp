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
#include <string>
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
        const auto key = std::pair{int(count > 0), size};
        if (auto found = icons.find(key); found != icons.end())
            return found->second;
        auto image = Image::scale(base, size, size);
        if (count > 0) {
            const int diameter = std::max(5, int(std::lround(size * 0.34)));
            const int inset = std::max(1, size / 48);
            const int outer = diameter + inset * 2;
            const auto badge = render(diameter);
            for (int y = 0; y < outer; ++y) {
                for (int x = 0; x < outer; ++x) {
                    auto *pixel = image.pixels.data() + (static_cast<size_t>(y) * size + size - outer + x) * 4;
                    const double coverage = std::clamp(outer / 2.0 -
                        std::hypot(x + 0.5 - outer / 2.0, y + 0.5 - outer / 2.0) + 0.5, 0.0, 1.0);
                    blend(pixel, uint32_t(std::lround(coverage * 255)) << 24);
                    if (x >= inset && y >= inset && x < outer - inset && y < outer - inset)
                        blend(pixel, badge[static_cast<size_t>(y - inset) * diameter + x - inset]);
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
