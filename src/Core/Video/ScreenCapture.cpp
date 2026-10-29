// Ported from ouwou/acheron upstream/video-streams 71b4907
#include "Core/Video/ScreenCapture.hpp"

#include "Core/Video/CardScaler.hpp"
#include "Core/Video/LinuxCapture.hpp"
#include "Core/Video/PictureConverter.hpp"
#include "Platform/Image.hpp"
#include "Platform/Settings.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cwchar>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
}
#endif

namespace Styx::Core::Video {

namespace {

constexpr char CAPTURE_METHOD_KEY[] = "video/capture_method";
constexpr char AUTO_METHOD[] = "auto";
constexpr char GRAPHICS_CAPTURE_METHOD[] = "graphics_capture";
constexpr char DESKTOP_DUPLICATION_METHOD[] = "desktop_duplication";
constexpr char PORTAL_METHOD[] = "portal";
constexpr char X11_METHOD[] = "x11";

} // namespace

FrameSize encodeSizeFor(FrameSize source, int maxHeight)
{
    FrameSize size = source;
    const FrameSize bound{ maxHeight * 16 / 9, maxHeight };
    if (maxHeight > 0 && (source.width > bound.width || source.height > bound.height)) {
        const auto widthAtBoundHeight = static_cast<int64_t>(bound.height) * source.width / source.height;
        if (widthAtBoundHeight <= bound.width)
            size = { static_cast<int>(widthAtBoundHeight), bound.height };
        else
            size = { bound.width, static_cast<int>(static_cast<int64_t>(bound.width) * source.height / source.width) };
    }
    return { std::max(2, size.width & ~1), std::max(2, size.height & ~1) };
}

CaptureMethod captureMethod()
{
    const std::string stored = Platform::Settings::instance().getString(CAPTURE_METHOD_KEY, AUTO_METHOD);
    if (stored == GRAPHICS_CAPTURE_METHOD)
        return CaptureMethod::GraphicsCapture;
    if (stored == DESKTOP_DUPLICATION_METHOD)
        return CaptureMethod::DesktopDuplication;
    if (stored == PORTAL_METHOD)
        return CaptureMethod::Portal;
    if (stored == X11_METHOD)
        return CaptureMethod::X11;
    return CaptureMethod::Auto;
}

void setCaptureMethod(CaptureMethod method)
{
    const char *stored = AUTO_METHOD;
    if (method == CaptureMethod::GraphicsCapture)
        stored = GRAPHICS_CAPTURE_METHOD;
    else if (method == CaptureMethod::DesktopDuplication)
        stored = DESKTOP_DUPLICATION_METHOD;
    else if (method == CaptureMethod::Portal)
        stored = PORTAL_METHOD;
    else if (method == CaptureMethod::X11)
        stored = X11_METHOD;
    Platform::Settings::instance().setString(CAPTURE_METHOD_KEY, stored);
}

bool stillPictureIsDue(std::chrono::milliseconds msSinceLastPicture, bool keyFrameWaitedFor, int framerate)
{
    return msSinceLastPicture >= std::chrono::milliseconds(STILL_PICTURE_INTERVAL_MS) || (keyFrameWaitedFor && msSinceLastPicture >= std::chrono::milliseconds(1000 / std::max(1, framerate)));
}

bool windowIsCovered(const std::vector<StackedWindow> &topDown, std::size_t target)
{
    if (target >= topDown.size())
        return true;
    const StackedWindow &window = topDown[target];
    if (window.bounds.empty() || window.minimised || window.cloaked)
        return true;

    for (std::size_t i = 0; i < target; i++) {
        const StackedWindow &above = topDown[i];
        if (!above.visible || above.minimised)
            continue;
        if (above.livePreview)
            return true;
        if (!above.overlay && !above.cloaked && !above.tabProxy && above.bounds.intersects(window.bounds))
            return true;
    }
    return false;
}

#ifdef _WIN32

namespace {

using Clock = std::chrono::steady_clock;

constexpr char SCREEN_ID_PREFIX[] = "screen:";
constexpr char WINDOW_ID_PREFIX[] = "window:";
constexpr int HD_HEIGHT = 720;
constexpr int STALLED_WAIT_MS = 50;
constexpr int NOTHING_NEW_WAIT_MS = 25;
constexpr FrameSize SMALLEST_WINDOW_PICTURE{ 160, 120 };
constexpr int REACH_PAST_SCREEN = 15;
constexpr int EMPTY_PICTURES_FOR_MS = 3000;
constexpr int THUMBNAIL_DRAWING_LIMIT_MS = 500;

constexpr DWORD FRAME_BOUNDS_ATTRIBUTE = 9;
constexpr DWORD CLOAKED_ATTRIBUTE = 14;
constexpr UINT RENDER_FULL_CONTENT = 2;

std::atomic<bool> graphicsCaptureRuledOut{ false };

struct FrameDeleter
{
    void operator()(AVFrame *frame) const { av_frame_free(&frame); }
};
using FramePointer = std::unique_ptr<AVFrame, FrameDeleter>;

class ThreadScaling
{
public:
    explicit ThreadScaling(DPI_AWARENESS_CONTEXT wanted)
    {
        if (setAwareness && wanted)
            previous = setAwareness(wanted);
    }
    ~ThreadScaling()
    {
        if (previous)
            setAwareness(previous);
    }

    ThreadScaling(const ThreadScaling &) = delete;
    ThreadScaling &operator=(const ThreadScaling &) = delete;

