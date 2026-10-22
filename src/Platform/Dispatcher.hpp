#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace Styx::Platform {

using TimerId = uint64_t;

class Dispatcher {
public:
    static Dispatcher &main();
    static Dispatcher &instance() { return main(); }

    Dispatcher();
    ~Dispatcher();

    Dispatcher(const Dispatcher &) = delete;
    Dispatcher &operator=(const Dispatcher &) = delete;

    // Post a task to be executed on the main thread during runPending().
    // Thread-safe and non-blocking.
    void post(std::function<void()> task);

    // Register a wake hook invoked whenever a task is posted.
    // Useful for host loops (e.g. SDL event push, win32 PostMessage, Qt QEvent).
    void setWakeHook(std::function<void()> hook);

    // Set a timer. Can be single-shot or repeating.
    // Thread-safe. Callback is invoked on the thread running runPending().
    TimerId setTimer(std::chrono::milliseconds interval, bool repeating, std::function<void()> callback);
    TimerId setTimer(uint32_t intervalMs, bool repeating, std::function<void()> callback) {
        return setTimer(std::chrono::milliseconds(intervalMs), repeating, std::move(callback));
    }

    // Cancel a pending timer by ID. Thread-safe.
    void cancelTimer(TimerId id);

    // Run all pending tasks and trigger all expired timers.
    // Must be called from the host loop's main thread.
    void runPending();

    // Convenience alias for runPending()
    void poll() { runPending(); }

    // Returns the duration until the next timer deadline, or nullopt if no active timers.
    // Returns 0ms if there are queued tasks or a timer has already elapsed.
    std::optional<std::chrono::milliseconds> nextTimeout() const;

    // Drain all posted tasks without running timers.
    void drainTasks();

    // Returns true if there are tasks currently pending in the queue.
    bool hasPendingTasks() const;

private:
    struct TimerItem {
        TimerId id;
        std::chrono::steady_clock::time_point deadline;
        std::chrono::milliseconds interval;
        bool repeating;
        std::function<void()> callback;
    };

    mutable std::mutex m_mutex;
    std::vector<std::function<void()>> m_tasks;
    std::vector<TimerItem> m_timers;
    std::function<void()> m_wakeHook;
    TimerId m_nextTimerId{1};
};

template <typename F>
inline void postToMain(F &&fn) {
    Dispatcher::main().post(std::forward<F>(fn));
}

} // namespace Styx::Platform
