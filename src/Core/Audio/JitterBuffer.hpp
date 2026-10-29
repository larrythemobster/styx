#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace Styx {
namespace Core {
namespace Audio {

class JitterBuffer
{
public:
    explicit JitterBuffer(int capacity = 10);

    /// Add a frame to the buffer.
    void push(uint16_t sequence, const std::string &data);

    /// Get the next frame in sequence order, or an empty string for packet loss.
    std::string pop();

    /// Reset the buffer state.
    void reset();

    /// Returns true once the buffer has pre-buffered enough frames for playback.
    [[nodiscard]] bool isReady() const { return initialized && !prebuffering; }

private:
    /// Returns true if sequence a is "newer" than b (handles 16-bit wraparound).
    static bool seqNewer(uint16_t a, uint16_t b);

    std::unordered_map<uint16_t, std::string> frames;
    uint16_t nextSequence = 0;
    bool initialized = false;
    int capacity;

    int targetDelay = 3;
    bool prebuffering = true;
    int consecutiveMisses = 0;
    int consecutiveHits = 0;

    static constexpr int MAX_CONSECUTIVE_MISSES = 25;
};

} // namespace Audio
} // namespace Core
} // namespace Styx
