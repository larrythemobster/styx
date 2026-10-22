#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace Styx::Platform {

class ThreadPool {
public:
    explicit ThreadPool(size_t threadCount = 4, std::string name = "");
    ~ThreadPool();

    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

    void post(std::function<void()> task);
    void stopAndWait();
    void clear();

private:
    void worker();

    std::vector<std::thread> m_threads;
    std::queue<std::function<void()>> m_tasks;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stopping = false;
    std::string m_name;
};

} // namespace Styx::Platform
