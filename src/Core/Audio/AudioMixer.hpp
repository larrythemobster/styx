#pragma once

#include <string>
#include <vector>

#include <cstdint>
#include <utility>

namespace Styx {
namespace Core {
namespace Audio {

class AudioMixer
{
public:
    // all frames must be same size
    static std::string mix(const std::vector<std::pair<std::string /* pcm */, float /* gain */>> &inputs);
};

} // namespace Audio
} // namespace Core
} // namespace Styx