    [[nodiscard]] static DPI_AWARENESS_CONTEXT of(HWND window) { return windowAwareness ? windowAwareness(window) : nullptr; }

private:
    using SetAwareness = DPI_AWARENESS_CONTEXT(WINAPI *)(DPI_AWARENESS_CONTEXT);
    using WindowAwareness = DPI_AWARENESS_CONTEXT(WINAPI *)(HWND);
    static inline const SetAwareness setAwareness = reinterpret_cast<SetAwareness>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext")));
    static inline const WindowAwareness windowAwareness = reinterpret_cast<WindowAwareness>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetWindowDpiAwarenessContext")));
    DPI_AWARENESS_CONTEXT previous = nullptr;
};

class UnscaledCoordinates : public ThreadScaling
{
public:
    UnscaledCoordinates() : ThreadScaling(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) {}
};

struct Monitor
{
    int adapter = 0;
    int output = 0;
    HMONITOR handle = nullptr;
    CaptureRect geometry;
    bool onItsSide = false;
    bool turned = false;
};

std::vector<Monitor> attachedMonitors()
{
    using CreateFactory = HRESULT(WINAPI *)(REFIID, void **);
    static const auto createFactory = reinterpret_cast<CreateFactory>(reinterpret_cast<void *>(GetProcAddress(LoadLibraryW(L"dxgi.dll"), "CreateDXGIFactory1")));

    std::vector<Monitor> found;
    IDXGIFactory1 *factory = nullptr;
    if (!createFactory || FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
        return found;

    IDXGIAdapter *adapter = nullptr;
    for (UINT adapterIndex = 0; SUCCEEDED(factory->EnumAdapters(adapterIndex, &adapter)); adapterIndex++) {
        IDXGIOutput *output = nullptr;
        for (UINT outputIndex = 0; SUCCEEDED(adapter->EnumOutputs(outputIndex, &output)); outputIndex++) {
            DXGI_OUTPUT_DESC description;
            DEVMODEW mode = {};
            mode.dmSize = sizeof(mode);
            if (SUCCEEDED(output->GetDesc(&description)) && description.AttachedToDesktop && EnumDisplaySettingsW(description.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) {
                Monitor monitor;
                monitor.adapter = static_cast<int>(adapterIndex);
                monitor.output = static_cast<int>(outputIndex);
                monitor.handle = description.Monitor;
                monitor.geometry = CaptureRect{ mode.dmPosition.x, mode.dmPosition.y, static_cast<int>(mode.dmPelsWidth), static_cast<int>(mode.dmPelsHeight) };
                monitor.onItsSide = description.Rotation == DXGI_MODE_ROTATION_ROTATE90 || description.Rotation == DXGI_MODE_ROTATION_ROTATE270;
                monitor.turned = monitor.onItsSide || description.Rotation == DXGI_MODE_ROTATION_ROTATE180;
                found.push_back(monitor);
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return found;
}

const Monitor *numberedMonitor(const std::vector<Monitor> &monitors, const std::string &sourceId)
{
    constexpr std::string_view prefix = SCREEN_ID_PREFIX;
    if (!sourceId.starts_with(prefix))
        return nullptr;
    try {
        const size_t number = std::stoul(sourceId.substr(prefix.size()));
        return number < monitors.size() ? &monitors[number] : nullptr;
    } catch (...) {
        return nullptr;
    }
}

std::string describeError(int error)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error, text, sizeof(text));
    return text;
}

FrameSize evenSize(FrameSize size)
{
    return { std::max(2, size.width & ~1), std::max(2, size.height & ~1) };
}

CaptureRect fromNative(const RECT &rect)
{
    return { rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top };
}

HWND windowOf(const std::string &sourceId)
{
    constexpr std::string_view prefix = WINDOW_ID_PREFIX;
    if (!sourceId.starts_with(prefix))
        return nullptr;
    try {
        const auto handle = std::stoull(sourceId.substr(prefix.size()));
        return reinterpret_cast<HWND>(static_cast<uintptr_t>(handle));
    } catch (...) {
        return nullptr;
    }
}

HRESULT windowAttribute(HWND window, DWORD attribute, void *value, DWORD size)
{
    using GetAttribute = HRESULT(WINAPI *)(HWND, DWORD, PVOID, DWORD);
    static const auto getAttribute = reinterpret_cast<GetAttribute>(reinterpret_cast<void *>(GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmGetWindowAttribute")));
    return getAttribute ? getAttribute(window, attribute, value, size) : E_INVALIDARG;
}

bool isCloaked(HWND window)
{
    DWORD cloak = 0;
    const HRESULT asked = windowAttribute(window, CLOAKED_ATTRIBUTE, &cloak, sizeof(cloak));
    return SUCCEEDED(asked) ? cloak == 1 || cloak == 2 : asked != E_INVALIDARG;
}

CaptureRect visibleBounds(HWND window)
{
    RECT bounds = {};
    if (FAILED(windowAttribute(window, FRAME_BOUNDS_ATTRIBUTE, &bounds, sizeof(bounds))) || IsRectEmpty(&bounds)) {
        if (!GetWindowRect(window, &bounds))
            return {};
    }
    return fromNative(bounds);
}

CaptureRect clientOnScreen(HWND window)
{
    RECT client = {};
    POINT corner = {};
    if (GetClientRect(window, &client) && ClientToScreen(window, &corner))
        return { corner.x, corner.y, client.right, client.bottom };
    return GetWindowRect(window, &client) ? fromNative(client) : CaptureRect{};
}

bool hasClass(HWND window, const wchar_t *name)
{
    wchar_t found[64] = {};
    return GetClassNameW(window, found, 64) > 0 && wcscmp(found, name) == 0;
}

struct WindowStack
{
    HWND target = nullptr;
    HWND foreground = nullptr;
    CaptureRect targetClient;
    std::vector<StackedWindow> topDown;
    bool reachedTarget = false;
    std::vector<HWND> ordinary;
};

BOOL CALLBACK stackDownToTarget(HWND window, LPARAM parameter)
{
    auto *stack = reinterpret_cast<WindowStack *>(parameter);
    stack->ordinary.push_back(window);
    if (stack->reachedTarget)
        return TRUE;

    StackedWindow each;
    each.visible = IsWindowVisible(window) != FALSE;
    each.minimised = IsIconic(window) != FALSE;
    if (window == stack->target) {
        each.bounds = stack->targetClient;
        each.cloaked = isCloaked(window);
        stack->topDown.push_back(each);
        stack->reachedTarget = true;
        return TRUE;
    }

    if (each.visible && !each.minimised) {
        const LONG_PTR extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
        each.bounds = visibleBounds(window);
        each.cloaked = isCloaked(window);
        each.overlay = (extended & (WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)) != 0 && window != stack->foreground;
        each.tabProxy = hasClass(window, L"Windows.Internal.Shell.TabProxyWindow");
        each.livePreview = (extended & WS_EX_TOPMOST) && hasClass(window, L"LivePreview");
    }
    stack->topDown.push_back(each);
    return TRUE;
}

bool shellSurfaceCovers(std::vector<HWND> ordinary, const CaptureRect &client)
{
    std::sort(ordinary.begin(), ordinary.end());
    for (HWND window = FindWindowExW(nullptr, nullptr, nullptr, nullptr); window; window = FindWindowExW(nullptr, window, nullptr, nullptr)) {
        if (std::binary_search(ordinary.begin(), ordinary.end(), window) || !IsWindowVisible(window) || IsIconic(window))
            continue;

        DWORD cloak = 0;
        if (FAILED(windowAttribute(window, CLOAKED_ATTRIBUTE, &cloak, sizeof(cloak))) || cloak != 0)
            continue;
        if (visibleBounds(window).intersects(client))
            return true;
    }
    return false;
}

enum class ShellSurfaces {
    LookedFor,
    LeftForLater,
};

bool isCoveredOnScreen(HWND window, const CaptureRect &client, ShellSurfaces shellSurfaces)
{
    WindowStack stack;
    stack.target = window;
    stack.foreground = GetForegroundWindow();
    stack.targetClient = client;
    EnumWindows(stackDownToTarget, reinterpret_cast<LPARAM>(&stack));
    if (!stack.reachedTarget || windowIsCovered(stack.topDown, stack.topDown.size() - 1))
        return true;
    return shellSurfaces == ShellSurfaces::LookedFor && shellSurfaceCovers(std::move(stack.ordinary), client);
}

bool isShareable(HWND window)
{
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process == GetCurrentProcessId() || !IsWindowVisible(window) || window == GetShellWindow() || isCloaked(window))
        return false;

    const LONG_PTR extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((extended & WS_EX_TOOLWINDOW) || ((extended & WS_EX_NOACTIVATE) && !(extended & WS_EX_APPWINDOW)))
        return false;
    return IsIconic(window) || !visibleBounds(window).empty();
}

uint32_t processShownIn(HWND window)
{
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (hasClass(window, L"ApplicationFrameWindow")) {
        DWORD inner = 0;
        const HWND core = FindWindowExW(window, nullptr, L"Windows.UI.Core.CoreWindow", nullptr);
        if (core && GetWindowThreadProcessId(core, &inner) && inner)
            return inner;
    }
    return process;
}

BOOL CALLBACK listShareable(HWND window, LPARAM list)
{
    wchar_t title[512] = {};
    if (!isShareable(window) || GetWindowTextW(window, title, 512) <= 0)
        return TRUE;

    char utf8[2048] = {};
    WideCharToMultiByte(CP_UTF8, 0, title, -1, utf8, sizeof(utf8), nullptr, nullptr);

    CaptureSource source;
    source.id = std::string(WINDOW_ID_PREFIX) + std::to_string(reinterpret_cast<uintptr_t>(window));
    source.kind = CaptureSource::Kind::Window;
    source.name = utf8;
    source.processId = processShownIn(window);
    WINDOWPLACEMENT placement = {};
    placement.length = sizeof(placement);
    if (!IsIconic(window))
        source.geometry = visibleBounds(window);
    else if (GetWindowPlacement(window, &placement))
        source.geometry = fromNative(placement.rcNormalPosition);
    reinterpret_cast<std::vector<CaptureSource> *>(list)->push_back(source);
    return TRUE;
}

class Bitmap
{
public:
    explicit Bitmap(FrameSize size) : extent(size), memory(CreateCompatibleDC(nullptr))
    {
        BITMAPINFO layout = {};
        layout.bmiHeader.biSize = sizeof(layout.bmiHeader);
        layout.bmiHeader.biWidth = size.width;
        layout.bmiHeader.biHeight = -size.height;
        layout.bmiHeader.biPlanes = 1;
        layout.bmiHeader.biBitCount = 32;
        layout.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(memory, &layout, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap)
            previous = SelectObject(memory, bitmap);
    }

    ~Bitmap()
    {
        if (bitmap) {
            SelectObject(memory, previous);
            DeleteObject(bitmap);
        }
        DeleteDC(memory);
    }

    Bitmap(const Bitmap &) = delete;
    Bitmap &operator=(const Bitmap &) = delete;

    [[nodiscard]] bool usable() const { return bitmap != nullptr; }
    [[nodiscard]] HDC context() const { return memory; }
    [[nodiscard]] FrameSize size() const { return extent; }
    [[nodiscard]] const uint32_t *row(int y) const { return static_cast<const uint32_t *>(bits) + static_cast<size_t>(y) * extent.width; }

private:
    FrameSize extent;
    HDC memory;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    void *bits = nullptr;
};

FrameSize scaledKeepAspectRatio(FrameSize size, FrameSize bound)
{
    if (size.width <= 0 || size.height <= 0 || bound.width <= 0 || bound.height <= 0)
        return {};
    const auto wAtBoundH = static_cast<int64_t>(bound.height) * size.width / size.height;
    if (wAtBoundH <= bound.width)
        return { static_cast<int>(wAtBoundH), bound.height };
    return { bound.width, static_cast<int>(static_cast<int64_t>(bound.width) * size.height / size.width) };
}

Platform::ImageBuffer shrunk(HDC from, const CaptureRect &area, FrameSize bound)
{
    FrameSize size = (area.width > bound.width || area.height > bound.height)
        ? scaledKeepAspectRatio(area.size(), bound)
        : area.size();
    size.width = std::max(1, size.width);
    size.height = std::max(1, size.height);

    const Bitmap reduced(size);
    if (!reduced.usable())
        return {};

    SetStretchBltMode(reduced.context(), HALFTONE);
    SetBrushOrgEx(reduced.context(), 0, 0, nullptr);
    if (!StretchBlt(reduced.context(), 0, 0, size.width, size.height, from, area.left(), area.top(), area.width, area.height, SRCCOPY))
        return {};
    GdiFlush();

    Platform::ImageBuffer image;
    image.width = size.width;
    image.height = size.height;
    image.channels = 4;
    image.pixels.resize(static_cast<size_t>(size.width) * size.height * 4);
    for (int y = 0; y < size.height; y++) {
        const uint32_t *source = reduced.row(y);
        uint8_t *to = image.pixels.data() + static_cast<size_t>(y) * size.width * 4;
        for (int x = 0; x < size.width; x++) {
            const uint32_t pixel = source[x];
            to[x * 4 + 0] = static_cast<uint8_t>((pixel >> 16) & 0xFF);
            to[x * 4 + 1] = static_cast<uint8_t>((pixel >> 8) & 0xFF);
            to[x * 4 + 2] = static_cast<uint8_t>(pixel & 0xFF);
            to[x * 4 + 3] = 0xFF;
        }
    }
    return image;
}

class WindowDrawer
{
public:
    static std::shared_ptr<WindowDrawer> start(HWND window, bool insideOnly)
    {
        auto drawer = std::make_shared<WindowDrawer>(window, insideOnly);
        std::thread([drawer] { drawer->run(); }).detach();
        return drawer;
    }

    WindowDrawer(HWND window, bool insideOnly) : window(window), insideOnly(insideOnly) {}

    void abandon()
    {
        {
            const std::lock_guard lock(mutex);
            abandoned = true;
        }
        changed.notify_all();
    }

    bool draw(std::chrono::milliseconds limit)
    {
        std::unique_lock lock(mutex);
        if (drawing)
            return false;

        const FrameSize size = sizeAsTheWindowMeasures();
        if (size.empty())
            return false;
        if (!bitmap || bitmap->size() != size)
            bitmap = std::make_unique<Bitmap>(size);
        if (!bitmap->usable())
            return false;

        drawing = true;
        changed.notify_all();
        changed.wait_for(lock, limit, [this] { return !drawing; });
        return !drawing && drew;
    }

    [[nodiscard]] const Bitmap &drawn() const { return *bitmap; }

    void drawPointer(const CaptureRect &client)
    {
        CURSORINFO cursor = {};
        cursor.cbSize = sizeof(cursor);
        if (!GetCursorInfo(&cursor) || !(cursor.flags & CURSOR_SHOWING))
            return;
        const CapturePoint at{ cursor.ptScreenPos.x, cursor.ptScreenPos.y };
        const HWND under = WindowFromPoint(cursor.ptScreenPos);
        if (cursor.hCursor != hotspotOf) {
            ICONINFO icon = {};
            hotspot = {};
            if (GetIconInfo(cursor.hCursor, &icon)) {
                hotspot = CapturePoint{ static_cast<int>(icon.xHotspot), static_cast<int>(icon.yHotspot) };
                DeleteObject(icon.hbmMask);
                if (icon.hbmColor)
                    DeleteObject(icon.hbmColor);
            }
            hotspotOf = cursor.hCursor;
        }
        const FrameSize size = bitmap->size();
        DrawIconEx(bitmap->context(), (at.x - client.left()) * size.width / client.width - hotspot.x, (at.y - client.top()) * size.height / client.height - hotspot.y, cursor.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
        GdiFlush();
    }

private:
    FrameSize sizeAsTheWindowMeasures() const
    {
        const ThreadScaling asTheWindow(ThreadScaling::of(window));
        RECT area = {};
        if (!(insideOnly ? GetClientRect(window, &area) : GetWindowRect(window, &area)))
            return {};
        return fromNative(area).size();
    }

    void run()
    {
        std::unique_lock lock(mutex);
        for (;;) {
            changed.wait(lock, [this] { return drawing || abandoned; });
            if (abandoned)
                return;

            lock.unlock();
            bool printed = false;
            {
                const ThreadScaling asTheWindow(ThreadScaling::of(window));
                printed = PrintWindow(window, bitmap->context(), RENDER_FULL_CONTENT | (insideOnly ? PW_CLIENTONLY : 0)) != FALSE;
                GdiFlush();
            }
            lock.lock();
            drew = printed;
            drawing = false;
            changed.notify_all();
        }
    }

    const HWND window;
    const bool insideOnly;
    std::mutex mutex;
    std::condition_variable changed;
    std::unique_ptr<Bitmap> bitmap;
    bool drawing = false;
    bool drew = false;
    bool abandoned = false;
    HCURSOR hotspotOf = nullptr;
    CapturePoint hotspot;
};

Platform::ImageBuffer windowThumbnail(HWND window, FrameSize bound)
{
    RECT whole = {};
    if (!window || IsIconic(window) || !GetWindowRect(window, &whole) || IsRectEmpty(&whole))
        return {};

    const auto drawer = WindowDrawer::start(window, false);
    const bool drew = drawer->draw(std::chrono::milliseconds(THUMBNAIL_DRAWING_LIMIT_MS));
    drawer->abandon();
    if (!drew)
        return {};

    const CaptureRect outer = fromNative(whole);
    const CaptureRect shown = visibleBounds(window).translated(-outer.left(), -outer.top()) & CaptureRect{ 0, 0, outer.width, outer.height };
    const FrameSize drawn = drawer->drawn().size();
    if (outer.width <= 0 || outer.height <= 0)
        return {};
    const CaptureRect inBitmap{
        shown.left() * drawn.width / outer.width,
        shown.top() * drawn.height / outer.height,
        shown.width * drawn.width / outer.width,
        shown.height * drawn.height / outer.height
    };
    return inBitmap.empty() ? Platform::ImageBuffer{} : shrunk(drawer->drawn().context(), inBitmap, bound);
}

bool prepareBgra(AVFrame *frame, FrameSize size)
{
    if (frame->buf[0] && frame->format == AV_PIX_FMT_BGRA && frame->width == size.width && frame->height == size.height)
        return av_frame_make_writable(frame) >= 0;

    av_frame_unref(frame);
    frame->format = AV_PIX_FMT_BGRA;
    frame->width = size.width;
    frame->height = size.height;
    return av_frame_get_buffer(frame, 0) >= 0;
}

bool showsItself(HWND window)
{
    return IsWindow(window) && IsWindowVisible(window) && !IsIconic(window);
}

FrameSize windowPictureSize(FrameSize client)
{
    const FrameSize even = evenSize(client);
    return { std::max(even.width, SMALLEST_WINDOW_PICTURE.width), std::max(even.height, SMALLEST_WINDOW_PICTURE.height) };
}

int readBack(const AVFrame *onCard, AVFrame *memory)
{
    const int format = reinterpret_cast<const AVHWFramesContext *>(onCard->hw_frames_ctx->data)->sw_format;
    const bool fits = memory->buf[0] && memory->format == format && memory->width == onCard->width && memory->height == onCard->height && av_frame_make_writable(memory) >= 0;
    if (!fits) {
        av_frame_unref(memory);
        memory->format = format;
        memory->width = onCard->width;
        memory->height = onCard->height;
        const int made = av_frame_get_buffer(memory, 0);
        if (made < 0)
            return made;
    }
    return av_hwframe_transfer_data(memory, onCard, 0);
}

bool isEmptyPicture(const AVFrame *onCard, AVFrame *scratch)
{
    constexpr int POINTS_ACROSS = 4;
    if (!prepareBgra(scratch, { onCard->width, onCard->height }) || av_hwframe_transfer_data(scratch, onCard, 0) < 0)
        return false;

    for (int row = 0; row < POINTS_ACROSS; row++) {
        const uint8_t *line = scratch->data[0] + (scratch->height * (2 * row + 1) / (2 * POINTS_ACROSS)) * scratch->linesize[0];
        for (int column = 0; column < POINTS_ACROSS; column++) {
            if (line[(scratch->width * (2 * column + 1) / (2 * POINTS_ACROSS)) * 4 + 3] != 0)
                return false;
        }
    }
    return true;
}

bool showsTopLeftOf(const Bitmap &from, const AVFrame *bgra)
{
    const int columns = std::min(from.size().width, bgra->width);
    const int rows = std::min(from.size().height, bgra->height);
    for (int y = 0; y < rows; y++) {
        if (memcmp(bgra->data[0] + static_cast<size_t>(y) * bgra->linesize[0], from.row(y), static_cast<size_t>(columns) * 4) != 0)
            return false;
    }
    return true;
}

void copyTopLeft(const Bitmap &from, AVFrame *bgra)
{
    const int columns = std::min(from.size().width, bgra->width);
    for (int y = 0; y < bgra->height; y++) {
        uint8_t *to = bgra->data[0] + static_cast<size_t>(y) * bgra->linesize[0];
        const int copied = y < from.size().height ? columns : 0;
        if (copied > 0)
            memcpy(to, from.row(y), static_cast<size_t>(copied) * 4);
        memset(to + static_cast<size_t>(copied) * 4, 0, static_cast<size_t>(bgra->width - copied) * 4);
    }
}

class GraphicsCaptureSession
{
public:
    enum class Taken {
        Picture,
        Nothing,
        Ended,
    };

    static std::shared_ptr<GraphicsCaptureSession> start(const std::string &chain, const std::string &adapter, int *error)
    {
        AVBufferRef *device = nullptr;
        *error = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, adapter.empty() ? nullptr : adapter.c_str(), nullptr, 0);
        if (*error < 0)
            return nullptr;

        auto session = std::make_shared<GraphicsCaptureSession>();
        *error = assemble(chain, device, &session->graph, &session->sink);
        av_buffer_unref(&device);
        if (*error < 0)
            return nullptr;
        std::thread([session] { session->run(); }).detach();
        return session;
    }

    ~GraphicsCaptureSession() { avfilter_graph_free(&graph); }

    void abandon()
    {
        const std::lock_guard lock(mutex);
        abandoned = true;
    }

    int endedWith()
    {
        const std::lock_guard lock(mutex);
        return ended;
    }

    Taken take(AVFrame *into, Clock::time_point notBefore, Clock::time_point giveUpAt)
    {
        std::unique_lock lock(mutex);
        for (;;) {
            if (ended)
                return Taken::Ended;
            const auto now = Clock::now();
            if (fresh && now >= notBefore) {
                av_frame_unref(into);
                av_frame_move_ref(into, latest.get());
                fresh = false;
                return Taken::Picture;
            }
            if (now >= giveUpAt)
                return Taken::Nothing;
            arrived.wait_until(lock, fresh ? notBefore : giveUpAt);
        }
    }

private:
    void run()
    {
        const FramePointer picture(av_frame_alloc());
        for (;;) {
            const int got = av_buffersink_get_frame(sink, picture.get());
            const std::lock_guard lock(mutex);
            if (abandoned)
                break;
            if (got < 0) {
                ended = got;
                break;
            }
            av_frame_unref(latest.get());
            av_frame_move_ref(latest.get(), picture.get());
            fresh = true;
            arrived.notify_all();
        }
        arrived.notify_all();
        avfilter_graph_free(&graph);
    }

    AVFilterGraph *graph = nullptr;
    AVFilterContext *sink = nullptr;
    std::mutex mutex;
    std::condition_variable arrived;
    FramePointer latest{ av_frame_alloc() };
    bool fresh = false;
    bool abandoned = false;
    int ended = 0;
};

bool graphicsCaptureUsable()
{
    static const bool initialises = [] {
        const AVFilter *filter = avfilter_get_by_name("gfxcapture");
        if (!filter)
            return false;
        OSVERSIONINFOEXW version{};
        version.dwOSVersionInfoSize = sizeof(version);
        version.dwMajorVersion = 10;
        version.dwBuildNumber = 18362;
        DWORDLONG mask = VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL);
        mask = VerSetConditionMask(mask, VER_BUILDNUMBER, VER_GREATER_EQUAL);
        if (!VerifyVersionInfoW(&version, VER_MAJORVERSION | VER_BUILDNUMBER, mask))
            return false;

        AVFilterGraph *graph = avfilter_graph_alloc();
        AVFilterContext *made = nullptr;
        const bool initialised = avfilter_graph_create_filter(&made, filter, "probe", nullptr, nullptr, graph) >= 0;
        avfilter_graph_free(&graph);
        return initialised;
    }();
    return initialises && !graphicsCaptureRuledOut;
}

bool usesGraphicsCapture(CaptureMethod method, bool forWindow)
{
    const bool preferred = method == CaptureMethod::GraphicsCapture || (method == CaptureMethod::Auto && forWindow);
    return graphicsCaptureUsable() && (preferred || !screenCaptureAvailable());
}

std::chrono::microseconds pictureInterval(int framerate)
{
    return std::chrono::microseconds(1'000'000 / std::max(1, framerate));
}

bool waitedForRetry(Clock::time_point retryAt)
{
    const auto now = Clock::now();
    if (now >= retryAt)
        return false;
    std::this_thread::sleep_for(std::min<Clock::duration>(retryAt - now, std::chrono::milliseconds(STALLED_WAIT_MS)));
    return true;
}

AVBufferRef *cardPictures(AVBufferRef *device, FrameSize size)
{
    AVBufferRef *pool = av_hwframe_ctx_alloc(device);
    if (!pool)
        return nullptr;
    auto *frames = reinterpret_cast<AVHWFramesContext *>(pool->data);
    frames->format = AV_PIX_FMT_D3D11;
    frames->sw_format = AV_PIX_FMT_BGRA;
    frames->width = size.width;
    frames->height = size.height;
    static_cast<AVD3D11VAFramesContext *>(frames->hwctx)->BindFlags = D3D11_BIND_RENDER_TARGET;
    if (av_hwframe_ctx_init(pool) < 0)
        av_buffer_unref(&pool);
    return pool;
}

AVD3D11VADeviceContext *deviceOf(const AVFrame *card)
{
    return static_cast<AVD3D11VADeviceContext *>(reinterpret_cast<AVHWFramesContext *>(card->hw_frames_ctx->data)->device_ctx->hwctx);
}

ID3D11Resource *textureOf(const AVFrame *card)
{
    return reinterpret_cast<ID3D11Resource *>(card->data[0]);
}

UINT sliceOf(const AVFrame *card)
{
    return static_cast<UINT>(reinterpret_cast<uintptr_t>(card->data[1]));
}

bool isSamePictureOnCard(const AVFrame *last, const AVFrame *now)
{
    return last->buf[0] && now->hw_frames_ctx && last->data[0] == now->data[0] && last->data[1] == now->data[1];
}

void keepReferenceTo(AVFrame *last, const AVFrame *now)
{
    av_frame_unref(last);
    av_frame_ref(last, now);
}

CaptureRect scaledPart(const CaptureRect &part, FrameSize from, FrameSize to)
{
    const int cornerX = part.left() * to.width / from.width;
    const int cornerY = part.top() * to.height / from.height;
    const int oppX = (part.left() + part.width) * to.width / from.width;
    const int oppY = (part.top() + part.height) * to.height / from.height;
    return { cornerX, cornerY, oppX - cornerX, oppY - cornerY };
}

void markColours(AVFrame *picture, bool bt601)
{
    picture->colorspace = bt601 ? AVCOL_SPC_SMPTE170M : AVCOL_SPC_BT709;
    picture->color_range = AVCOL_RANGE_MPEG;
}

void clearToBlack(AVFrame *card)
{
    AVD3D11VADeviceContext *device = deviceOf(card);
    ID3D11RenderTargetView *view = nullptr;
    if (FAILED(device->device->CreateRenderTargetView(textureOf(card), nullptr, &view)))
        return;
    const float black[4] = { 0, 0, 0, 1 };
    device->lock(device->lock_ctx);
    device->device_context->ClearRenderTargetView(view, black);
    device->unlock(device->lock_ctx);
    view->Release();
}

void copyOnCard(const AVFrame *from, const CaptureRect &area, AVFrame *to, const CapturePoint &at)
{
    AVD3D11VADeviceContext *device = deviceOf(to);
    const D3D11_BOX box = { static_cast<UINT>(area.left()), static_cast<UINT>(area.top()), 0, static_cast<UINT>(area.right()), static_cast<UINT>(area.bottom()), 1 };
    device->lock(device->lock_ctx);
    device->device_context->CopySubresourceRegion(textureOf(to), sliceOf(to), static_cast<UINT>(at.x), static_cast<UINT>(at.y), 0, textureOf(from), sliceOf(from), &box);
    device->unlock(device->lock_ctx);
}

bool cutOntoCard(const AVFrame *from, const CaptureRect &area, AVBufferRef *pool, const CapturePoint &at, AVFrame *to)
{
    if (av_hwframe_get_buffer(pool, to, 0) < 0)
        return false;
    if (area.size() != FrameSize{ to->width, to->height })
        clearToBlack(to);
    copyOnCard(from, area, to, at);
    return true;
}

struct CutOut
{
    CaptureRect onScreen;
    CaptureRect inPicture;

    bool operator==(const CutOut &other) const = default;
};

enum class Drawing {
    Failed,
    Unchanged,
    Drawn,
};

} // namespace

#endif // _WIN32

bool screenCaptureAvailable()
{
#ifdef _WIN32
    static const bool available = avfilter_get_by_name("ddagrab") != nullptr;
    return available;
#else
    return captureMethodUsable(CaptureMethod::Auto);
#endif
}

bool windowCaptureAvailable()
{
#ifdef _WIN32
    return graphicsCaptureUsable() || screenCaptureAvailable();
#else
    return captureMethodUsable(CaptureMethod::Auto);
#endif
}

bool captureMethodUsable(CaptureMethod method)
{
#ifdef _WIN32
    switch (method) {
    case CaptureMethod::GraphicsCapture:
        return graphicsCaptureUsable();
    case CaptureMethod::DesktopDuplication:
        return screenCaptureAvailable();
    case CaptureMethod::Portal:
    case CaptureMethod::X11:
        return false;
    case CaptureMethod::Auto:
        break;
    }
    return screenCaptureAvailable() || graphicsCaptureUsable();
#else
    if (method == CaptureMethod::X11)
        return X11Capture::usable();
    if (method == CaptureMethod::Portal)
        return PortalCapture::usable();
    return method == CaptureMethod::Auto && (X11Capture::usable() || PortalCapture::usable());
#endif
}

CaptureSources captureSources(CaptureMethod method)
{
#ifdef _WIN32
    CaptureSources sources;
    if (!captureMethodUsable(method))
        return sources;

    const std::vector<Monitor> monitors = attachedMonitors();
    for (size_t i = 0; i < monitors.size(); i++) {
        CaptureSource source;
        source.id = std::string(SCREEN_ID_PREFIX) + std::to_string(i);
        source.name = "Screen " + std::to_string(i + 1);
        source.geometry = monitors[i].geometry;
        sources.push_back(source);
    }
    return sources;
#else
    if (method == CaptureMethod::Auto)
        method = X11Capture::usable() ? CaptureMethod::X11 : CaptureMethod::Portal;
    return method == CaptureMethod::X11 ? X11Capture::screens() : CaptureSources{};
#endif
}

CaptureSources captureWindows(CaptureMethod method)
{
#ifdef _WIN32
    CaptureSources windows;
    if (!captureMethodUsable(method))
        return windows;

    const UnscaledCoordinates unscaled;
    EnumWindows(listShareable, reinterpret_cast<LPARAM>(&windows));
    return windows;
#else
    return method == CaptureMethod::X11 || (method == CaptureMethod::Auto && X11Capture::usable()) ? X11Capture::windows() : CaptureSources{};
#endif
}

Platform::ImageBuffer captureThumbnail(const CaptureSource &source, FrameSize bound)
{
    if (bound.empty())
        return {};

#ifdef _WIN32
    const UnscaledCoordinates unscaled;
    if (source.kind == CaptureSource::Kind::Window)
        return windowThumbnail(windowOf(source.id), bound);
    if (source.geometry.empty())
        return {};

    const HDC screen = GetDC(nullptr);
    const Platform::ImageBuffer thumbnail = shrunk(screen, source.geometry, bound);
    ReleaseDC(nullptr, screen);
    return thumbnail;
#else
    return {};
#endif
}

void restoreIfMinimised(const CaptureSource &window)
{
#ifdef _WIN32
    const HWND handle = windowOf(window.id);
    if (handle && IsIconic(handle))
        ShowWindowAsync(handle, SW_RESTORE);
#else
    X11Capture::restoreIfMinimised(window);
#endif
}

FrameSize encodeSizeOf(const CaptureSource &source, int maxHeight)
{
#ifdef _WIN32
    if (source.kind == CaptureSource::Kind::Screen)
        return source.geometry.empty() ? FrameSize{} : encodeSizeFor(evenSize(source.geometry.size()), maxHeight);

    const UnscaledCoordinates unscaled;
    const HWND window = windowOf(source.id);
    const FrameSize inside = window && showsItself(window) ? clientOnScreen(window).size() : FrameSize{};
    return inside.empty() ? FrameSize{} : encodeSizeFor(windowPictureSize(inside), maxHeight);
#else
    if (source.kind == CaptureSource::Kind::Screen)
        return source.geometry.empty() ? FrameSize{} : encodeSizeFor(evenSize(source.geometry.size()), maxHeight);
    return source.geometry.empty() ? FrameSize{} : encodeSizeFor({source.geometry.width, source.geometry.height}, maxHeight);
#endif
}

bool isChosenByTheDesktop(const CaptureSource &source)
{
    return source.id.starts_with("pipewire:");
}

bool isHandedOverByTheDesktop(const CaptureSource &source)
{
    return isChosenByTheDesktop(source);
}

void releaseSource(const CaptureSource &source)
{
    if (isHandedOverByTheDesktop(source))
        PortalCapture::release(source.id);
}

struct ScreenCapture::Impl
{
    Request request;
    bool deliversChanges = true;
    std::string name;

#ifdef _WIN32
    enum class Path {
        Duplication,
        GraphicsCapture,
        WindowByDuplication,
    };

    Path path = Path::Duplication;
    AVFilterGraph *graph = nullptr;
    AVFilterContext *sink = nullptr;
    bool everDelivered = false;
    bool cardScalingFailed = false;
    std::optional<Clock::time_point> failingSince;
    Clock::time_point retryAt{};
    bool closed = false;
    bool failed = false;

    HWND window = nullptr;
    FrameSize clientSize{};
    Clock::time_point sizeChangedAt{};
    FrameSize pictureSize{};
    Clock::time_point pictureDue{};

    std::shared_ptr<GraphicsCaptureSession> session;
    Clock::time_point remakeAt{};
    Clock::time_point emptyPicturesUntil{};

    HMONITOR screenHandle = nullptr;
    std::optional<Monitor> screen;
    bool screenChanged = true;
    AVBufferRef *device = nullptr;
    int deviceAdapter = -1;
    AVBufferRef *pool = nullptr;
    std::shared_ptr<WindowDrawer> drawer;
    uint64_t cutOutPictures = 0;
    uint64_t drawnPictures = 0;
    std::optional<bool> lastWasCutOut;
    const char *drawnBecause = "";

    PictureConverter converter;
    PictureConverter rescaler;
    CardScaler scaler;
    FramePointer captured{ av_frame_alloc() };
    FramePointer wholeScreen{ av_frame_alloc() };
    FramePointer bgra{ av_frame_alloc() };
    FramePointer asDrawn{ av_frame_alloc() };

    const char *wentOn = "";
    bool skipsUnchanged = true;
    FramePointer lastOfScreen{ av_frame_alloc() };
    CutOut lastCut;
    FrameSize drawingDelivered{};
    FrameSize drawingInBgra{};

    ~Impl()
    {
        tearDown();
        av_buffer_unref(&pool);
        av_buffer_unref(&device);
    }

    void configure(const Request &wanted)
    {
        tearDown();
        request = wanted;
        failingSince.reset();
        retryAt = {};
        closed = false;
        failed = false;
        cardScalingFailed = false;

        window = windowOf(request.sourceId);
        skipsUnchanged = Platform::Settings::instance().getBool("video/skip_unchanged_pictures", true);
        if (usesGraphicsCapture(request.method, window != nullptr))
            path = Path::GraphicsCapture;
        else
            path = window ? Path::WindowByDuplication : Path::Duplication;
    }

    Status watchWindow()
    {
        if (!IsWindow(window)) {
            tearDown();
            closed = true;
            return Status::Closed;
        }

        DWORD affinity = 0;
        const bool keptOutOfCaptures = path == Path::WindowByDuplication && GetWindowDisplayAffinity(window, &affinity) && affinity != 0;
        const CaptureRect client = IsIconic(window) || !IsWindowVisible(window) || keptOutOfCaptures ? CaptureRect{} : clientOnScreen(window);
        if (client.empty()) {
            if (session && session->endedWith() != 0)
                session.reset();
            dropDuplication();
            clientSize = {};
            emptyPicturesUntil = Clock::now() + std::chrono::milliseconds(EMPTY_PICTURES_FOR_MS);
            failingSince.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(STALLED_WAIT_MS));
            return Status::Hidden;
        }

        const auto now = Clock::now();
        if (client.size() != clientSize) {
            clientSize = client.size();
            sizeChangedAt = now;
        }
        const FrameSize wanted = windowPictureSize(client.size());
        const bool held = now - sizeChangedAt >= std::chrono::milliseconds(RESIZE_SETTLE_MS);
        if (wanted != pictureSize && (pictureSize.empty() || held)) {
            pictureSize = wanted;
            if (session)
                session->abandon();
            session.reset();
            av_buffer_unref(&pool);
            av_frame_unref(lastOfScreen.get());
            drawingDelivered = {};
        }
        return Status::Frame;
    }

    void delivered()
    {
        everDelivered = true;
        failingSince.reset();
    }

    [[nodiscard]] bool scalesOnCard() const
    {
        return request.keepOnCard && !cardScalingFailed && CardScaler::usable();
    }

    bool scaleOnCard(const AVFrame *from, const CaptureRect &area, FrameSize size, const CaptureRect &placed, AVFrame *into)
    {
        const CardScaler::Output output = { size, request.bt601Only || size.height < HD_HEIGHT, request.rgbOnCard };
        if (scaler.scale(from, area, output, placed, into))
            return true;
        cardScalingFailed = true;
        return false;
    }

    Status readDuplication(AVFrame *into, bool picturesWanted, std::string &failureText)
    {
        if (!graph) {
            if (waitedForRetry(retryAt))
                return Status::Stalled;
            const int built = buildDuplication();
            if (built < 0)
                return afterFailure(built, failureText);
        }

        AVFrame *cap = captured.get();
        const int got = av_buffersink_get_frame(sink, cap);
        if (got == AVERROR(EAGAIN))
            return Status::NoFrame;
        if (got < 0)
            return afterFailure(got, failureText);

        if (!picturesWanted || (skipsUnchanged && isSamePictureOnCard(lastOfScreen.get(), cap))) {
            av_frame_unref(cap);
            return Status::NoFrame;
        }
        keepReferenceTo(lastOfScreen.get(), cap);
        return deliverFromCard(cap, into, failureText);
    }

    Status readGraphicsCapture(AVFrame *into, bool picturesWanted, std::string &failureText)
    {
        if (!session) {
            if (waitedForRetry(retryAt))
                return Status::Stalled;
            if (waitedForRetry(remakeAt))
                return Status::NoFrame;
            const int built = buildGraphicsCapture();
            if (built < 0 && path != Path::GraphicsCapture)
                return Status::Stalled;
            if (built < 0)
                return afterFailure(built, failureText);
        }

        const auto interval = pictureInterval(request.framerate);
        const auto giveUpAt = std::max(Clock::now(), pictureDue) + std::chrono::milliseconds(NOTHING_NEW_WAIT_MS);
        const GraphicsCaptureSession::Taken taken = session->take(captured.get(), pictureDue - interval / 4, giveUpAt);
        if (taken == GraphicsCaptureSession::Taken::Ended) {
            const int ended = session->endedWith();
            if (ended != AVERROR_EOF || !window)
                return afterFailure(ended, failureText);
            session.reset();
            remakeAt = Clock::now() + std::chrono::milliseconds(RETRY_INTERVAL_MS);
            return Status::NoFrame;
        }
        if (taken == GraphicsCaptureSession::Taken::Nothing)
            return Status::NoFrame;
        pictureDue = std::max(pictureDue + interval, Clock::now() + interval / 2);

        bool undrawn = window && !showsItself(window);
        if (window && !undrawn && Clock::now() < emptyPicturesUntil) {
            undrawn = isEmptyPicture(captured.get(), bgra.get());
            if (!undrawn) {
                emptyPicturesUntil = {};
                av_frame_unref(bgra.get());
            }
        }
        if (undrawn) {
            av_frame_unref(captured.get());
            return Status::NoFrame;
        }

        return deliverFromCard(captured.get(), into, failureText);
    }

    Status deliverFromCard(AVFrame *cap, AVFrame *into, std::string &failureText)
    {
        const FrameSize ownSize{ cap->width, cap->height };
        const FrameSize size = encodeSizeFor(ownSize, request.maxHeight);
        const bool bt601 = request.bt601Only || size.height < HD_HEIGHT;
        if (request.keepOnCard && request.rgbOnCard && size == ownSize) {
            av_frame_move_ref(into, cap);
            markColours(into, false);
        } else if (scalesOnCard() && scaleOnCard(cap, CaptureRect{ 0, 0, cap->width, cap->height }, size, CaptureRect{ 0, 0, size.width, size.height }, into)) {
            av_frame_unref(cap);
        } else {
            int converted = readBack(cap, bgra.get());
            av_frame_unref(cap);
            if (converted >= 0)
                converted = converter.convert(bgra.get(), size, request.softwareFormat, bt601, into);
            if (converted < 0)
                return afterFailure(converted, failureText);
            markColours(into, bt601);
        }

        delivered();
        return Status::Frame;
    }

    Status readWindowByDuplication(AVFrame *into, std::string &failureText)
    {
        std::this_thread::sleep_until(pictureDue);
        pictureDue = std::max(pictureDue + pictureInterval(request.framerate), Clock::now());

        const CaptureRect client = clientOnScreen(window);
        const bool screenDuplicated = duplicateScreenOf(client);
        const FrameSize size = encodeSizeFor(pictureSize, request.maxHeight);
        const bool asCaptured = pool && request.keepOnCard && request.rgbOnCard && size == pictureSize;
        const bool scaledOnCard = pool && !asCaptured && scalesOnCard();
        const bool staysOnCard = asCaptured || scaledOnCard;

        AVFrame *whole = wholeScreen.get();
        AVFrame *card = captured.get();
        AVFrame *bg = bgra.get();
        CutOut cut;
        drawnBecause = "its screen cannot be duplicated";
        const bool cutOut = screenDuplicated && cutOutOfScreen(client, &cut);
        if (cutOut && skipsUnchanged && isSamePictureOnCard(lastOfScreen.get(), whole) && cut == lastCut) {
            av_frame_unref(whole);
            return Status::NoFrame;
        }
        if (!cutOut && drawnByItself(client) != Drawing::Drawn)
            return Status::NoFrame;

        bool ready = true;
        if (cutOut && scaledOnCard) {
            ready = scaleOnCard(whole, cut.onScreen, size, scaledPart(cut.inPicture, pictureSize, size), into);
        } else {
            if (cutOut)
                ready = cutOntoCard(whole, cut.onScreen, pool, cut.inPicture.topLeft(), card) && (staysOnCard || (prepareBgra(bg, pictureSize) && av_hwframe_transfer_data(bg, card, 0) >= 0));
            else if (staysOnCard)
                ready = av_hwframe_get_buffer(pool, card, 0) >= 0 && av_hwframe_transfer_data(card, bg, 0) >= 0;

            const bool bt601 = !staysOnCard && (request.bt601Only || size.height < HD_HEIGHT);
            if (ready && scaledOnCard)
                ready = scaleOnCard(card, CaptureRect{ 0, 0, card->width, card->height }, size, CaptureRect{ 0, 0, size.width, size.height }, into);
            else if (ready && staysOnCard)
                av_frame_move_ref(into, card);
            else if (ready)
                ready = converter.convert(bg, size, request.softwareFormat, bt601, into) >= 0;
            if (ready && !scaledOnCard)
                markColours(into, bt601);
        }
        av_frame_unref(card);
        av_frame_unref(lastOfScreen.get());
        if (ready && cutOut) {
            av_frame_move_ref(lastOfScreen.get(), whole);
            lastCut = cut;
        }
        drawingDelivered = ready && !cutOut ? drawingInBgra : FrameSize{};
        av_frame_unref(whole);
        if (!ready)
            return Status::NoFrame;

        if (lastWasCutOut != cutOut)
            lastWasCutOut = cutOut;
        (cutOut ? cutOutPictures : drawnPictures)++;
        delivered();
        return Status::Frame;
    }

    bool duplicateScreenOf(const CaptureRect &client)
    {
        const CapturePoint middle = client.center();
        const HMONITOR under = MonitorFromPoint({ middle.x, middle.y }, MONITOR_DEFAULTTONULL);
        const bool retryDue = Clock::now() >= retryAt;
        if (under != screenHandle || (screenChanged && retryDue)) {
            dropDuplication();
            screenHandle = under;
            screenChanged = false;
            screen.reset();
            for (const Monitor &each : attachedMonitors()) {
                if (each.handle == under)
                    screen = each;
            }
        }

        const int adapter = screen ? screen->adapter : std::max(0, deviceAdapter);
        if (adapter != deviceAdapter || (!device && retryDue)) {
            dropDuplication();
            av_buffer_unref(&pool);
            av_buffer_unref(&device);
            deviceAdapter = adapter;
            if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, std::to_string(adapter).c_str(), nullptr, 0) < 0)
                retryAt = Clock::now() + std::chrono::milliseconds(RETRY_INTERVAL_MS);
        }
        if (device && !pool)
            pool = cardPictures(device, pictureSize);

        if (!pool || !screen || screen->turned)
            return false;
        if (graph)
            return true;
        if (!retryDue)
            return false;

        const std::string chain = "ddagrab=output_idx=" + std::to_string(screen->output) + ":framerate=" + std::to_string(request.framerate);
        const int built = assemble(chain, device, &graph, &sink);
        if (built < 0) {
            screenChanged = true;
            retryAt = Clock::now() + std::chrono::milliseconds(RETRY_INTERVAL_MS);
        }
        return built >= 0;
    }

