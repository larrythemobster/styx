#include "Dispatcher.hpp"

#include <algorithm>

namespace Styx::Platform {

Dispatcher &Dispatcher::main()
{
    static Dispatcher s_main;
    return s_main;
}

Dispatcher::Dispatcher() = default;
Dispatcher::~Dispatcher() = default;

void Dispatcher::post(std::function<void()> task)
{
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tasks.push_back(std::move(task));
        hook = m_wakeHook;
    }
    if (hook) {
        hook();
    }
}

void Dispatcher::setWakeHook(std::function<void()> hook)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_wakeHook = std::move(hook);
}

TimerId Dispatcher::setTimer(std::chrono::milliseconds interval, bool repeating, std::function<void()> callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    TimerId id = m_nextTimerId++;
    TimerItem item;
    item.id = id;
    item.interval = interval;
    item.repeating = repeating;
    item.callback = std::move(callback);
    item.deadline = std::chrono::steady_clock::now() + interval;
    m_timers.push_back(std::move(item));
    return id;
}

void Dispatcher::cancelTimer(TimerId id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_timers.erase(
        std::remove_if(m_timers.begin(), m_timers.end(), [id](const TimerItem &t) { return t.id == id; }),
        m_timers.end());
}

void Dispatcher::drainTasks()
{
    std::vector<std::function<void()>> toRun;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        toRun.swap(m_tasks);
    }
    for (auto &task : toRun) {
        if (task) {
            task();
        }
    }
}

bool Dispatcher::hasPendingTasks() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_tasks.empty();
}

void Dispatcher::runPending()
{
    drainTasks();

    auto now = std::chrono::steady_clock::now();
    std::vector<std::function<void()>> callbacks;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<TimerItem> remaining;
        remaining.reserve(m_timers.size());

        for (auto &timer : m_timers) {
            if (timer.deadline <= now) {
                callbacks.push_back(timer.callback);
                if (timer.repeating) {
                    timer.deadline = now + timer.interval;
                    remaining.push_back(std::move(timer));
                }
            } else {
                remaining.push_back(std::move(timer));
            }
        }
        m_timers = std::move(remaining);
    }

    for (auto &cb : callbacks) {
        if (cb) {
            cb();
        }
    }

    // Drain any tasks that might have been queued by timer callbacks
    drainTasks();
}

std::optional<std::chrono::milliseconds> Dispatcher::nextTimeout() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_tasks.empty()) {
        return std::chrono::milliseconds(0);
    }
    if (m_timers.empty()) {
        return std::nullopt;
    }

    auto now = std::chrono::steady_clock::now();
    auto minDeadline = m_timers.front().deadline;
    for (const auto &t : m_timers) {
        if (t.deadline < minDeadline) {
            minDeadline = t.deadline;
        }
    }

    if (minDeadline <= now) {
        return std::chrono::milliseconds(0);
    }

    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(minDeadline - now);
    return diff;
}

} // namespace Styx::Platform
