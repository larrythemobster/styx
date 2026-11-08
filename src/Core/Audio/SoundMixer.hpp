#pragma once

#include "Sounds.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace Styx::Core::Sounds {

struct Voice
{
    Sound sound = Sound::Message;
    std::shared_ptr<const std::vector<float>> samples;
    std::size_t position = 0;
    bool looping = false;
};

// Mixes every voice into output (mono, frameCount samples, overwritten), advances them, drops
// one-shots that finished, starts looping ones over, and clamps the result to [-1, 1].
void mixVoices(std::vector<Voice> &voices, float *output, std::size_t frameCount);

} // namespace Styx::Core::Sounds
