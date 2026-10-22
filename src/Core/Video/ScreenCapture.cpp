// Ported from ouwou/Acheron video-streams commit 71b4907 (GPL-3.0-or-later).
// Windows Graphics Capture and desktop duplication are FFmpeg capture filters.  Keep this
// translation unit independent of Qt: capture runs on the encoder's native thread.
#include "Core/Video/ScreenCapture.hpp"
#include "Core/Video/LinuxCapture.hpp"
#include "Core/Video/PictureConverter.hpp"
#include "Platform/Image.hpp"
#include "Platform/Settings.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi.h>
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}
#endif

namespace Styx::Core::Video {
namespace {
constexpr char captureMethodKey[] = "video/capture_method";
constexpr int HdHeight = 720;
constexpr int StalledWaitMs = 50;
constexpr int NewFrameWaitMs = 25;
constexpr int EmptyPicturesMs = 3000;


bool intersects(const CaptureRect &a, const CaptureRect &b) {
    if (a.empty() || b.empty()) return false;
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}
FrameSize even(FrameSize size) { return {std::max(2, size.width & ~1), std::max(2, size.height & ~1)}; }
FrameSize scaledKeepAspectRatio(FrameSize size, FrameSize bound) {
    if (size.width <= 0 || size.height <= 0 || bound.width <= 0 || bound.height <= 0) return {};
    const auto wAtBoundH = std::int64_t(bound.height) * size.width / size.height;
    if (wAtBoundH <= bound.width) {
        return {int(wAtBoundH), bound.height};
    } else {
        return {bound.width, int(std::int64_t(bound.width) * size.height / size.width)};
    }
}

#ifdef _WIN32
template <typename T> using Com = Microsoft::WRL::ComPtr<T>;
struct FrameRelease { void operator()(AVFrame *frame) const { av_frame_free(&frame); } };
using Frame = std::unique_ptr<AVFrame, FrameRelease>;
using Clock = std::chrono::steady_clock;

struct Monitor {
    int adapter = 0;
    int output = 0;
    HMONITOR handle = nullptr;
    CaptureRect bounds;
    bool rotated = false;
};
std::vector<Monitor> monitors() {
    std::vector<Monitor> found;
    Com<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(factory.GetAddressOf())))) return found;
    Com<IDXGIAdapter> adapter;
    for (UINT ai = 0; SUCCEEDED(factory->EnumAdapters(ai, &adapter)); ++ai) {
        Com<IDXGIOutput> output;
        for (UINT oi = 0; SUCCEEDED(adapter->EnumOutputs(oi, &output)); ++oi) {
            DXGI_OUTPUT_DESC desc{};
            DEVMODEW mode{}; mode.dmSize = sizeof(mode);
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop &&
                EnumDisplaySettingsW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) {
                found.push_back({int(ai), int(oi), desc.Monitor,
                    {int(mode.dmPosition.x), int(mode.dmPosition.y), int(mode.dmPelsWidth), int(mode.dmPelsHeight)},
                    desc.Rotation != DXGI_MODE_ROTATION_IDENTITY});
            }
            output.Reset();
        }
        adapter.Reset();
    }
    return found;
}
const Monitor *monitorFor(const std::vector<Monitor> &all, const std::string &id) {
    if (!id.starts_with("screen:")) return nullptr;
    try {
        const auto index = std::stoul(id.substr(7));
        return index < all.size() ? &all[index] : nullptr;
    } catch (...) { return nullptr; }
}
HWND windowFor(const std::string &id) {
    if (!id.starts_with("window:")) return nullptr;
    try { return reinterpret_cast<HWND>(static_cast<uintptr_t>(std::stoull(id.substr(7)))); }
    catch (...) { return nullptr; }
}
CaptureRect rect(const RECT &value) { return {value.left, value.top, value.right - value.left, value.bottom - value.top}; }
HRESULT dwmAttribute(HWND window, DWORD attribute, void *value, DWORD size) {
    using Fn = HRESULT(WINAPI *)(HWND, DWORD, PVOID, DWORD);
    static auto fn = reinterpret_cast<Fn>(GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmGetWindowAttribute"));
    return fn ? fn(window, attribute, value, size) : E_NOTIMPL;
}
bool cloaked(HWND window) {
    DWORD value = 0;
    return SUCCEEDED(dwmAttribute(window, 14, &value, sizeof(value))) && value != 0;
}
CaptureRect visibleBounds(HWND window) {
    RECT value{};
    if (FAILED(dwmAttribute(window, 9, &value, sizeof(value))) || IsRectEmpty(&value)) GetWindowRect(window, &value);
    return rect(value);
}
CaptureRect clientBounds(HWND window) {
    RECT client{}; POINT point{};
    if (GetClientRect(window, &client) && ClientToScreen(window, &point))
        return {point.x, point.y, client.right, client.bottom};
    return visibleBounds(window);
}
bool shareable(HWND window) {
    DWORD process = 0; GetWindowThreadProcessId(window, &process);
    const LONG_PTR styles = GetWindowLongPtrW(window, GWL_EXSTYLE);
    return process != GetCurrentProcessId() && IsWindowVisible(window) && window != GetShellWindow() &&
        !cloaked(window) && !(styles & WS_EX_TOOLWINDOW) &&
        !((styles & WS_EX_NOACTIVATE) && !(styles & WS_EX_APPWINDOW));
}
bool graphicsUsable() {
    static const bool usable = [] {
        if (!avfilter_get_by_name("gfxcapture")) return false;
        OSVERSIONINFOEXW version{}; version.dwOSVersionInfoSize = sizeof(version);
        version.dwMajorVersion = 10; version.dwBuildNumber = 18362;
        DWORDLONG mask = VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL);
        mask = VerSetConditionMask(mask, VER_BUILDNUMBER, VER_GREATER_EQUAL);
        return VerifyVersionInfoW(&version, VER_MAJORVERSION | VER_BUILDNUMBER, mask) != FALSE;
    }();
    return usable;
}
bool duplicationUsable() { return avfilter_get_by_name("ddagrab") != nullptr; }
int graphFor(const std::string &chain, int adapter, AVFilterGraph **graph, AVFilterContext **sink) {
    AVBufferRef *device = nullptr;
    const std::string adapterName = std::to_string(adapter);
    int result = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, adapterName.c_str(), nullptr, 0);
    if (result >= 0) result = assemble(chain, device, graph, sink);
    av_buffer_unref(&device);
    return result;
}
std::string errorText(int error) {
    char value[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, value, sizeof(value));
    return value;
}

