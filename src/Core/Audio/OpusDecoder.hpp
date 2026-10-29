#pragma once

#include <string>
#include <vector>

#include <opus.h>

namespace Styx {
namespace Core {
namespace Audio {

class OpusDecoder
{
public:
    OpusDecoder();
    ~OpusDecoder();

    OpusDecoder(const OpusDecoder &) = delete;
    OpusDecoder &operator=(const OpusDecoder &) = delete;

    bool init(int sampleRate, int channels);

    // decode into frames of 20ms
    std::vector<std::string> decode(const std::string &opusData);

    std::string decodePlc();

private:
    std::vector<std::string> splitFrames(const std::string &pcm, int totalSamples);

    ::OpusDecoder *decoder = nullptr;
    int frameSamples = 0;
    int frameChannels = 0;
    int frameBytes = 0;
};

} // namespace Audio
} // namespace Core
} // namespace Styx
