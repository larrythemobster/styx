// Ported from ouwou/Acheron video-streams commit 71b4907 (GPL-3.0-or-later).
#pragma once

#include "Platform/Signal.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Styx::Core::Audio {

inline constexpr int LOOPBACK_SAMPLE_RATE = 48000;
inline constexpr int LOOPBACK_CHANNELS = 2;
inline constexpr int LOOPBACK_SAMPLE_BYTES = 2;
inline constexpr int LOOPBACK_FRAME_DURATION_MS = 20;
inline constexpr int LOOPBACK_FRAME_SAMPLES = LOOPBACK_SAMPLE_RATE * LOOPBACK_FRAME_DURATION_MS / 1000;
inline constexpr int LOOPBACK_FRAME_SIZE = LOOPBACK_FRAME_SAMPLES * LOOPBACK_CHANNELS * LOOPBACK_SAMPLE_BYTES;
inline constexpr int AUDIO_SAMPLE_RATE = LOOPBACK_SAMPLE_RATE;
inline constexpr int AUDIO_CHANNELS = LOOPBACK_CHANNELS;
inline constexpr int AUDIO_SAMPLE_BYTES = LOOPBACK_SAMPLE_BYTES;
inline constexpr int AUDIO_FRAME_DURATION_MS = LOOPBACK_FRAME_DURATION_MS;
inline constexpr int AUDIO_FRAME_SAMPLES = LOOPBACK_FRAME_SAMPLES;
inline constexpr int AUDIO_FRAME_SIZE = LOOPBACK_FRAME_SIZE;

// Whether this machine can capture what one program is playing, or everything but one program.
[[nodiscard]] bool processLoopbackAvailable();

struct PlayingProgram {
    std::uint32_t processId = 0;
    std::string name;
};

// Programs with an open sound output, excluding Styx. Empty where the platform cannot report it.
[[nodiscard]] std::vector<PlayingProgram> playingPrograms();

// Captures 20 ms frames of 48 kHz interleaved stereo signed 16-bit PCM. frameCaptured and failed
// run on the backend worker thread. frameCaptured owns its buffer, so queued Signal connections
// remain valid. start() and stop() are non-blocking; one final frame can arrive after stop().
class LoopbackCapture {
public:
    enum class Scope {
        ProcessTree,
        AllButProcessTree,
    };

    LoopbackCapture();
    ~LoopbackCapture();
    LoopbackCapture(const LoopbackCapture &) = delete;
    LoopbackCapture &operator=(const LoopbackCapture &) = delete;

    void start(std::uint32_t processId, Scope scope);
    void stop();

    Platform::Signal<std::vector<std::uint8_t>> frameCaptured;
    Platform::Signal<std::string> failed;

private:
    class Worker;
    std::unique_ptr<Worker> worker;
};
} // namespace Styx::Core::Audio