class GraphicsSession {
public:
    enum class Take { Frame, Nothing, Ended };
    static std::shared_ptr<GraphicsSession> start(const std::string &chain, int adapter, int *error) {
        auto session = std::shared_ptr<GraphicsSession>(new GraphicsSession);
        *error = graphFor(chain, adapter, &session->graph, &session->sink);
        if (*error < 0) return {};
        std::thread([session] { session->run(); }).detach();
        return session;
    }
    ~GraphicsSession() { avfilter_graph_free(&graph); }
    void abandon() { std::lock_guard lock(mutex); abandoned = true; }
    Take take(AVFrame *into, Clock::time_point due) {
        std::unique_lock lock(mutex);
        const auto limit = due + std::chrono::milliseconds(NewFrameWaitMs);
        while (!ended && (!fresh || Clock::now() < due)) {
            if (Clock::now() >= limit) return Take::Nothing;
            arrived.wait_until(lock, fresh ? due : limit);
        }
        if (ended) return Take::Ended;
        av_frame_unref(into); av_frame_move_ref(into, latest.get()); fresh = false;
        return Take::Frame;
    }
    int endedWith() const { std::lock_guard lock(mutex); return ended; }
private:
    void run() {
        Frame picture{av_frame_alloc()};
        for (;;) {
            const int got = av_buffersink_get_frame(sink, picture.get());
            std::lock_guard lock(mutex);
            if (abandoned) break;
            if (got < 0) { ended = got; break; }
            av_frame_unref(latest.get()); av_frame_move_ref(latest.get(), picture.get()); fresh = true; arrived.notify_all();
        }
        arrived.notify_all();
    }
    AVFilterGraph *graph = nullptr; AVFilterContext *sink = nullptr;
    mutable std::mutex mutex; std::condition_variable arrived;
    Frame latest{av_frame_alloc()}; bool fresh = false; bool abandoned = false; int ended = 0;
};

