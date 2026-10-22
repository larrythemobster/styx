#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <atomic>

namespace Styx::Platform {

using TimerId = uint64_t;

#ifdef _WIN32
using SocketHandle = uintptr_t;
constexpr SocketHandle kInvalidSocketHandle = ~static_cast<SocketHandle>(0);
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocketHandle = -1;
#endif

enum class SocketMode : uint32_t {
    None = 0,
    Read = 1 << 0,
    Write = 1 << 1,
    ReadWrite = Read | Write
};

inline SocketMode operator|(SocketMode a, SocketMode b) {
    return static_cast<SocketMode>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline SocketMode operator&(SocketMode a, SocketMode b) {
    return static_cast<SocketMode>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline bool hasFlag(SocketMode mask, SocketMode flag) {
    return (static_cast<uint32_t>(mask) & static_cast<uint32_t>(flag)) != 0;
}

struct WorkerOptions {
    std::string name;
    bool initComMta = false;
    std::function<void()> onThreadStart;
    std::function<void()> onThreadStop;
};

class WorkerLoop {
public:
    explicit WorkerLoop(std::string_view name = "", bool initComMta = false);
    explicit WorkerLoop(WorkerOptions options);
    ~WorkerLoop();

    WorkerLoop(const WorkerLoop &) = delete;
    WorkerLoop &operator=(const WorkerLoop &) = delete;

    // Post a task to be executed on the worker thread.
    // Thread-safe and non-blocking.
    void post(std::function<void()> task);

    // Schedule a timer. Single-shot or repeating.
    // Thread-safe. Callback executes on the worker thread.
    TimerId setTimer(std::chrono::milliseconds interval, bool repeating, std::function<void()> callback);
    TimerId setTimer(uint32_t intervalMs, bool repeating, std::function<void()> callback) {
        return setTimer(std::chrono::milliseconds(intervalMs), repeating, std::move(callback));
    }

    // Cancel a timer by ID. Thread-safe.
    void cancelTimer(TimerId id);

    // Watch a socket for readiness events.
    // Callback executes on the worker thread.
    void watchSocket(SocketHandle fd, SocketMode mode, std::function<void(SocketMode)> callback);
    void watchSocket(SocketHandle fd, std::function<void()> onReadable);

    // Stop watching a socket. Thread-safe.
    void unwatchSocket(SocketHandle fd);

    // Stop the worker loop and signal the thread to exit.
    void stop();

    // Wait for the worker thread to finish.
    void join();

    // Stop and wait (safe to call multiple times or from destructor).
    void stopAndWait();

    // Check if the caller is running on the worker loop thread.
    [[nodiscard]] bool isCurrentThread() const;

    // Check if the loop is still active.
    [[nodiscard]] bool isRunning() const;

    // Thread ID of the worker loop.
    [[nodiscard]] std::thread::id threadId() const;

private:
    struct TimerItem {
        TimerId id;
        std::chrono::steady_clock::time_point deadline;
        std::chrono::milliseconds interval;
        bool repeating;
        std::function<void()> callback;
    };

    struct WatchedItem {
        SocketHandle fd;
        SocketMode mode;
        std::function<void(SocketMode)> callback;
    };

    void run();
    void wake();
    void initWakeSockets();
    void closeWakeSockets();
    void drainWakeSocket();

    WorkerOptions m_options;
    mutable std::mutex m_mutex;
    std::atomic<bool> m_stopping{false};
    std::vector<std::function<void()>> m_tasks;
    std::vector<TimerItem> m_timers;
    std::unordered_map<SocketHandle, WatchedItem> m_watchedSockets;
    TimerId m_nextTimerId{1};

    SocketHandle m_wakeReceiver{kInvalidSocketHandle};
    SocketHandle m_wakeSender{kInvalidSocketHandle};

    std::thread m_thread;
};

} // namespace Styx::Platform
