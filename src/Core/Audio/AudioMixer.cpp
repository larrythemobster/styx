#include "AudioMixer.hpp"

#include <algorithm>
#include <cmath>

namespace Styx {
namespace Core {
namespace Audio {

std::string AudioMixer::mix(const std::vector<std::pair<std::string, float>> &inputs)
{
    if (inputs.empty())
        return {};

    int frameSize = static_cast<int>(inputs.front().first.size());
    if (frameSize == 0)
        return {};

    int sampleCount = frameSize / static_cast<int>(sizeof(int16_t));

    std::vector<int32_t> mixed(sampleCount, 0);

    for (const auto &[pcm, gain] : inputs) {
        if (pcm.size() != frameSize)
            continue;

        const auto *samples = reinterpret_cast<const int16_t *>(pcm.data());
        for (int i = 0; i < sampleCount; i++)
            mixed[i] += static_cast<int32_t>(std::lround(samples[i] * gain));
    }

    std::string output(frameSize, '\0');
    auto *out = reinterpret_cast<int16_t *>(output.data());
    for (int i = 0; i < sampleCount; i++)
        out[i] = static_cast<int16_t>(std::clamp(mixed[i],
                                                 static_cast<int32_t>(INT16_MIN),
                                                 static_cast<int32_t>(INT16_MAX)));

    return output;
}

} // namespace Audio
} // namespace Core
} // namespace Styx