bool covered(HWND target, const CaptureRect &client) {
    struct Stack { HWND target; CaptureRect client; std::vector<StackedWindow> windows; bool found = false; } state{target, client};
    EnumWindows([](HWND window, LPARAM value) -> BOOL {
        auto &state = *reinterpret_cast<Stack *>(value);
        StackedWindow item; item.visible = IsWindowVisible(window); item.minimised = IsIconic(window); item.cloaked = cloaked(window);
        if (window == state.target) { item.bounds = state.client; state.found = true; state.windows.push_back(item); return FALSE; }
        if (item.visible && !item.minimised) {
            item.bounds = visibleBounds(window);
            const LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE);
            item.overlay = (style & (WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)) != 0 && window != GetForegroundWindow();
            wchar_t klass[64]{}; GetClassNameW(window, klass, 64);
            item.tabProxy = wcscmp(klass, L"Windows.Internal.Shell.TabProxyWindow") == 0;
            item.livePreview = (style & WS_EX_TOPMOST) && wcscmp(klass, L"LivePreview") == 0;
        }
        state.windows.push_back(item); return TRUE;
    }, reinterpret_cast<LPARAM>(&state));
    return !state.found || windowIsCovered(state.windows, state.windows.size() - 1);
}
bool prepareBgra(AVFrame *frame, FrameSize size) {
    if (frame->buf[0] && frame->format == AV_PIX_FMT_BGRA && frame->width == size.width && frame->height == size.height)
        return av_frame_make_writable(frame) >= 0;
    av_frame_unref(frame); frame->format = AV_PIX_FMT_BGRA; frame->width = size.width; frame->height = size.height;
    return av_frame_get_buffer(frame, 32) >= 0;
}
bool printWindow(HWND window, const CaptureRect &client, AVFrame *into) {
    if (!prepareBgra(into, even({client.width, client.height}))) return false;
    HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, into->width, into->height);
    const auto old = SelectObject(memory, bitmap);
    const BOOL drawn = PrintWindow(window, memory, 2);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = into->width;
    info.bmiHeader.biHeight = -into->height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    const bool copied = drawn && GetDIBits(memory, bitmap, 0, into->height, into->data[0], &info, DIB_RGB_COLORS) != 0;
    SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(nullptr, screen);
    return copied;
}
#endif
} // namespace

