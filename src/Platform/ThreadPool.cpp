#include "Platform/ThreadPool.hpp"

namespace Styx::Platform {

ThreadPool::ThreadPool(size_t threadCount, std::string name)
    : m_name(std::move(name))
{
    if (threadCount == 0)
        threadCount = 1;
    m_threads.reserve(threadCount);
    for (size_t i = 0; i < threadCount; ++i) {
        m_threads.emplace_back(&ThreadPool::worker, this);
    }
}

ThreadPool::~ThreadPool()
{
    stopAndWait();
}

void ThreadPool::post(std::function<void()> task)
{
    if (!task)
        return;
    {
        std::lock_guard lock(m_mutex);
        if (m_stopping)
            return;
        m_tasks.push(std::move(task));
    }
    m_cv.notify_one();
}

void ThreadPool::clear()
{
    std::lock_guard lock(m_mutex);
    std::queue<std::function<void()>> empty;
    std::swap(m_tasks, empty);
}

void ThreadPool::stopAndWait()
{
    {
        std::lock_guard lock(m_mutex);
        if (m_stopping)
            return;
        m_stopping = true;
    }
    m_cv.notify_all();
    for (auto &t : m_threads) {
        if (t.joinable())
            t.join();
    }
    m_threads.clear();
}

void ThreadPool::worker()
{
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this] {
                return m_stopping || !m_tasks.empty();
            });
            if (m_stopping && m_tasks.empty())
                return;
            task = std::move(m_tasks.front());
            m_tasks.pop();
        }
        if (task) {
            task();
        }
    }
}

} // namespace Styx::Platform