    bool cutOutOfScreen(const CaptureRect &client, CutOut *cut)
    {
        drawnBecause = "its size is changing, or is smaller than what is sent";
        if (evenSize(client.size()) != pictureSize) {
            return false;
        }
        drawnBecause = "a window above overlaps it";
        if (isCoveredOnScreen(window, client, ShellSurfaces::LeftForLater)) {
            return false;
        }

        drawnBecause = "it reaches past its screen";
        const CaptureRect screenGeom = screen->geometry;
        if (!screenGeom.adjusted(-REACH_PAST_SCREEN, -REACH_PAST_SCREEN, REACH_PAST_SCREEN, REACH_PAST_SCREEN).contains(client)) {
            return false;
        }
        const CaptureRect onScreen = client & screenGeom;
        const CaptureRect inPicture = CaptureRect{ onScreen.left() - client.left(), onScreen.top() - client.top(), onScreen.width, onScreen.height } & CaptureRect{ 0, 0, pictureSize.width, pictureSize.height };

        drawnBecause = "its screen had no picture";
        AVFrame *whole = wholeScreen.get();
        av_frame_unref(whole);
        const int got = av_buffersink_get_frame(sink, whole);
        if (got < 0) {
            if (got != AVERROR(EAGAIN)) {
                dropDuplication();
                screenChanged = true;
                retryAt = Clock::now() + std::chrono::milliseconds(RETRY_INTERVAL_MS);
            }
            return false;
        }

        drawnBecause = "it moved or was covered while its screen was read";
        if (clientOnScreen(window) != client || isCoveredOnScreen(window, client, ShellSurfaces::LookedFor)) {
            av_frame_unref(whole);
            return false;
        }
        *cut = { inPicture.translated(client.left() - screenGeom.left(), client.top() - screenGeom.top()), inPicture };
        return true;
    }

