#pragma once

#include "Platform/Image.hpp"
#include "Platform/Native.hpp"
#include <cstdint>
#include <string>
#include <vector>

#include <memory>

#ifdef _WIN32
#include <d3d11.h>
#include <wrl/client.h>
#endif

namespace Styx {
namespace Core {
namespace Stream {

class D3DDevice;

struct CaptureSource
{
    enum class Kind {
        Monitor,
        Window,
    };

    Kind kind = Kind::Monitor;
    uintptr_t handle = 0; // HMONITOR or HWND
    std::string title;
    Platform::Rect geometry;
    uint32_t processId = 0; // owning process of a window, for per-app audio
    bool primary = false;
};

// Windows Graphics Capture needs Windows 10 1903; per-process loopback audio needs 2004.
bool isScreenCaptureSupported();
std::vector<CaptureSource> enumerateCaptureSources();
Platform::ImageBuffer captureThumbnail(const CaptureSource &source, const Platform::Size &maxSize);

#ifdef _WIN32

// Captures one monitor or window with Windows Graphics Capture. Frames arrive on a WGC
// thread and are copied into a texture we own, so the capture pool never stalls on the
// encoder; the encoder reads the newest frame whenever it ticks.
class ScreenCapture
{
public:
    explicit ScreenCapture(std::shared_ptr<D3DDevice> device);
    ~ScreenCapture();

    bool start(const CaptureSource &source, bool captureCursor, int framerate, std::string *error);
    void stop();

    // The newest frame (BGRA), or null before the first one. Valid until the next call;
    // the caller holds the device mutex while using it.
    struct Frame
    {
        ID3D11Texture2D *texture = nullptr;
        Platform::Size size;
        uint64_t sequence = 0;
    };
    Frame latestFrame();

    [[nodiscard]] bool isClosed() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

#endif

} // namespace Stream
} // namespace Core
} // namespace Styx
