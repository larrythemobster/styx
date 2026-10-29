// Ported from ouwou/Acheron video-streams commit 71b4907 (GPL-3.0-or-later).
#pragma once

#include "Core/Audio/LoopbackCapture.hpp"

#include <cstdint>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

namespace Styx::Core {

// Whether a frame of 16-bit samples has nothing in it to hear. Frames are captured for as long
// as anything at all is played on the machine, and silence does not come as zeros: it is the
// noise that rounding to 16 bits leaves, which was measured at a root mean square of 0.5 for a
// program that plays nothing and of up to 1.8 for everything but one program.
[[nodiscard]] inline bool isSilence(std::span<const std::uint8_t> pcm)
{
    constexpr std::int64_t LOUDEST_SILENCE_RMS = 6;

    const auto *samples = reinterpret_cast<const std::int16_t *>(pcm.data());
    const std::int64_t count = static_cast<std::int64_t>(pcm.size() / sizeof(std::int16_t));
    if (count == 0)
        return true;
    const std::int64_t squares = std::accumulate(samples, samples + count, std::int64_t(0), [](std::int64_t sum, std::int16_t sample) {
        return sum + std::int64_t(sample) * sample;
    });
    return squares <= LOUDEST_SILENCE_RMS * LOUDEST_SILENCE_RMS * count;
}

[[nodiscard]] inline bool isSilence(const std::vector<std::uint8_t> &pcm)
{
    return isSilence(std::span<const std::uint8_t>(pcm));
}

// Cuts what is captured into runs of sound, as the microphone's frames are cut into talk
// spurts: a run starts with the first frame that has something in it and goes on through a
// short silence, so that a pause in the music does not end it.
class SoundRuns
{
public:
    enum class Step {
        // Silence outside a run. Nothing is sent for it.
        Skip,
        // The first frame of a run.
        Begin,
        Continue,
        // The silence has lasted too long: the run is over, and this frame is not part of it.
        End,
    };

    // As long as a talk spurt is held open after the last of the voice.
    static constexpr int HELD_SILENT_FRAMES = 25;

    Step offer(bool silent)
    {
        if (!silent) {
            silentFramesLeft = HELD_SILENT_FRAMES;
            return std::exchange(open, true) ? Step::Continue : Step::Begin;
        }
        if (!open)
            return Step::Skip;
        if (silentFramesLeft > 0) {
            silentFramesLeft--;
            return Step::Continue;
        }
        open = false;
        return Step::End;
    }

    // Frames have stopped coming, or cannot be sent. True when that ends a run.
    bool stop() { return std::exchange(open, false); }

private:
    bool open = false;
    int silentFramesLeft = 0;
};

// StreamAudio (sending runs of sound as Opus to Discord::Voice::VoiceClient) couples directly
// to the voice connection and OpusEncoder. It is listed for the voice integration once
// VoiceClient is moved off Qt.
inline constexpr int STREAM_AUDIO_MIN_BITRATE = 128000;
inline constexpr int STREAM_AUDIO_SOUND_GONE_MS = 100;

} // namespace Styx::Core
