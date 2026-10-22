#pragma once

#include "Core/Video/CaptureSource.hpp"

#include <chrono>
#include <memory>
#include <string>

struct AVFrame;
namespace Styx::Core::Video {
class LinuxGrabber {
public:
    enum class Result { Frame, NoFrame, Hidden, Closed, Failed };
    virtual ~LinuxGrabber() = default;
    virtual Result grab(AVFrame *into, std::chrono::milliseconds wait) = 0;
    [[nodiscard]] virtual bool deliversOnlyChanges() const = 0;
    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual std::string failure() const = 0;
};
namespace X11Capture {
[[nodiscard]] bool usable();
[[nodiscard]] CaptureSources screens();
[[nodiscard]] CaptureSources windows();
[[nodiscard]] FrameSize sizeOf(const CaptureSource &source);
void restoreIfMinimised(const CaptureSource &window);
[[nodiscard]] std::unique_ptr<LinuxGrabber> open(const std::string &sourceId, int framerate);
}
namespace PortalCapture {
[[nodiscard]] bool usable();
[[nodiscard]] CaptureSource adopt(std::uint32_t node, FrameSize size);
[[nodiscard]] FrameSize sizeOf(const CaptureSource &source);
[[nodiscard]] std::unique_ptr<LinuxGrabber> open(const std::string &sourceId, int framerate, bool onCard);
void release(const std::string &sourceId);
}
} // namespace Styx::Core::Video