    Drawing drawnByItself(const CaptureRect &client)
    {
        if (!drawer)
            drawer = WindowDrawer::start(window, true);
        if (!drawer->draw(std::chrono::milliseconds(DRAWING_LIMIT_MS)))
            return Drawing::Failed;
        drawer->drawPointer(client);

        const Bitmap &drawn = drawer->drawn();
        AVFrame *bg = bgra.get();
        const FrameSize delivered = std::exchange(drawingDelivered, FrameSize{});
        drawingInBgra = {};
        if (drawn.size() == client.size()) {
            if (!prepareBgra(bg, pictureSize))
                return Drawing::Failed;
            if (skipsUnchanged && delivered == drawn.size() && showsTopLeftOf(drawn, bg)) {
                drawingDelivered = delivered;
                return Drawing::Unchanged;
            }
            copyTopLeft(drawn, bg);
            drawingInBgra = drawn.size();
            return Drawing::Drawn;
        }

        AVFrame *asDr = asDrawn.get();
        if (!prepareBgra(asDr, drawn.size()))
            return Drawing::Failed;
        copyTopLeft(drawn, asDr);
        av_frame_unref(bg);
        return rescaler.convert(asDr, pictureSize, AV_PIX_FMT_BGRA, false, bg) >= 0 ? Drawing::Drawn : Drawing::Failed;
    }