FrameSize encodeSizeFor(FrameSize source, int maxHeight) {
    FrameSize size = source;
    const FrameSize bound{maxHeight * 16 / 9, maxHeight};
    if (maxHeight > 0 && source.width > 0 && source.height > 0 && (source.width > bound.width || source.height > bound.height)) {
        // Fitted with its shape kept, rounding down, as QSize::scaled(Qt::KeepAspectRatio) does.
        const auto widthAtBoundHeight = std::int64_t(bound.height) * source.width / source.height;
        if (widthAtBoundHeight <= bound.width)
            size = {int(widthAtBoundHeight), bound.height};
        else
            size = {bound.width, int(std::int64_t(bound.width) * source.height / source.width)};
    }
    return even(size);
}
FrameSize encodeSizeOf(const CaptureSource &source, int maxHeight) {
#ifdef _WIN32
    if (source.kind == CaptureSource::Kind::Window) {
        const HWND window = windowFor(source.id);
        if (!window || !IsWindowVisible(window) || IsIconic(window)) return {};
        const auto inside = clientBounds(window);
        return inside.empty() ? FrameSize{} : encodeSizeFor(even({std::max(160, inside.width), std::max(120, inside.height)}), maxHeight);
    }
#endif
    return source.geometry.empty() ? FrameSize{} : encodeSizeFor({source.geometry.width, source.geometry.height}, maxHeight);
}
bool stillPictureIsDue(std::chrono::milliseconds since, bool waitingForKeyFrame, int framerate) {
    return since >= std::chrono::milliseconds(500) ||
        (waitingForKeyFrame && since >= std::chrono::milliseconds(1000 / std::max(1, framerate)));
}
bool windowIsCovered(const std::vector<StackedWindow> &topDown, std::size_t target) {
    if (target >= topDown.size()) return true;
    const auto &window = topDown[target];
    if (window.bounds.empty() || window.minimised || window.cloaked) return true;
    for (std::size_t i = 0; i < target; ++i) {
        const auto &above = topDown[i];
        if (!above.visible || above.minimised) continue;
        if (above.livePreview) return true;
        if (!above.overlay && !above.cloaked && !above.tabProxy && intersects(above.bounds, window.bounds)) return true;
    }
    return false;
}
bool screenCaptureAvailable() {
#ifdef _WIN32
    return duplicationUsable();
#else
    return captureMethodUsable(CaptureMethod::Auto);
#endif
}
bool windowCaptureAvailable() {
#ifdef _WIN32
    return graphicsUsable() || duplicationUsable();
#else
    return captureMethodUsable(CaptureMethod::Auto);
#endif
}
CaptureMethod captureMethod() {
    const auto setting = Platform::Settings::instance().getString(captureMethodKey, "auto");
    if (setting == "graphics_capture") return CaptureMethod::GraphicsCapture;
    if (setting == "desktop_duplication") return CaptureMethod::DesktopDuplication;
    if (setting == "portal") return CaptureMethod::Portal;
    if (setting == "x11") return CaptureMethod::X11;
    return CaptureMethod::Auto;
}
void setCaptureMethod(CaptureMethod method) {
    const char *name = method == CaptureMethod::GraphicsCapture ? "graphics_capture" :
        method == CaptureMethod::DesktopDuplication ? "desktop_duplication" :
        method == CaptureMethod::Portal ? "portal" : method == CaptureMethod::X11 ? "x11" : "auto";
    Platform::Settings::instance().setString(captureMethodKey, name);
}
bool captureMethodUsable(CaptureMethod method) {
#ifdef _WIN32
    if (method == CaptureMethod::GraphicsCapture) return graphicsUsable();
    if (method == CaptureMethod::DesktopDuplication) return duplicationUsable();
    return method == CaptureMethod::Auto && (graphicsUsable() || duplicationUsable());
#else
    if (method == CaptureMethod::X11) return X11Capture::usable();
    if (method == CaptureMethod::Portal) return PortalCapture::usable();
    return method == CaptureMethod::Auto && (X11Capture::usable() || PortalCapture::usable());
#endif
}
CaptureSources captureSources(CaptureMethod method) {
#ifdef _WIN32
    if (method == CaptureMethod::Portal || method == CaptureMethod::X11) return {};
    CaptureSources sources;
    if (!captureMethodUsable(method)) return sources;
    const auto all = monitors();
    for (std::size_t i = 0; i < all.size(); ++i) sources.push_back({"screen:" + std::to_string(i), CaptureSource::Kind::Screen, "Screen " + std::to_string(i + 1), all[i].bounds, 0});
    return sources;
#else
    if (method == CaptureMethod::Auto) method = X11Capture::usable() ? CaptureMethod::X11 : CaptureMethod::Portal;
    return method == CaptureMethod::X11 ? X11Capture::screens() : CaptureSources{};
#endif
}
CaptureSources captureWindows(CaptureMethod method) {
#ifdef _WIN32
    CaptureSources result;
    if (!captureMethodUsable(method)) return result;
    EnumWindows([](HWND window, LPARAM output) -> BOOL {
        if (!shareable(window)) return TRUE;
        wchar_t title[512]{}; if (!GetWindowTextW(window, title, 512)) return TRUE;
        char utf8[2048]{}; WideCharToMultiByte(CP_UTF8, 0, title, -1, utf8, sizeof(utf8), nullptr, nullptr);
        DWORD process = 0; GetWindowThreadProcessId(window, &process);
        reinterpret_cast<CaptureSources *>(output)->push_back({"window:" + std::to_string(reinterpret_cast<uintptr_t>(window)), CaptureSource::Kind::Window, utf8, visibleBounds(window), process});
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
#else
    return method == CaptureMethod::X11 || (method == CaptureMethod::Auto && X11Capture::usable()) ? X11Capture::windows() : CaptureSources{};
#endif
}
bool isChosenByTheDesktop(const CaptureSource &source) { return source.id.starts_with("pipewire:"); }
bool isHandedOverByTheDesktop(const CaptureSource &source) { return isChosenByTheDesktop(source); }
void releaseSource(const CaptureSource &source) { if (isHandedOverByTheDesktop(source)) PortalCapture::release(source.id); }
void restoreIfMinimised(const CaptureSource &source) {
#ifdef _WIN32
    if (const auto window = windowFor(source.id); window && IsIconic(window)) ShowWindowAsync(window, SW_RESTORE);
#else
    X11Capture::restoreIfMinimised(source);
#endif
}
Platform::ImageBuffer captureThumbnail(const CaptureSource &source, FrameSize bound) {
    Platform::ImageBuffer image;
#ifdef _WIN32
    if (bound.width <= 0 || bound.height <= 0) return image;
    const HWND window = source.kind == CaptureSource::Kind::Window ? windowFor(source.id) : nullptr;
    if (source.kind == CaptureSource::Kind::Window && (!window || !IsWindow(window) || IsIconic(window)))
        return image;
    HDC screen = GetDC(nullptr);
    bool drawn = false;
    FrameSize size{};
    HDC memory = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old = nullptr;

    if (window) {
        RECT outer{};
        if (!GetWindowRect(window, &outer) || IsRectEmpty(&outer)) {
            ReleaseDC(nullptr, screen);
            return image;
        }
        const int outerW = outer.right - outer.left;
        const int outerH = outer.bottom - outer.top;
        if (outerW <= 0 || outerH <= 0) {
            ReleaseDC(nullptr, screen);
            return image;
        }
        RECT vis{};
        if (FAILED(dwmAttribute(window, 9, &vis, sizeof(vis))) || IsRectEmpty(&vis)) {
            vis = outer;
        }
        const int srcX = std::max(0, int(vis.left - outer.left));
        const int srcY = std::max(0, int(vis.top - outer.top));
        const int srcW = std::min(outerW - srcX, int(vis.right - vis.left));
        const int srcH = std::min(outerH - srcY, int(vis.bottom - vis.top));
        if (srcW <= 0 || srcH <= 0) {
            ReleaseDC(nullptr, screen);
            return image;
        }

        size = (srcW > bound.width || srcH > bound.height)
            ? scaledKeepAspectRatio({srcW, srcH}, bound)
            : FrameSize{srcW, srcH};
        size.width = std::max(1, size.width);
        size.height = std::max(1, size.height);

        memory = CreateCompatibleDC(screen);
        bitmap = CreateCompatibleBitmap(screen, size.width, size.height);
        old = SelectObject(memory, bitmap);
        SetStretchBltMode(memory, HALFTONE);

        HDC full = CreateCompatibleDC(screen);
        HBITMAP fullBitmap = CreateCompatibleBitmap(screen, outerW, outerH);
        const auto fullOld = SelectObject(full, fullBitmap);
        drawn = PrintWindow(window, full, 2) && StretchBlt(memory, 0, 0, size.width, size.height, full, srcX, srcY, srcW, srcH, SRCCOPY);
        SelectObject(full, fullOld);
        DeleteObject(fullBitmap);
        DeleteDC(full);
    } else {
        const CaptureRect area = source.geometry;
        if (area.empty()) {
            ReleaseDC(nullptr, screen);
            return image;
        }
        size = (area.width > bound.width || area.height > bound.height)
            ? scaledKeepAspectRatio({area.width, area.height}, bound)
            : FrameSize{area.width, area.height};
        size.width = std::max(1, size.width);
        size.height = std::max(1, size.height);

        memory = CreateCompatibleDC(screen);
        bitmap = CreateCompatibleBitmap(screen, size.width, size.height);
        old = SelectObject(memory, bitmap);
        SetStretchBltMode(memory, HALFTONE);
        drawn = StretchBlt(memory, 0, 0, size.width, size.height, screen, area.x, area.y, area.width, area.height, SRCCOPY | CAPTUREBLT);
    }

    if (drawn && memory && bitmap) {
        image = {size.width, size.height, 4, std::vector<uint8_t>(size_t(size.width) * size.height * 4)};
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = size.width;
        info.bmiHeader.biHeight = -size.height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        if (GetDIBits(memory, bitmap, 0, size.height, image.pixels.data(), &info, DIB_RGB_COLORS) == 0) {
            image = {};
        } else {
            // GDI hands back BGRX: swap to RGBA and make every pixel opaque.
            for (size_t i = 0; i < image.pixels.size(); i += 4) {
                std::swap(image.pixels[i], image.pixels[i + 2]);
                image.pixels[i + 3] = 255;
            }
        }
    }
    if (memory) {
        SelectObject(memory, old);
        DeleteObject(bitmap);
        DeleteDC(memory);
    }
    ReleaseDC(nullptr, screen);
#endif
    return image;
}

struct ScreenCapture::Impl {
    std::unique_ptr<LinuxGrabber> linux;
#ifdef _WIN32
    HWND window = nullptr; CaptureMethod method = CaptureMethod::Auto; bool graphics = false; bool windowDuplication = false;
    AVFilterGraph *graph = nullptr; AVFilterContext *sink = nullptr; std::shared_ptr<GraphicsSession> session;
    Frame captured{av_frame_alloc()}; Frame bgra{av_frame_alloc()}; PictureConverter converter;
    FrameSize pictureSize{}; CaptureRect client{}; Clock::time_point due{}; Clock::time_point retry{}; std::optional<Clock::time_point> failing;
    bool delivered = false; bool skipUnchanged = true; uint64_t cutOut = 0; uint64_t drawn = 0;
    ~Impl() { avfilter_graph_free(&graph); if (session) session->abandon(); }
#endif
    Request request; std::string name; bool changes = true;
};
ScreenCapture::ScreenCapture() : impl(std::make_unique<Impl>()) {}
ScreenCapture::~ScreenCapture() = default;
void ScreenCapture::configure(const Request &request) {
    impl = std::make_unique<Impl>(); impl->request = request; failureText.clear();
#ifndef _WIN32
    const auto method = request.method == CaptureMethod::Auto ? (X11Capture::usable() ? CaptureMethod::X11 : CaptureMethod::Portal) : request.method;
    impl->linux = method == CaptureMethod::X11 ? X11Capture::open(request.sourceId, request.framerate) :
        method == CaptureMethod::Portal ? PortalCapture::open(request.sourceId, request.framerate, request.keepOnCard) : nullptr;
    if (impl->linux) { impl->name = impl->linux->name(); impl->changes = impl->linux->deliversOnlyChanges(); return; }
    failureText = "No usable capture backend is available for this source.";
#else
    impl->window = windowFor(request.sourceId); impl->method = request.method; impl->skipUnchanged = Platform::Settings::instance().getBool("video/skip_unchanged_pictures", true);
    impl->graphics = (request.method == CaptureMethod::GraphicsCapture || (request.method == CaptureMethod::Auto && impl->window)) && graphicsUsable();
    impl->windowDuplication = impl->window && !impl->graphics;
    impl->name = impl->graphics ? "gfxcapture" : impl->windowDuplication ? "ddagrab + PrintWindow" : "ddagrab";
#endif
}
ScreenCapture::Status ScreenCapture::read(AVFrame *into) {
#ifndef _WIN32
    if (!impl->linux) return Status::Failed;
    switch (impl->linux->grab(into, std::chrono::milliseconds(RETRY_INTERVAL_MS))) {
    case LinuxGrabber::Result::Frame: return Status::Frame; case LinuxGrabber::Result::NoFrame: return Status::NoFrame;
    case LinuxGrabber::Result::Hidden: return Status::Hidden; case LinuxGrabber::Result::Closed: return Status::Closed;
    case LinuxGrabber::Result::Failed: failureText = impl->linux->failure(); return Status::Failed;
    } return Status::Failed;
#else
    auto &state = *impl;
    const Request &request = state.request;
    const auto now = Clock::now();
    if (state.window) {
        if (!IsWindow(state.window)) return Status::Closed;
        state.client = (!IsWindowVisible(state.window) || IsIconic(state.window)) ? CaptureRect{} : clientBounds(state.window);
        if (state.client.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(StalledWaitMs)); return Status::Hidden; }
        state.pictureSize = even({std::max(160, state.client.width), std::max(120, state.client.height)});
    }
    auto fail = [&](int error) {
        if (!state.failing) state.failing = now;
        if (now - *state.failing >= std::chrono::milliseconds(GIVE_UP_AFTER_MS)) { failureText = "The capture source can no longer be captured (" + errorText(error) + ")."; return Status::Failed; }
        state.retry = now + std::chrono::milliseconds(RETRY_INTERVAL_MS); return Status::Stalled;
    };
    if (now < state.retry) { std::this_thread::sleep_for(std::chrono::milliseconds(StalledWaitMs)); return Status::Stalled; }
    auto deliver = [&](AVFrame *frame) {
        const auto size = encodeSizeFor({frame->width, frame->height}, request.maxHeight);
        int result = 0;
        if (request.keepOnCard && request.rgbOnCard && size.width == frame->width && size.height == frame->height) {
            av_frame_unref(into); av_frame_move_ref(into, frame);
        } else {
            if (frame->hw_frames_ctx) result = av_hwframe_transfer_data(state.bgra.get(), frame, 0);
            else { av_frame_unref(state.bgra.get()); av_frame_ref(state.bgra.get(), frame); }
            av_frame_unref(frame);
            if (result >= 0) result = state.converter.convert(state.bgra.get(), size, request.softwareFormat, request.bt601Only || size.height < HdHeight, into);
        }
        if (result < 0) return fail(result);
        into->colorspace = request.bt601Only || size.height < HdHeight ? AVCOL_SPC_SMPTE170M : AVCOL_SPC_BT709;
        into->color_range = AVCOL_RANGE_MPEG; state.delivered = true; state.failing.reset(); return Status::Frame;
    };
    if (state.windowDuplication && covered(state.window, state.client)) {
        if (!picturesWanted) return Status::NoFrame;
        if (!printWindow(state.window, state.client, state.captured.get())) return Status::NoFrame;
        ++state.drawn; return deliver(state.captured.get());
    }
    if (state.graphics) {
        if (!state.session) {
            const std::string source = state.window ? ":hwnd=" + std::to_string(reinterpret_cast<uintptr_t>(state.window)) : "";
            int adapter = 0; FrameSize size = state.pictureSize;
            if (!state.window) { const auto all = monitors(); const auto *monitor = monitorFor(all, request.sourceId); if (!monitor) return fail(AVERROR(ENODEV)); adapter = monitor->adapter; size = even({monitor->bounds.width, monitor->bounds.height}); }
            const auto encoded = encodeSizeFor(size, request.maxHeight);
            int error = 0; state.session = GraphicsSession::start("gfxcapture=max_framerate=" + std::to_string(std::max(1, request.framerate)) + source + ":width=" + std::to_string(encoded.width) + ":height=" + std::to_string(encoded.height) + ":resize_mode=scale", adapter, &error);
            if (!state.session) { if (state.window && request.method == CaptureMethod::Auto && duplicationUsable()) { state.graphics = false; state.windowDuplication = true; return Status::Stalled; } return fail(error); }
        }
        const auto take = state.session->take(state.captured.get(), state.due);
        state.due = now + std::chrono::microseconds(1'000'000 / std::max(1, request.framerate));
        if (take == GraphicsSession::Take::Nothing) return Status::NoFrame;
        if (take == GraphicsSession::Take::Ended) return fail(state.session->endedWith());
        return picturesWanted ? deliver(state.captured.get()) : Status::NoFrame;
    }
    if (!state.graph) {
        const auto all = monitors(); const auto *monitor = state.window ? nullptr : monitorFor(all, request.sourceId);
        if (state.window) {
            const POINT middle{state.client.x + state.client.width / 2, state.client.y + state.client.height / 2};
            const HMONITOR handle = MonitorFromPoint(middle, MONITOR_DEFAULTTONULL);
            for (const auto &candidate : all) if (candidate.handle == handle) monitor = &candidate;
        }
        if (!monitor || monitor->rotated) return fail(AVERROR(ENODEV));
        CaptureRect crop = state.window ? CaptureRect{state.client.x - monitor->bounds.x, state.client.y - monitor->bounds.y, state.client.width, state.client.height} : monitor->bounds;
        std::string chain = "ddagrab=output_idx=" + std::to_string(monitor->output) + ":framerate=" + std::to_string(std::max(1, request.framerate));
        if (state.window) chain += ",crop=" + std::to_string(crop.width) + ":" + std::to_string(crop.height) + ":" + std::to_string(crop.x) + ":" + std::to_string(crop.y);
        const int error = graphFor(chain, monitor->adapter, &state.graph, &state.sink);
        if (error < 0) return fail(error);
    }
    const int got = av_buffersink_get_frame(state.sink, state.captured.get());
    if (got == AVERROR(EAGAIN)) return Status::NoFrame;
    if (got < 0) { avfilter_graph_free(&state.graph); state.sink = nullptr; return fail(got); }
    if (!picturesWanted) { av_frame_unref(state.captured.get()); return Status::NoFrame; }
    if (state.window) ++state.cutOut;
    return deliver(state.captured.get());
#endif
}
bool ScreenCapture::deliversOnlyChanges() const { return impl->changes; }
const std::string &ScreenCapture::captureName() const { return impl->name; }
} // namespace Styx::Core::Video
