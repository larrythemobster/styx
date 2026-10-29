#include "JitterBuffer.hpp"

namespace Styx {
namespace Core {
namespace Audio {

bool JitterBuffer::seqNewer(uint16_t a, uint16_t b)
{
    return static_cast<int16_t>(a - b) > 0;
}

JitterBuffer::JitterBuffer(int capacity)
    : capacity(capacity)
{
}

void JitterBuffer::push(uint16_t sequence, const std::string &data)
{
    if (!initialized) {
        nextSequence = sequence;
        initialized = true;
    }

    // If the sequence is way ahead of what we expect, the speaker resumed
    // after a long silence. Reset and resync.
    if (seqNewer(sequence, static_cast<uint16_t>(nextSequence + capacity * 10))) {
        reset();
        nextSequence = sequence;
        initialized = true;
    }

    // Discard if too old (already played or beyond buffer capacity)
    if (seqNewer(nextSequence, sequence))
        return;

    frames[sequence] = data;

    // Evict frames that are too far behind the playback pointer.
    for (auto it = frames.begin(); it != frames.end();) {
        if (seqNewer(nextSequence, static_cast<uint16_t>(it->first + capacity)))
            it = frames.erase(it);
        else
            ++it;
    }

    if (prebuffering && frames.size() >= targetDelay)
        prebuffering = false;
}

std::string JitterBuffer::pop()
{
    if (!initialized)
        return {};

    if (prebuffering)
        return {};

    std::string data;
    auto it = frames.find(nextSequence);
    if (it != frames.end()) {
        data = std::move(it->second);
        frames.erase(it);
    }
    nextSequence++;

    if (!data.empty()) {
        consecutiveHits++;
        consecutiveMisses = 0;

        if (consecutiveHits >= 200 && targetDelay > 2) {
            targetDelay--;
            consecutiveHits = 0;
        }
    } else {
        consecutiveMisses++;
        consecutiveHits = 0;

        if (consecutiveMisses >= 3 && targetDelay < 8) {
            targetDelay++;
            prebuffering = true;
            consecutiveMisses = 0;
        }

        if (consecutiveMisses >= MAX_CONSECUTIVE_MISSES) {
            reset();
        }
    }

    return data;
}

void JitterBuffer::reset()
{
    frames.clear();
    nextSequence = 0;
    initialized = false;
    targetDelay = 3;
    prebuffering = true;
    consecutiveMisses = 0;
    consecutiveHits = 0;
}

} // namespace Audio
} // namespace Core
} // namespace Styx
