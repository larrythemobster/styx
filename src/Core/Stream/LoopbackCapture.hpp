#pragma once

#ifdef _WIN32

#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace Styx {
namespace Core {
namespace Stream {

// Stream audio through WASAPI process loopback (Windows 10 2004+). Either one application's
// process tree (sharing a window) or everything except our own process tree (sharing a
// screen), so viewers never hear the voice chat played back by this client.
// Delivers 48 kHz interleaved stereo int16 on its own thread.
class LoopbackCapture
{
public:
    enum class Mode {
        IncludeProcess,
        ExcludeProcess,
    };

    using Callback = std::function<void(const int16_t *samples, int frames)>;

    LoopbackCapture() = default;
    ~LoopbackCapture();

    LoopbackCapture(const LoopbackCapture &) = delete;
    LoopbackCapture &operator=(const LoopbackCapture &) = delete;

    static bool isSupported();

    bool start(Mode mode, quint32 processId, Callback callback, QString *error);
    void stop();

    static constexpr int SAMPLE_RATE = 48000;
    static constexpr int CHANNELS = 2;

private:
    void run(Mode mode, quint32 processId);

    Callback callback;
    std::thread thread;
    std::atomic<bool> running{ false };
    std::atomic<int> startState{ 0 }; // 0 pending, 1 ok, -1 failed
    QString startError;
};

} // namespace Stream
} // namespace Core
} // namespace Styx

#endif
