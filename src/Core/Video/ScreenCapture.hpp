#pragma once

#include "Core/Video/CaptureSource.hpp"

#include <cstddef>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace Styx::Platform {
struct ImageBuffer;
}

struct AVFrame;

namespace Styx::Core::Video {

enum class CaptureMethod { Auto, GraphicsCapture, DesktopDuplication, Portal, X11 };
[[nodiscard]] FrameSize encodeSizeFor(FrameSize source, int maxHeight);
[[nodiscard]] FrameSize encodeSizeOf(const CaptureSource &source, int maxHeight);
[[nodiscard]] Platform::ImageBuffer captureThumbnail(const CaptureSource &source, FrameSize bound);

[[nodiscard]] bool stillPictureIsDue(std::chrono::milliseconds sinceLastPicture, bool keyFrameWaitedFor, int framerate);

struct StackedWindow {
    CaptureRect bounds;
    bool visible = true;
    bool minimised = false;
    bool cloaked = false;
    bool overlay = false;
    bool tabProxy = false;
    bool livePreview = false;
};
[[nodiscard]] bool windowIsCovered(const std::vector<StackedWindow> &topDown, std::size_t target);

[[nodiscard]] bool screenCaptureAvailable();
[[nodiscard]] bool windowCaptureAvailable();
[[nodiscard]] CaptureMethod captureMethod();
void setCaptureMethod(CaptureMethod method);
[[nodiscard]] bool captureMethodUsable(CaptureMethod method);
[[nodiscard]] CaptureSources captureSources(CaptureMethod method = CaptureMethod::Auto);
[[nodiscard]] CaptureSources captureWindows(CaptureMethod method = CaptureMethod::Auto);
[[nodiscard]] bool isChosenByTheDesktop(const CaptureSource &source);
[[nodiscard]] bool isHandedOverByTheDesktop(const CaptureSource &source);
void releaseSource(const CaptureSource &source);
void restoreIfMinimised(const CaptureSource &window);

class ScreenCapture {
public:
    struct Request { std::string sourceId; CaptureMethod method = CaptureMethod::Auto; int maxHeight = 0; int framerate = 30; int softwareFormat = -1; bool keepOnCard = false; bool rgbOnCard = false; bool bt601Only = false; };
    enum class Status { Frame, NoFrame, Stalled, Hidden, Closed, Failed };
    ScreenCapture();
    ~ScreenCapture();
    ScreenCapture(const ScreenCapture &) = delete;
    ScreenCapture &operator=(const ScreenCapture &) = delete;
    void configure(const Request &request);
    [[nodiscard]] Status read(AVFrame *into);
    void setPicturesWanted(bool wanted) { picturesWanted = wanted; }
    [[nodiscard]] const std::string &failure() const { return failureText; }
    [[nodiscard]] bool deliversOnlyChanges() const;
    [[nodiscard]] const std::string &captureName() const;
    static constexpr int RETRY_INTERVAL_MS = 250;
    static constexpr int GIVE_UP_AFTER_MS = 5000;
    static constexpr int RESIZE_SETTLE_MS = 300;
    static constexpr int DRAWING_LIMIT_MS = 5000;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    bool picturesWanted = true;
    std::string failureText;
};
} // namespace Styx::Core::Video