    Status afterFailure(int error, std::string &failureText)
    {
        tearDown();

        const auto now = Clock::now();
        const bool secureDesktopShowing = error == AVERROR(EPERM) && everDelivered;
        if (!failingSince || secureDesktopShowing)
            failingSince = now;
        if (now - *failingSince >= std::chrono::milliseconds(GIVE_UP_AFTER_MS)) {
            failed = true;
            const std::string subject = window ? "The window" : "The screen";
            failureText = subject + " can no longer be captured (" + describeError(error) + ").";
            return Status::Failed;
        }
        retryAt = now + std::chrono::milliseconds(RETRY_INTERVAL_MS);
        return Status::Stalled;
    }

    int buildDuplication()
    {
        const std::vector<Monitor> monitors = attachedMonitors();
        const Monitor *mon = numberedMonitor(monitors, request.sourceId);
        if (!mon)
            return AVERROR(ENODEV);

        AVBufferRef *dev = nullptr;
        int result = av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_D3D11VA, std::to_string(mon->adapter).c_str(), nullptr, 0);
        if (result < 0)
            return result;

        const std::string chain = "ddagrab=output_idx=" + std::to_string(mon->output) + ":framerate=" + std::to_string(request.framerate);
        result = assemble(chain, dev, &graph, &sink);
        av_buffer_unref(&dev);
        wentOn = "";
        return result;
    }

