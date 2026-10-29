#pragma once

#include <zlib.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "Platform/Signal.hpp"

namespace Styx::Discord {

class IngestThread
{
public:
    IngestThread();
    ~IngestThread();
    void start();
    void stop();
    void push(const std::string &data);
    void reset();

    Platform::Signal<nlohmann::json, uint64_t> payloadReceived;
    uint64_t currentGeneration() const { return generation.load(); }

private:
    struct TaggedData { std::string data; uint64_t generation; };
    void threadLoop();
    std::thread thread;
    std::atomic<bool> running{false};
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<TaggedData> queue;
    std::string decompressedBuffer;
    z_stream stream{};
    bool streamActive = false;
    std::atomic<uint64_t> generation{0};
};

} // namespace Styx::Discord
