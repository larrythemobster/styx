#include "Sounds.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string_view>
#include <vector>

namespace Styx::Core::Sounds {
namespace {

constexpr double PI = 3.14159265358979323846;
// Short fades at both ends of every tone stop the speaker clicking.
constexpr double FADE_SECONDS = 0.005;

void appendSilence(std::vector<float> &out, double seconds)
{
    out.insert(out.end(), static_cast<std::size_t>(std::lround(seconds * SAMPLE_RATE)), 0.0f);
}

// A soft bell: the note plus a quieter octave, rising for FADE_SECONDS, then decaying, then
// fading to silence over the last FADE_SECONDS.
void appendTone(std::vector<float> &out, double frequency, double seconds, double amplitude)
{
    const auto count = static_cast<std::size_t>(std::lround(seconds * SAMPLE_RATE));
    const double fadeSamples = FADE_SECONDS * SAMPLE_RATE;
    for (std::size_t i = 0; i < count; ++i) {
        const double t = static_cast<double>(i) / SAMPLE_RATE;
        const double fromStart = static_cast<double>(i) / fadeSamples;
        const double fromEnd = static_cast<double>(count - 1 - i) / fadeSamples;
        const double fade = std::min({1.0, fromStart, fromEnd});
        const double decay = std::exp(-3.0 * t / seconds);
        const double wave = (std::sin(2 * PI * frequency * t) + 0.25 * std::sin(4 * PI * frequency * t)) / 1.25;
        out.push_back(static_cast<float>(amplitude * fade * decay * wave));
    }
}

} // namespace

std::vector<float> synthesize(Sound sound)
{
    std::vector<float> out;
    switch (sound) {
    case Sound::Message:
        appendTone(out, 1318.5, 0.09, 0.35);
        appendTone(out, 1760.0, 0.16, 0.35);
        break;
    case Sound::Mention:
        appendTone(out, 1046.5, 0.07, 0.45);
        appendTone(out, 1318.5, 0.07, 0.45);
        appendTone(out, 1568.0, 0.18, 0.45);
        break;
    case Sound::Ring:
        // Two rising pairs and a pause, so playing it on repeat gives a phone-like cadence.
        for (int pair = 0; pair < 2; ++pair) {
            appendTone(out, 880.0, 0.18, 0.4);
            appendTone(out, 1108.7, 0.18, 0.4);
            appendSilence(out, 0.08);
        }
        appendSilence(out, 0.6);
        break;
    case Sound::Join:
        appendTone(out, 784.0, 0.08, 0.35);
        appendTone(out, 1046.5, 0.14, 0.35);
        break;
    case Sound::Leave:
        appendTone(out, 1046.5, 0.08, 0.35);
        appendTone(out, 784.0, 0.14, 0.35);
        break;
    }
    return out;
}

std::string_view settingKey(Sound sound)
{
    switch (sound) {
    case Sound::Message: return "sounds/message";
    case Sound::Mention: return "sounds/mention";
    case Sound::Ring: return "sounds/ring";
    case Sound::Join: return "sounds/join";
    case Sound::Leave: return "sounds/leave";
    }
    return {};
}

} // namespace Styx::Core::Sounds