    int buildGraphicsCapture()
    {
        FrameSize ownSize = pictureSize;
        std::string source;
        std::string adapter;
        if (window) {
            source = ":hwnd=" + std::to_string(reinterpret_cast<uintptr_t>(window));
        } else {
            const std::vector<Monitor> monitors = attachedMonitors();
            const Monitor *mon = numberedMonitor(monitors, request.sourceId);
            if (!mon)
                return AVERROR(ENODEV);
            ownSize = evenSize(mon->geometry.size());
            source = ":hmonitor=" + std::to_string(reinterpret_cast<uintptr_t>(mon->handle));
            adapter = std::to_string(mon->adapter);
        }

        const FrameSize size = encodeSizeFor(ownSize, request.maxHeight);
        const bool asCaptured = request.keepOnCard && request.rgbOnCard && size == ownSize;
        const bool fillsItsPicture = !window || evenSize(clientSize) == ownSize;
        const bool scalesByItself = request.keepOnCard && request.rgbOnCard && size != ownSize && fillsItsPicture;
        const FrameSize capSize = scalesByItself ? size : ownSize;

        std::string chain = "gfxcapture=max_framerate=" + std::to_string(request.framerate) + source + ":width=" + std::to_string(capSize.width) + ":height=" + std::to_string(capSize.height);
        if (scalesByItself)
            chain += ":resize_mode=scale";
        int result = 0;
        session = GraphicsCaptureSession::start(chain, adapter, &result);
        if (window)
            emptyPicturesUntil = Clock::now() + std::chrono::milliseconds(EMPTY_PICTURES_FOR_MS);
        if (result >= 0)
            return result;

        if (result == AVERROR(ENOSYS))
            graphicsCaptureRuledOut = true;
        const bool leftToChoose = request.method == CaptureMethod::Auto && window && !everDelivered;
        if ((result == AVERROR(ENOSYS) || leftToChoose) && screenCaptureAvailable()) {
            path = window ? Path::WindowByDuplication : Path::Duplication;
        }
        return result;
    }

