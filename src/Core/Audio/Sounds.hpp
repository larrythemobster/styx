#pragma once

#include <string_view>
#include <vector>

namespace Styx::Core::Sounds {

enum class Sound { Message, Mention, Ring, Join, Leave };

inline constexpr int SAMPLE_RATE = 48000;

// Mono samples at SAMPLE_RATE. The sounds are built from tones, so Styx ships no audio files.
[[nodiscard]] std::vector<float> synthesize(Sound sound);

// The bool setting that turns a sound on or off. Every sound is on by default.
[[nodiscard]] std::string_view settingKey(Sound sound);

// Plays a sound on the default output device if its setting is on. Call on the main thread.
void play(Sound sound);

// Repeats a sound until stopLoop, if its setting is on. Starting one that's already looping
// does nothing. Call on the main thread.
void startLoop(Sound sound);
void stopLoop(Sound sound);

} // namespace Styx::Core::Sounds
