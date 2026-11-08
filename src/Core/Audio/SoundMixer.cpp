#include "SoundMixer.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace Styx::Core::Sounds {

void mixVoices(std::vector<Voice> &voices, float *output, std::size_t frameCount)
{
    std::fill(output, output + frameCount, 0.0f);
    for (auto &voice : voices) {
        const auto &samples = *voice.samples;
        // An empty looping sound would never advance.
        if (samples.empty())
            continue;
        std::size_t written = 0;
        while (written < frameCount && voice.position < samples.size()) {
            const std::size_t count = std::min(frameCount - written, samples.size() - voice.position);
            for (std::size_t i = 0; i < count; ++i)
                output[written + i] += samples[voice.position + i];
            written += count;
            voice.position += count;
            if (voice.looping && voice.position == samples.size())
                voice.position = 0;
        }
    }
    voices.erase(std::remove_if(voices.begin(), voices.end(), [](const Voice &voice) {
        return voice.samples->empty() || (!voice.looping && voice.position >= voice.samples->size());
    }), voices.end());
    for (std::size_t i = 0; i < frameCount; ++i)
        output[i] = std::clamp(output[i], -1.0f, 1.0f);
}

} // namespace Styx::Core::Sounds