    void dropDuplication()
    {
        avfilter_graph_free(&graph);
        sink = nullptr;
        av_frame_unref(lastOfScreen.get());
    }

    void tearDown()
    {
        dropDuplication();
        if (session)
            session->abandon();
        session.reset();
        if (drawer)
            drawer->abandon();
        drawer.reset();
    }
#endif // _WIN32
};

ScreenCapture::ScreenCapture() : impl(std::make_unique<Impl>()) {}

ScreenCapture::~ScreenCapture() = default;

void ScreenCapture::configure(const Request &request)
{
    failureText.clear();
#ifdef _WIN32
    impl->configure(request);
#else
    impl->request = request;
    const auto method = request.method == CaptureMethod::Auto ? (X11Capture::usable() ? CaptureMethod::X11 : CaptureMethod::Portal) : request.method;
    impl->linux = method == CaptureMethod::X11 ? X11Capture::open(request.sourceId, request.framerate) :
        method == CaptureMethod::Portal ? PortalCapture::open(request.sourceId, request.framerate, request.keepOnCard) : nullptr;
    if (impl->linux) {
        impl->name = impl->linux->name();
        impl->deliversChanges = impl->linux->deliversOnlyChanges();
        return;
    }
    failureText = "No usable capture backend is available for this source.";
#endif
}

