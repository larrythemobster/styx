#include "StreamSender.hpp"

#include "D3DDevice.hpp"
#include "FrameConverter.hpp"
#include "LoopbackCapture.hpp"
#include "VideoEncoder.hpp"

#include "Core/Audio/OpusEncoder.hpp"
#include "Core/Logging.hpp"

#include <cstring>

namespace Styx {
namespace Core {
namespace Stream {

static constexpr int OPUS_FRAME_SAMPLES = 960; // 20 ms at 48 kHz
static constexpr int KEYFRAME_MIN_INTERVAL_MS = 500;

StreamSender::StreamSender(Platform::WorkerLoop &loop) : loop(loop) {}

StreamSender::~StreamSender()
{
    stop();
}

void StreamSender::start(const CaptureSource &source, const StreamQuality &q, bool includeAudio)
{
    stop();
    quality = q.resolved({static_cast<int>(source.geometry.width), static_cast<int>(source.geometry.height)});
    targetBitrate = quality.bitrate;

    device = D3DDevice::create();
    if (!device) {
        failed.publish("Could not create a Direct3D 11 device.");
        return;
    }

    capture = std::make_unique<ScreenCapture>(device);
    std::string error;
    if (!capture->start(source, quality.captureCursor, quality.framerate, &error)) {
        failed.publish("Could not capture \"" + source.title + "\": " + error);
        capture.reset();
        return;
    }

    converter = std::make_unique<FrameConverter>(device);
    encoder = std::make_unique<VideoEncoder>(device);
    if (!reopenEncoder(targetBitrate))
        return;
    audioSamplesSent = 0;
    clock = std::chrono::steady_clock::now();

    bool audio = false;
    if (includeAudio && LoopbackCapture::isSupported()) {
        opus = std::make_unique<Audio::OpusEncoder>();
        if (opus->init(LoopbackCapture::SAMPLE_RATE, LoopbackCapture::CHANNELS, OPUS_APPLICATION_AUDIO)) {
            opus->setBitrate(128000);
            opus->setSignalType(OPUS_SIGNAL_MUSIC);
            opus->setComplexity(8);
            opus->setFec(false);

            loopback = std::make_unique<LoopbackCapture>();
            // a window shares its own app; a screen shares everything except this client,
            // so viewers never hear the call echoed back
            const bool window = source.kind == CaptureSource::Kind::Window && source.processId != 0;
            const auto mode = window ? LoopbackCapture::Mode::IncludeProcess : LoopbackCapture::Mode::ExcludeProcess;
            const uint32_t pid = window ? source.processId : GetCurrentProcessId();
            std::string audioError;
            audio = loopback->start(mode, pid, [this](const int16_t *samples, int frames) { onAudioSamples(samples, frames); }, &audioError);
            if (!audio) {
                log(LogLevel::Warning, LogStream) << "Streaming without audio:" << audioError;
                loopback.reset();
                opus.reset();
            }
        } else {
            opus.reset();
        }
    }

    running = true;
    frameIndex = 0;
    lastCaptureSequence = 0;
    lastEncodedMs = -10000;
    lastKeyframeMs = -10000;
    keyframePending = true;
    scheduleEncode();

    started.publish(encoder->settings().size.width, encoder->settings().size.height, quality.framerate,
                 quality.maxBitrate, audio, encoder->name());
}

void StreamSender::stop()
{
    running = false;
    if (timer) { loop.cancelTimer(timer); timer = 0; }
    if (loopback)
        loopback->stop();
    loopback.reset();
    {
        std::lock_guard lock(audioMutex);
        audioPending.clear();
    }
    opus.reset();
    if (capture)
        capture->stop();
    capture.reset();
    encoder.reset();
    converter.reset();
    cpuTarget.Reset();
    cpuStaging.Reset();
    device.reset();
}

bool StreamSender::reopenEncoder(int bitrate)
{
    EncoderSettings settings;
    settings.size = quality.size;
    settings.framerate = quality.framerate;
    settings.bitrate = bitrate;
    settings.maxBitrate = std::max(bitrate, quality.maxBitrate * bitrate / std::max(1, quality.bitrate));

    std::string error;
    if (!encoder->open(settings, &error)) {
        failed.publish("No usable H264 encoder: " + error);
        return false;
    }
    targetBitrate = bitrate;
    cpuTarget.Reset();
    cpuStaging.Reset();
    keyframePending = true;
    return true;
}

void StreamSender::requestKeyframe()
{
    keyframePending = true;
}

void StreamSender::onBandwidthEstimate(unsigned int bitsPerSecond)
{
    if (!running || !encoder)
        return;

    // Re-opening the encoder costs a keyframe, so only react to a sustained change: the
    // estimate stays below 80% of what we send (or above 130% when we are throttled) for
    // several REMB reports in a row.
    const int floor = quality.bitrate / 4;
    const int estimate = static_cast<int>(std::min<unsigned int>(bitsPerSecond, INT32_MAX));
    if (estimate < targetBitrate * 8 / 10) {
        estimateAboveCount = 0;
        if (++estimateBelowCount >= 3) {
            estimateBelowCount = 0;
            const int next = std::max(floor, estimate * 9 / 10);
            if (next < targetBitrate) {
                log(LogLevel::Info, LogStream) << "Lowering stream bitrate to" << next / 1000 << "kbps (REMB" << estimate / 1000 << ")";
                reopenEncoder(next);
            }
        }
    } else if (targetBitrate < quality.bitrate && estimate > targetBitrate * 13 / 10) {
        estimateBelowCount = 0;
        if (++estimateAboveCount >= 5) {
            estimateAboveCount = 0;
            const int next = std::min(quality.bitrate, targetBitrate * 5 / 4);
            log(LogLevel::Info, LogStream) << "Raising stream bitrate to" << next / 1000 << "kbps";
            reopenEncoder(next);
        }
    } else {
        estimateBelowCount = 0;
        estimateAboveCount = 0;
    }
}

bool StreamSender::readBackNv12(ID3D11Texture2D *source, AVFrame *frame)
{
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (!cpuStaging) {
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        if (FAILED(device->device()->CreateTexture2D(&desc, nullptr, &cpuStaging)))
            return false;
    }
    auto *context = device->context();
    context->CopyResource(cpuStaging.Get(), source);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(cpuStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return false;

    const int width = frame->width;
    const int height = frame->height;
    const auto *src = static_cast<const uint8_t *>(mapped.pData);
    for (int y = 0; y < height; ++y)
        std::memcpy(frame->data[0] + y * frame->linesize[0], src + y * mapped.RowPitch, width);
    // NV12: interleaved UV plane directly after the luma rows of the full texture height
    const uint8_t *uv = src + mapped.RowPitch * desc.Height;
    for (int y = 0; y < height / 2; ++y)
        std::memcpy(frame->data[1] + y * frame->linesize[1], uv + y * mapped.RowPitch, width);

    context->Unmap(cpuStaging.Get(), 0);
    return true;
}

void StreamSender::scheduleEncode()
{
    if (!running)
        return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - clock).count();
    const auto due = frameIndex * 1'000'000 / quality.framerate;
    const auto delay = std::chrono::milliseconds(std::max<int64_t>(1, (due - elapsed + 999) / 1000));
    timer = loop.setTimer(delay, false, [this] {
        timer = 0;
        encodeTick();
        scheduleEncode();
    });
}

void StreamSender::encodeTick()
{
    if (!running || !encoder || !capture)
        return;
    const int64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - clock).count();
    const int64_t sample = std::max(frameIndex, elapsedUs * quality.framerate / 1'000'000);
    const int64_t ptsUs = sample * 1'000'000 / quality.framerate;
    frameIndex = sample + 1;
    const int64_t now = elapsedUs / 1000;
    const bool forceKeyframe = keyframePending && now - lastKeyframeMs >= KEYFRAME_MIN_INTERVAL_MS;

