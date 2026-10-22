#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct AVFrame;

namespace Styx::Core::Video {

struct CameraDevice {
    std::string id;
    std::string name;
};

class CameraFrameSource {
public:
    enum class Status { Frame, NoFrame, Ended };
    virtual ~CameraFrameSource() = default;
    [[nodiscard]] virtual bool open(const std::string &deviceId) = 0;
    [[nodiscard]] virtual Status read(AVFrame *into, std::chrono::steady_clock::time_point &capturedAt) = 0;
    [[nodiscard]] virtual std::string failure() const = 0;
    [[nodiscard]] virtual std::string description() const = 0;
};

using CameraFrameSourceFactory = std::function<std::unique_ptr<CameraFrameSource>()>;
void setCameraFrameSourceFactory(CameraFrameSourceFactory factory);
[[nodiscard]] std::unique_ptr<CameraFrameSource> makeCameraFrameSource();
[[nodiscard]] bool cameraCaptureAvailable();
[[nodiscard]] std::vector<CameraDevice> cameraDevices();

class CaptureClock {
public:
    [[nodiscard]] std::chrono::steady_clock::time_point at(
        std::optional<std::chrono::microseconds> stamped,
        std::chrono::steady_clock::time_point readAt);

    static constexpr std::chrono::milliseconds STARTED_OVER{1000};
    static constexpr int SLOWEST_CLOCK_FOLLOWED = 5000;

private:
    std::optional<std::chrono::steady_clock::duration> ourClockAhead;
    std::chrono::steady_clock::time_point lastReadAt;
    std::optional<std::chrono::steady_clock::time_point> last;
};

#if defined(STYX_HAVE_AVDEVICE) && (defined(_WIN32) || defined(__linux__))
class CameraCapture final : public CameraFrameSource {
public:
    CameraCapture();
    ~CameraCapture() override;
    CameraCapture(const CameraCapture &) = delete;
    CameraCapture &operator=(const CameraCapture &) = delete;

    [[nodiscard]] bool open(const std::string &deviceId) override;
    [[nodiscard]] Status read(AVFrame *into, std::chrono::steady_clock::time_point &capturedAt) override;
    [[nodiscard]] std::string failure() const override;
    [[nodiscard]] std::string description() const override;

    static constexpr int READ_WAIT_MS = 100;
    static constexpr int NO_PICTURE_TIMEOUT_MS = 5000;

private:
    enum class State { Opening, Open, Ended };
    void run(std::string deviceId);
    void opened(std::string openedMode);
    void end(std::string reason);
    void publish(AVFrame *picture, std::chrono::steady_clock::time_point capturedAt);

    mutable std::mutex mutex;
    std::condition_variable changed;
    State state = State::Opening;
    std::string failureText;
    std::string mode;
    AVFrame *newest = nullptr;
    std::chrono::steady_clock::time_point newestCapturedAt{};
    std::atomic_bool stopping{false};
    std::thread thread;
};
#endif

} // namespace Styx::Core::Video