ScreenCapture::Status ScreenCapture::read(AVFrame *into)
{
#ifdef _WIN32
    if (impl->failed)
        return Status::Failed;
    if (impl->closed)
        return Status::Closed;

    const UnscaledCoordinates unscaled;
    if (impl->window) {
        const Status watched = impl->watchWindow();
        if (watched != Status::Frame)
            return watched;
    }

    if (!picturesWanted && impl->path != Impl::Path::Duplication) {
        std::this_thread::sleep_for(pictureInterval(impl->request.framerate));
        return Status::NoFrame;
    }

    switch (impl->path) {
    case Impl::Path::GraphicsCapture:
        return impl->readGraphicsCapture(into, picturesWanted, failureText);
    case Impl::Path::WindowByDuplication:
        return impl->readWindowByDuplication(into, failureText);
    case Impl::Path::Duplication:
        break;
    }
    return impl->readDuplication(into, picturesWanted, failureText);
#else
    if (!impl->linux)
        return Status::Failed;
    switch (impl->linux->grab(into, std::chrono::milliseconds(RETRY_INTERVAL_MS))) {
    case LinuxGrabber::Result::Frame:
        return Status::Frame;
    case LinuxGrabber::Result::NoFrame:
        return Status::NoFrame;
    case LinuxGrabber::Result::Hidden:
        return Status::Hidden;
    case LinuxGrabber::Result::Closed:
        return Status::Closed;
    case LinuxGrabber::Result::Failed:
        failureText = impl->linux->failure();
        return Status::Failed;
    }
    return Status::Failed;
#endif
}

bool ScreenCapture::deliversOnlyChanges() const
{
#ifdef _WIN32
    return true;
#else
    return impl->deliversChanges;
#endif
}

const std::string &ScreenCapture::captureName() const
{
#ifdef _WIN32
    static std::string result;
    switch (impl->path) {
    case Impl::Path::GraphicsCapture:
        result = "gfxcapture";
        break;
    case Impl::Path::WindowByDuplication:
        result = "ddagrab " + std::to_string(impl->cutOutPictures) + " + PrintWindow " + std::to_string(impl->drawnPictures) + " frames";
        break;
    case Impl::Path::Duplication:
        result = "ddagrab";
        break;
    }
    return result;
#else
    return impl->name;
#endif
}

} // namespace Styx::Core::Video
