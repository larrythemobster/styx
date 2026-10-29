#include "OpusDecoder.hpp"
#include "IAudioBackend.hpp"

#include "Core/Logging.hpp"

namespace Styx {
namespace Core {
namespace Audio {

OpusDecoder::OpusDecoder() = default;

OpusDecoder::~OpusDecoder()
{
    if (decoder)
        opus_decoder_destroy(decoder);
}

bool OpusDecoder::init(int sampleRate, int channels)
{
    if (decoder) {
        opus_decoder_destroy(decoder);
        decoder = nullptr;
    }

    frameSamples = sampleRate * AUDIO_FRAME_DURATION_MS / 1000;
    frameChannels = channels;
    frameBytes = frameSamples * frameChannels * static_cast<int>(sizeof(opus_int16));

    int error;
    decoder = opus_decoder_create(sampleRate, channels, &error);
    if (error != OPUS_OK || !decoder) {
        Core::log(Core::LogLevel::Error, LogVoice) << "Failed to create Opus decoder:" << opus_strerror(error);
        return false;
    }
    return true;
}

std::vector<std::string> OpusDecoder::decode(const std::string &opusData)
{
    if (!decoder)
        return {};

    // 120ms * 48khz
    static constexpr int MAX_OPUS_SAMPLES = 5760;
    int pcmSize = MAX_OPUS_SAMPLES * frameChannels * static_cast<int>(sizeof(opus_int16));
    std::string pcm(pcmSize, '\0');

    int samples = opus_decode(decoder,
                              reinterpret_cast<const unsigned char *>(opusData.data()),
                              opusData.size(),
                              reinterpret_cast<opus_int16 *>(pcm.data()),
                              MAX_OPUS_SAMPLES,
                              0);

    if (samples < 0) {
        Core::log(Core::LogLevel::Debug, LogVoice) << "Opus decode failed:" << opus_strerror(samples);
        return {};
    }

    return splitFrames(pcm, samples);
}

std::string OpusDecoder::decodePlc()
{
    if (!decoder)
        return {};

    std::string pcm(frameBytes, '\0');

    int samples = opus_decode(decoder,
                              nullptr, 0,
                              reinterpret_cast<opus_int16 *>(pcm.data()),
                              frameSamples,
                              0);

    if (samples < 0) {
        Core::log(Core::LogLevel::Debug, LogVoice) << "Opus PLC failed:" << opus_strerror(samples);
        return {};
    }

    pcm.resize(samples * frameChannels * static_cast<int>(sizeof(opus_int16)));
    return pcm;
}

std::vector<std::string> OpusDecoder::splitFrames(const std::string &pcm, int totalSamples)
{
    std::vector<std::string> frames;

    int offset = 0;
    int remaining = totalSamples;

    while (remaining >= frameSamples) {
        frames.push_back(pcm.substr(offset, frameBytes));
        offset += frameBytes;
        remaining -= frameSamples;
    }

    if (remaining > 0) {
        Core::log(Core::LogLevel::Debug, LogVoice) << "Opus decoded partial tail:" << remaining << "samples";
    }

    return frames;
}

} // namespace Audio
} // namespace Core
} // namespace Styx
