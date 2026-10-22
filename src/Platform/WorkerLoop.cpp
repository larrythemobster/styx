#include "Platform/WorkerLoop.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <objbase.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
using pollfd_t = WSAPOLLFD;
static inline int pollSockets(pollfd_t *fds, ULONG nfds, int timeout) {
    return WSAPoll(fds, nfds, timeout);
}
static inline int closeNativeSocket(Styx::Platform::SocketHandle s) {
    return ::closesocket(static_cast<SOCKET>(s));
}
static inline bool setNonBlocking(Styx::Platform::SocketHandle s) {
    u_long mode = 1;
    return ioctlsocket(static_cast<SOCKET>(s), FIONBIO, &mode) == 0;
}
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
using pollfd_t = struct pollfd;
static inline int pollSockets(pollfd_t *fds, nfds_t nfds, int timeout) {
    return ::poll(fds, nfds, timeout);
}
static inline int closeNativeSocket(Styx::Platform::SocketHandle s) {
    return ::close(s);
}
static inline bool setNonBlocking(Styx::Platform::SocketHandle s) {
    int flags = fcntl(s, F_GETFL, 0);
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif

#include <algorithm>

namespace Styx::Platform {

#ifdef _WIN32
static void ensureWinsock() {
    static const bool initialized = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)initialized;
}
#endif

WorkerLoop::WorkerLoop(std::string_view name, bool initComMta)
    : WorkerLoop(WorkerOptions{ std::string(name), initComMta, nullptr, nullptr })
{
}

WorkerLoop::WorkerLoop(WorkerOptions options)
    : m_options(std::move(options))
{
#ifdef _WIN32
    ensureWinsock();
#endif
    initWakeSockets();
    m_thread = std::thread(&WorkerLoop::run, this);
}

WorkerLoop::~WorkerLoop()
{
    stopAndWait();
    closeWakeSockets();
}

void WorkerLoop::initWakeSockets()
{
    auto rx = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (rx == static_cast<decltype(rx)>(kInvalidSocketHandle))
        return;

    sockaddr_in rxAddr{};
    rxAddr.sin_family = AF_INET;
    rxAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    rxAddr.sin_port = 0;

    if (::bind(rx, reinterpret_cast<sockaddr *>(&rxAddr), sizeof(rxAddr)) != 0) {
        closeNativeSocket(static_cast<SocketHandle>(rx));
        return;
    }

    sockaddr_in boundAddr{};
    socklen_t addrLen = sizeof(boundAddr);
    if (::getsockname(rx, reinterpret_cast<sockaddr *>(&boundAddr), &addrLen) != 0) {
        closeNativeSocket(static_cast<SocketHandle>(rx));
        return;
    }

    setNonBlocking(static_cast<SocketHandle>(rx));
    m_wakeReceiver = static_cast<SocketHandle>(rx);

    auto tx = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (tx == static_cast<decltype(tx)>(kInvalidSocketHandle)) {
        closeNativeSocket(m_wakeReceiver);
        m_wakeReceiver = kInvalidSocketHandle;
        return;
    }

    if (::connect(tx, reinterpret_cast<sockaddr *>(&boundAddr), sizeof(boundAddr)) != 0) {
        closeNativeSocket(static_cast<SocketHandle>(tx));
        closeNativeSocket(m_wakeReceiver);
        m_wakeReceiver = kInvalidSocketHandle;
        return;
    }

    setNonBlocking(static_cast<SocketHandle>(tx));
    m_wakeSender = static_cast<SocketHandle>(tx);
}

void WorkerLoop::closeWakeSockets()
{
    if (m_wakeSender != kInvalidSocketHandle) {
        closeNativeSocket(m_wakeSender);
        m_wakeSender = kInvalidSocketHandle;
    }
    if (m_wakeReceiver != kInvalidSocketHandle) {
        closeNativeSocket(m_wakeReceiver);
        m_wakeReceiver = kInvalidSocketHandle;
    }
}

void WorkerLoop::wake()
{
    if (m_wakeSender != kInvalidSocketHandle) {
        char b = 1;
        ::send(static_cast<SOCKET>(m_wakeSender), &b, 1, 0);
    }
}

void WorkerLoop::drainWakeSocket()
{
    if (m_wakeReceiver != kInvalidSocketHandle) {
        char buf[256];
        while (::recv(static_cast<SOCKET>(m_wakeReceiver), buf, sizeof(buf), 0) > 0) {}
    }
}

void WorkerLoop::post(std::function<void()> task)
{
    if (!task)
        return;
    {
        std::lock_guard lock(m_mutex);
        m_tasks.push_back(std::move(task));
    }
    wake();
}

TimerId WorkerLoop::setTimer(std::chrono::milliseconds interval, bool repeating, std::function<void()> callback)
{
    if (!callback)
        return 0;
    TimerId id = 0;
    {
        std::lock_guard lock(m_mutex);
        id = m_nextTimerId++;
        auto deadline = std::chrono::steady_clock::now() + interval;
        m_timers.push_back({ id, deadline, interval, repeating, std::move(callback) });
    }
    wake();
    return id;
}

void WorkerLoop::cancelTimer(TimerId id)
{
    if (id == 0)
        return;
    {
        std::lock_guard lock(m_mutex);
        for (auto it = m_timers.begin(); it != m_timers.end(); ++it) {
            if (it->id == id) {
                m_timers.erase(it);
                break;
            }
        }
    }
    wake();
}

void WorkerLoop::watchSocket(SocketHandle fd, SocketMode mode, std::function<void(SocketMode)> callback)
{
    if (fd == kInvalidSocketHandle || !callback)
        return;
    {
        std::lock_guard lock(m_mutex);
        m_watchedSockets[fd] = WatchedItem{ fd, mode, std::move(callback) };
    }
    wake();
}

void WorkerLoop::watchSocket(SocketHandle fd, std::function<void()> onReadable)
{
    if (!onReadable)
        return;
    watchSocket(fd, SocketMode::Read, [cb = std::move(onReadable)](SocketMode) {
        cb();
    });
}

void WorkerLoop::unwatchSocket(SocketHandle fd)
{
    if (fd == kInvalidSocketHandle)
        return;
    {
        std::lock_guard lock(m_mutex);
        m_watchedSockets.erase(fd);
    }
    wake();
}

void WorkerLoop::stop()
{
    m_stopping.store(true, std::memory_order_release);
    wake();
}

void WorkerLoop::join()
{
    if (m_thread.joinable()) {
        if (std::this_thread::get_id() == m_thread.get_id())
            return;
        m_thread.join();
    }
}

void WorkerLoop::stopAndWait()
{
    stop();
    join();
}

bool WorkerLoop::isCurrentThread() const
{
    return std::this_thread::get_id() == m_thread.get_id();
}

bool WorkerLoop::isRunning() const
{
    return !m_stopping.load(std::memory_order_acquire);
}

std::thread::id WorkerLoop::threadId() const
{
    return m_thread.get_id();
}

void WorkerLoop::run()
{
    if (m_options.initComMta) {
#ifdef _WIN32
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
    }
    if (m_options.onThreadStart) {
        m_options.onThreadStart();
    }

    std::vector<pollfd_t> pollFds;
    std::vector<std::function<void()>> tasksToRun;
    std::vector<std::function<void()>> timersToRun;
    std::vector<std::pair<std::function<void(SocketMode)>, SocketMode>> socketCallbacksToRun;

    while (!m_stopping.load(std::memory_order_acquire)) {
        // 1. Drain posted tasks
        {
            std::lock_guard lock(m_mutex);
            if (!m_tasks.empty())
                tasksToRun.swap(m_tasks);
        }
        for (auto &task : tasksToRun) {
            task();
        }
        tasksToRun.clear();

        // 2. Check and fire timers
        auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard lock(m_mutex);
            for (auto it = m_timers.begin(); it != m_timers.end(); ) {
                if (it->deadline <= now) {
                    timersToRun.push_back(it->callback);
                    if (it->repeating) {
                        it->deadline = now + it->interval;
                        ++it;
                    } else {
                        it = m_timers.erase(it);
                    }
                } else {
                    ++it;
                }
            }
        }
        for (auto &cb : timersToRun) {
            cb();
        }
        timersToRun.clear();

        if (m_stopping.load(std::memory_order_acquire))
            break;

        // 3. Compute next poll timeout and prepare pollfd array
        int timeoutMs = -1;
        {
            std::lock_guard lock(m_mutex);
            if (!m_tasks.empty()) {
                timeoutMs = 0;
            } else if (!m_timers.empty()) {
                auto earliest = m_timers.front().deadline;
                for (const auto &t : m_timers) {
                    if (t.deadline < earliest)
                        earliest = t.deadline;
                }
                auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(earliest - std::chrono::steady_clock::now()).count();
                timeoutMs = static_cast<int>(std::max<int64_t>(0, diff));
            }

            pollFds.clear();
            if (m_wakeReceiver != kInvalidSocketHandle) {
                pollfd_t wakePfd{};
                wakePfd.fd = static_cast<SOCKET>(m_wakeReceiver);
                wakePfd.events = POLLIN;
                pollFds.push_back(wakePfd);
            }

            for (const auto &[fd, item] : m_watchedSockets) {
                pollfd_t pfd{};
                pfd.fd = static_cast<SOCKET>(fd);
                pfd.events = 0;
                if (hasFlag(item.mode, SocketMode::Read)) pfd.events |= POLLIN;
                if (hasFlag(item.mode, SocketMode::Write)) pfd.events |= POLLOUT;
                pollFds.push_back(pfd);
            }
        }

        if (pollFds.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        int res = pollSockets(pollFds.data(), static_cast<ULONG>(pollFds.size()), timeoutMs);
        if (res > 0) {
            size_t idx = 0;
            if (m_wakeReceiver != kInvalidSocketHandle) {
                if (pollFds[0].revents & (POLLIN | POLLERR | POLLHUP))
                    drainWakeSocket();
                idx = 1;
            }

            socketCallbacksToRun.clear();
            {
                std::lock_guard lock(m_mutex);
                for (; idx < pollFds.size(); ++idx) {
                    short rev = pollFds[idx].revents;
                    if (rev == 0)
                        continue;
                    SocketHandle fd = static_cast<SocketHandle>(pollFds[idx].fd);
                    auto it = m_watchedSockets.find(fd);
                    if (it != m_watchedSockets.end()) {
                        SocketMode occurred = SocketMode::None;
                        if (rev & (POLLIN | POLLERR | POLLHUP))
                            occurred = occurred | SocketMode::Read;
                        if (rev & POLLOUT)
                            occurred = occurred | SocketMode::Write;
                        socketCallbacksToRun.emplace_back(it->second.callback, occurred);
                    }
                }
            }
            for (auto &[cb, mode] : socketCallbacksToRun) {
                cb(mode);
            }
            socketCallbacksToRun.clear();
        }
    }

    // Final drain of posted tasks
    {
        std::lock_guard lock(m_mutex);
        tasksToRun.swap(m_tasks);
    }
    for (auto &task : tasksToRun) {
        task();
    }
    tasksToRun.clear();

    if (m_options.onThreadStop) {
        m_options.onThreadStop();
    }
    if (m_options.initComMta) {
#ifdef _WIN32
        CoUninitialize();
#endif
    }
}

} // namespace Styx::Platform
