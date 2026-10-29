#include "IngestThread.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#include "Core/Logging.hpp"

namespace Styx::Discord {

IngestThread::IngestThread() = default;
IngestThread::~IngestThread()
{
    stop();
    if (streamActive)
        inflateEnd(&stream);
}

void IngestThread::start()
{
    if (running.exchange(true)) {
        Core::log(Core::LogLevel::Debug, LogNetwork) << "Attempt to start already running IngestThread";
        return;
    }
    thread = std::thread(&IngestThread::threadLoop, this);
}

void IngestThread::stop()
{
    {
        std::lock_guard lock(mutex);
        running = false;
    }
    cv.notify_one();
    if (thread.joinable())
        thread.join();
}

void IngestThread::push(const std::string &data)
{
    {
        std::lock_guard lock(mutex);
        queue.push_back({data, generation.load()});
    }
    cv.notify_one();
}

void IngestThread::reset()
{
    std::lock_guard lock(mutex);
    ++generation;
    queue.clear();
}

void IngestThread::threadLoop()
{
    uint64_t activeGeneration = generation.load();
    std::string compressedTail;
    while (true) {
        TaggedData item;
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [this] { return !queue.empty() || !running; });
            if (!running)
                break;
            item = std::move(queue.front());
            queue.pop_front();
        }
        if (item.generation != activeGeneration) {
            if (streamActive) {
                inflateEnd(&stream);
                streamActive = false;
            }
            std::memset(&stream, 0, sizeof(stream));
            decompressedBuffer.clear();
            compressedTail.clear();
            activeGeneration = item.generation;
        }
        if (item.data.empty())
            continue;
        if (!streamActive) {
            if (inflateInit2(&stream, MAX_WBITS + 32) != Z_OK) {
                Core::log(Core::LogLevel::Warning, LogNetwork) << "inflateInit2 failed";
                continue;
            }
            streamActive = true;
        }
        std::string suffix = compressedTail;
        suffix.append(item.data, 0, std::min<std::size_t>(item.data.size(), 4));
        const bool splitSuffix = item.data.size() < 4 &&
            suffix.size() >= 4 && suffix.compare(suffix.size() - 4, 4, "\x00\x00\xff\xff", 4) == 0;
        const bool hasSuffix = splitSuffix ||
            (item.data.size() >= 4 && item.data.compare(item.data.size() - 4, 4, "\x00\x00\xff\xff", 4) == 0);
        if (item.data.size() >= 3)
            compressedTail.assign(item.data, item.data.size() - 3, 3);
        else {
            compressedTail += item.data;
            if (compressedTail.size() > 3)
                compressedTail.erase(0, compressedTail.size() - 3);
        }
        stream.next_in = reinterpret_cast<Bytef *>(item.data.data());
        stream.avail_in = static_cast<uInt>(item.data.size());
        std::array<char, 32768> out;
        int ret;
        do {
            stream.avail_out = static_cast<uInt>(out.size());
            stream.next_out = reinterpret_cast<Bytef *>(out.data());
            ret = inflate(&stream, Z_SYNC_FLUSH);
            if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
                Core::log(Core::LogLevel::Warning, LogNetwork) << "inflate failed: " << ret;
                inflateEnd(&stream);
                streamActive = false;
                decompressedBuffer.clear();
                compressedTail.clear();
                break;
            }
            decompressedBuffer.append(out.data(), out.size() - stream.avail_out);
        } while (stream.avail_out == 0);
        if (hasSuffix && streamActive) {
            auto doc = nlohmann::json::parse(decompressedBuffer, nullptr, false);
            decompressedBuffer.clear();
            if (doc.is_discarded())
                Core::log(Core::LogLevel::Warning, LogNetwork) << "Failed to parse gateway message";
            else if (doc.is_object() && item.generation == generation.load())
                payloadReceived.publish(std::move(doc), item.generation);
        }
    }
}

} // namespace Styx::Discord