    if (capture->isClosed()) {
        running = false;
        if (timer) { loop.cancelTimer(timer); timer = 0; }
        sourceClosed.publish();
        return;
    }

    const Platform::Size target = encoder->settings().size;
    AVFrame *frame = nullptr;
    {
        std::lock_guard deviceLock(device->mutex());
        const ScreenCapture::Frame captured = capture->latestFrame();
        if (!captured.texture)
            return; // nothing captured yet
        if (captured.sequence == lastCaptureSequence && !forceKeyframe && now - lastEncodedMs < 500)
            return;
        lastCaptureSequence = captured.sequence;

        if (encoder->takesGpuSurfaces()) {
            ID3D11Texture2D *surface = nullptr;
            UINT index = 0;
            frame = encoder->acquireGpuFrame(&surface, &index);
            if (!frame)
                return;
            if (!converter->convert(captured.texture, captured.size, surface, index, target)) {
                av_frame_free(&frame);
                return;
            }
        } else {
            if (!cpuTarget) {
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = static_cast<UINT>(target.width);
                desc.Height = static_cast<UINT>(target.height);
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_NV12;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_RENDER_TARGET;
                if (FAILED(device->device()->CreateTexture2D(&desc, nullptr, &cpuTarget)))
                    return;
            }
            if (!converter->convert(captured.texture, captured.size, cpuTarget.Get(), 0, target))
                return;
            frame = encoder->acquireCpuFrame();
            if (!frame || !readBackNv12(cpuTarget.Get(), frame)) {
                av_frame_free(&frame);
                return;
            }
        }
    }

    if (forceKeyframe) {
        keyframePending = false;
        lastKeyframeMs = now;
    }

    lastEncodedMs = now;

    std::vector<EncodedPacket> packets;
    encoder->encode(frame, ptsUs, forceKeyframe, packets);
    for (const auto &packet : packets) {
        if (packet.keyframe)
            lastKeyframeMs = now;
        encodedVideo.publish(packet.data, packet.ptsUs, packet.keyframe);
    }
}

void StreamSender::onAudioSamples(const int16_t *samples, int frames)
{
    // WASAPI thread
    std::vector<std::string> encoded;
    int64_t firstPts = 0;
    {
        std::lock_guard lock(audioMutex);
        if (!opus)
            return;
        audioPending.insert(audioPending.end(), samples, samples + static_cast<size_t>(frames) * LoopbackCapture::CHANNELS);
        const size_t frameValues = static_cast<size_t>(OPUS_FRAME_SAMPLES) * LoopbackCapture::CHANNELS;
        firstPts = audioSamplesSent * 1'000'000 / LoopbackCapture::SAMPLE_RATE;
        size_t offset = 0;
        while (audioPending.size() - offset >= frameValues) {
            std::string pcm(reinterpret_cast<const char *>(audioPending.data() + offset), frameValues * sizeof(int16_t));
            encoded.push_back(opus->encode(pcm));
            offset += frameValues;
        }
        audioPending.erase(audioPending.begin(), audioPending.begin() + static_cast<std::ptrdiff_t>(offset));
        audioSamplesSent += static_cast<int64_t>(encoded.size()) * OPUS_FRAME_SAMPLES;
    }
    int64_t pts = firstPts;
    for (const std::string &packet : encoded) {
        if (!packet.empty())
            encodedAudio.publish(packet, pts);
        pts += 20'000;
    }
}

} // namespace Stream
} // namespace Core
} // namespace Styx
