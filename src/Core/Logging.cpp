#include "Logging.hpp"
#include "Platform/Paths.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace Styx::Core {
namespace {
constexpr std::uintmax_t maxLogFileSize = 10 * 1024 * 1024;
constexpr int maxBackupFiles = 3;
struct LogState {
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<std::string> queue;
    std::thread writer;
    std::ofstream file;
    std::filesystem::path path;
    bool stopping = false;
    bool running = false;
    ~LogState() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        ready.notify_one();
        if (writer.joinable()) writer.join();
    }
};
LogState &state() { static LogState value; return value; }

void rotate(LogState &s)
{
    s.file.close();
    std::error_code error;
    auto backup = [&](int number) { auto p = s.path; p += "." + std::to_string(number); return p; };
    std::filesystem::remove(backup(maxBackupFiles), error);
    for (int i = maxBackupFiles - 1; i > 0; --i) {
        error.clear();
        if (std::filesystem::exists(backup(i), error))
            std::filesystem::rename(backup(i), backup(i + 1), error);
    }
    error.clear();
    std::filesystem::rename(s.path, backup(1), error);
    s.file.open(s.path, std::ios::binary | std::ios::app);
}

void writerLoop(LogState &s)
{
    std::vector<std::string> batch;
    for (;;) {
        {
            std::unique_lock lock(s.mutex);
            s.ready.wait(lock, [&] { return s.stopping || !s.queue.empty(); });
            if (s.queue.empty() && s.stopping) break;
            batch.swap(s.queue);
        }
        if (s.file) {
            for (const auto &line : batch) {
                s.file.write(line.data(), static_cast<std::streamsize>(line.size()));
                s.file.put('\n');
            }
            s.file.flush();
            const auto position = s.file.tellp();
            if (position >= 0 && static_cast<std::uintmax_t>(position) >= maxLogFileSize) rotate(s);
        }
        batch.clear();
    }
}
}

void Logger::init()
{
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (s.running) return;
    s.path = Platform::Paths::dataDir() / "styx.log";
    std::filesystem::create_directories(s.path.parent_path());
    s.file.clear();
    s.file.open(s.path, std::ios::binary | std::ios::app);
    if (s.file) {
        const auto position = s.file.tellp();
        if (position >= 0 && static_cast<std::uintmax_t>(position) >= maxLogFileSize) rotate(s);
        s.file << "\n=== Styx startup: "
               << std::format("{:%F %T}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()))
               << " ===\n";
        s.file.flush();
    } else {
        std::cerr << "Cannot open Styx log file\n";
    }
    s.stopping = false;
    s.writer = std::thread([&s] { writerLoop(s); });
    s.running = true;
}

void Logger::cleanup()
{
    auto &s = state();
    {
        std::lock_guard lock(s.mutex);
        if (!s.running) return;
        s.stopping = true;
    }
    s.ready.notify_one();
    s.writer.join();
    std::lock_guard lock(s.mutex);
    s.running = false;
    s.file.close();
}

void Logger::write(LogLevel level, std::string_view category, std::string message)
{
    constexpr std::string_view levels[] = { "DBG", "INF", "WRN", "ERR" };
    auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    auto formatted = std::format("[{:%F %T}] [T:{:x}] [{}] [{}] {}", now,
        std::hash<std::thread::id>{}(std::this_thread::get_id()), levels[static_cast<unsigned>(level)], category, message);
    auto &s = state();
    {
        std::lock_guard lock(s.mutex);
        std::clog << formatted << '\n';
        if (!s.running || s.stopping) return;
        s.queue.push_back(std::move(formatted));
    }
    s.ready.notify_one();
}

LogLine::~LogLine() noexcept
{
    try { Logger::write(level_, category_, stream_.str()); }
    catch (...) { std::fputs("Styx logging failed\n", stderr); }
}
}
