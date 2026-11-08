#include "Badge.hpp"

#include "Utf8.hpp"

#include <windows.h>
#include <shobjidl.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace Styx::Platform::Badge {
namespace {

ITaskbarList3 *taskbar()
{
    // Created once on the main thread and kept for the life of the process.
    static ITaskbarList3 *instance = [] {
        // The main thread may already have COM set up; either way it stays set up.
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

// Builds an icon from straight-alpha 0xAARRGGBB pixels, which is the layout a 32-bit
// top-down DIB uses.
HICON makeIcon(const std::vector<std::uint32_t> &pixels, int size)
{
    BITMAPINFO info = {};
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
    std::memcpy(bits, pixels.data(), pixels.size() * sizeof(std::uint32_t));
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO icon = {};
    icon.fIcon = TRUE;
    icon.hbmMask = mask;
    icon.hbmColor = colour;
    HICON result = CreateIconIndirect(&icon);
    DeleteObject(mask);
    DeleteObject(colour);
    return result;
}

} // namespace

void setCount(void *window, int count)
{
    ITaskbarList3 *list = taskbar();
    if (!list || !window)
        return;
    auto *handle = static_cast<HWND>(window);
    const std::string text = label(count);
    if (text.empty()) {
        list->SetOverlayIcon(handle, nullptr, L"");
        return;
    }
    const int size = GetSystemMetrics(SM_CXSMICON);
    HICON icon = makeIcon(render(text, size), size);
    if (!icon)
        return;
    const std::wstring description = toUtf16(text + (count == 1 ? " unread mention" : " unread mentions"));
    list->SetOverlayIcon(handle, icon, description.c_str());
    // The taskbar keeps its own copy of the overlay.
    DestroyIcon(icon);
}

} // namespace Styx::Platform::Badge
