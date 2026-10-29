// Ported from ouwou/acheron upstream/video-streams 71b4907
#include "Core/Video/BroadcastPipeline.hpp"

#include "Core/Media/VideoEncoder.hpp"
#include "Core/Video/ScreenCapture.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace Styx::Core::Video {

namespace {

using Discord::Voice::EncodedVideoFrame;
using Discord::Voice::VideoCodec;

constexpr int PREVIEW_INTERVAL_MS = 500;
constexpr FrameSize PREVIEW_BOUND{ 512, 288 };
constexpr int STATS_INTERVAL_MS = 1000;
// How long a window has to have been hidden before its share is paused. One that is being
// closed is hidden for a moment first, and that is the end of the share and not a pause.
constexpr int HIDDEN_BEFORE_PAUSED_MS = 200;
constexpr int MIN_BITRATE = 50'000;
constexpr int BITRATE_WHEN_NONE_GIVEN = 2'500'000;
constexpr int SWSCALE_OVERRUN_BYTES = 64;
constexpr int HD_HEIGHT = 720;

struct FrameDeleter
{
    void operator()(AVFrame *frame) const { av_frame_free(&frame); }
};
using FramePointer = std::unique_ptr<AVFrame, FrameDeleter>;

int usableBitrate(int wanted, int most)
{
    if (wanted <= 0)
        wanted = BITRATE_WHEN_NONE_GIVEN;
    if (most > 0)
        wanted = std::min(wanted, most);
    return std::max(wanted, MIN_BITRATE);
}

FrameSize scaledKeepAspectRatio(FrameSize size, FrameSize bound)
{
    if (size.width <= 0 || size.height <= 0 || bound.width <= 0 || bound.height <= 0)
        return {};
    const auto wAtBoundH = static_cast<int64_t>(bound.height) * size.width / size.height;
    if (wAtBoundH <= bound.width)
        return { static_cast<int>(wAtBoundH), bound.height };
    return { bound.width, static_cast<int>(static_cast<int64_t>(bound.width) * size.height / size.width) };
}

bool isYuv(int format)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(format));
    return desc && !(desc->flags & AV_PIX_FMT_FLAG_RGB);
}

bool usesBt709(const AVFrame *frame)
{
    if (frame->colorspace == AVCOL_SPC_BT709)
        return true;
    if (frame->colorspace == AVCOL_SPC_SMPTE170M || frame->colorspace == AVCOL_SPC_BT470BG)
        return false;
    return frame->height >= HD_HEIGHT;
}

bool isFullRange(const AVFrame *frame)
{
    if (frame->color_range == AVCOL_RANGE_JPEG)
        return true;
    if (frame->color_range == AVCOL_RANGE_MPEG)
        return false;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
    return desc && !(desc->flags & AV_PIX_FMT_FLAG_RGB) ? false : true;
}

// Small stills of what is being captured, each in an image of its own.
class PreviewMaker
{
public:
    ~PreviewMaker()
    {
        sws_freeContext(sws);
    }

    Platform::ImageBuffer make(const AVFrame *frame)
    {
        const AVFrame *picture = frame;
        if (frame->hw_frames_ctx) {
            av_frame_unref(downloaded.get());
            if (av_hwframe_transfer_data(downloaded.get(), frame, 0) < 0)
                return {};
            av_frame_copy_props(downloaded.get(), frame);
            picture = downloaded.get();
        }

        const FrameSize full{ picture->width, picture->height };
        const bool fits = full.width <= PREVIEW_BOUND.width && full.height <= PREVIEW_BOUND.height;
        FrameSize size = fits ? full : scaledKeepAspectRatio(full, PREVIEW_BOUND);
        size.width = std::max(1, size.width);
        size.height = std::max(1, size.height);

        Platform::ImageBuffer buffer;
        buffer.width = size.width;
        buffer.height = size.height;
        buffer.channels = 4;
        buffer.pixels.resize(static_cast<size_t>(size.width) * size.height * 4 + SWSCALE_OVERRUN_BYTES);

        sws = sws_getCachedContext(sws, picture->width, picture->height, static_cast<AVPixelFormat>(picture->format),
                                   size.width, size.height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) {
            av_frame_unref(downloaded.get());
            return {};
        }

        if (isYuv(picture->format)) {
            const int *coefficients = sws_getCoefficients(usesBt709(picture) ? SWS_CS_ITU709 : SWS_CS_ITU601);
            sws_setColorspaceDetails(sws, coefficients, isFullRange(picture) ? 1 : 0, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
        }

        uint8_t *planes[4] = { buffer.pixels.data(), nullptr, nullptr, nullptr };
        const int strides[4] = { size.width * 4, 0, 0, 0 };
        const int scaled = sws_scale(sws, picture->data, picture->linesize, 0, picture->height, planes, strides);
        av_frame_unref(downloaded.get());

        if (scaled <= 0)
            return {};
        buffer.pixels.resize(static_cast<size_t>(size.width) * size.height * 4);
        return buffer;
    }

private:
    SwsContext *sws = nullptr;
    FramePointer downloaded{ av_frame_alloc() };
};

} // namespace

class BroadcastPipeline::Worker
{
public:
    explicit Worker(BroadcastPipeline *pipeline) : pipeline(pipeline), thread([this] { run(); }) {}

    ~Worker()
    {
        {
            std::lock_guard lock(mutex);
            quitting = true;
            generation++;
        }
        wake.notify_all();
        if (thread.joinable())
            thread.join();
    }

    void start(const BroadcastSettings &settings)
    {
        const CaptureMethod method = captureMethod();
        {
            std::lock_guard lock(mutex);
            wanted = settings;
            wantedMethod = method;
            bitrate = settings.bitrate;
            generation++;
        }
        wake.notify_all();
    }

    void stop()
    {
        std::lock_guard lock(mutex);
        wanted.reset();
        generation++;
    }

    std::atomic<bool> keyframeWanted{ false };
    std::atomic<bool> suspended{ false };
    std::atomic<int> bitrate{ 0 };

private:
    void run();
    void capture(const BroadcastSettings &settings, CaptureMethod method, uint64_t session);
    [[nodiscard]] int64_t nextTimestamp();

    BroadcastPipeline *pipeline;
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<BroadcastSettings> wanted;
    CaptureMethod wantedMethod = CaptureMethod::Auto;
    bool quitting = false;
    // Moves on with every start and stop; a capture ends when it is no longer its own.
    std::atomic<uint64_t> generation{ 0 };

    // Timestamps run on from one capture to the next, so that a restarted one never goes back.
    const std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
    int64_t lastTimestamp = -1;

    std::thread thread;
};

void BroadcastPipeline::Worker::run()
{
    for (;;) {
        BroadcastSettings settings;
        CaptureMethod method = CaptureMethod::Auto;
        uint64_t session = 0;
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [this] { return quitting || wanted; });
            if (quitting)
                return;
            settings = *wanted;
            method = wantedMethod;
            wanted.reset();
            session = generation;
        }
        capture(settings, method, session);
    }
}

int64_t BroadcastPipeline::Worker::nextTimestamp()
{
    const int64_t microseconds = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - origin).count();
    lastTimestamp = std::max(lastTimestamp + 1, microseconds * 9 / 100);
    return lastTimestamp;
}

void BroadcastPipeline::Worker::capture(const BroadcastSettings &settings, CaptureMethod method, uint64_t session)
{
    Media::VideoEncoder::Settings encoding;
    encoding.codec = settings.codec;
    encoding.framerate = std::max(1, settings.framerate);
    encoding.bitrate = usableBitrate(bitrate, settings.maxBitrate);
    encoding.keyframeIntervalMs = settings.keyframeIntervalMs;
    encoding.content = Media::VideoEncoder::Content::Screen;
    Media::VideoEncoder encoder(encoding);
    if (!encoder.hasEncoder()) {
        pipeline->failed.publish("No " + Discord::Voice::videoCodecName(settings.codec) + " encoder works on this machine.");
        return;
    }

    ScreenCapture::Request request;
    request.sourceId = settings.sourceId;
    request.method = method;
    request.maxHeight = settings.maxHeight;
    request.framerate = encoding.framerate;
    request.softwareFormat = encoder.softwareFormat();
    request.keepOnCard = encoder.takesCardFrames();
    request.rgbOnCard = encoder.takesRgbCardFrames();
    request.bt601Only = settings.codec == VideoCodec::VP8;
    ScreenCapture screen;
    screen.configure(request);

    const FramePointer frame(av_frame_alloc());
    // The last picture of a source that has one only when it shows something new.
    const FramePointer kept(av_frame_alloc());
    PreviewMaker previews;
    Platform::ImageBuffer preview;
    bool previewIsOfThisPicture = false;
    std::vector<EncodedVideoFrame> encoded;

    using Clock = std::chrono::steady_clock;
    Clock::time_point sincePicture{};
    bool hasSincePicture = false;
    Clock::time_point sincePreview{};
    bool hasSincePreview = false;
    Clock::time_point sinceStats = Clock::now();
    Clock::time_point hiddenFor{};
    bool hasHiddenFor = false;

    FrameSize announcedSize{};
    bool stalled = false;
    bool wasSuspended = false;
    uint64_t framesEncoded = 0;
    uint64_t framesDropped = 0;
    int capturedLately = 0;
    int encodedLately = 0;
    uint64_t bytesLately = 0;

    while (generation == session) {
        const auto now = Clock::now();
        // With nobody watching, a picture is only wanted for a preview.
        const bool suspendedNow = suspended;
        const auto elapsedPreviewMs = hasSincePreview ? std::chrono::duration_cast<std::chrono::milliseconds>(now - sincePreview).count() : 0;
        const bool pictureWanted = !suspendedNow || announcedSize.empty() || !hasSincePreview || elapsedPreviewMs >= PREVIEW_INTERVAL_MS;
        screen.setPicturesWanted(pictureWanted);
        const ScreenCapture::Status status = screen.read(frame.get());
        if (status == ScreenCapture::Status::Failed) {
            pipeline->failed.publish(screen.failure());
            return;
        }
        if (status == ScreenCapture::Status::Closed) {
            pipeline->sourceClosed.publish();
            return;
        }

        // A hidden window is known to show nothing, also before its first picture. A source
        // that cannot be read has until its first picture to settle before that is a stall.
        const bool hidden = status == ScreenCapture::Status::Hidden;
        if (hidden && !hasHiddenFor) {
            hiddenFor = now;
            hasHiddenFor = true;
        }
        bool stalledNow = stalled;
        if (hidden) {
            const auto elapsedHidden = std::chrono::duration_cast<std::chrono::milliseconds>(now - hiddenFor).count();
            stalledNow = stalled || elapsedHidden >= HIDDEN_BEFORE_PAUSED_MS;
        } else if (status == ScreenCapture::Status::Stalled) {
            stalledNow = stalled || !announcedSize.empty();
        } else if (status == ScreenCapture::Status::Frame || hasHiddenFor) {
            stalledNow = false;
        }
        if (!hidden)
            hasHiddenFor = false;
        if (std::exchange(stalled, stalledNow) != stalledNow)
            pipeline->captureStalled.publish(stalledNow);

        if (!pictureWanted)
            continue;

        AVFrame *picture = nullptr;
        if (status == ScreenCapture::Status::Frame)
            previewIsOfThisPicture = false;
        if (status == ScreenCapture::Status::Frame && screen.deliversOnlyChanges()) {
            av_frame_unref(kept.get());
            av_frame_move_ref(kept.get(), frame.get());
            picture = kept.get();
        } else if (status == ScreenCapture::Status::Frame) {
            picture = frame.get();
        } else if (status == ScreenCapture::Status::NoFrame && kept->buf[0] && screen.deliversOnlyChanges()) {
            const bool keyFrameWaitedFor = keyframeWanted || (wasSuspended && !suspendedNow);
            const auto elapsedPicMs = hasSincePicture ? std::chrono::duration_cast<std::chrono::milliseconds>(now - sincePicture).count() : 0;
            if (stillPictureIsDue(std::chrono::milliseconds(elapsedPicMs), keyFrameWaitedFor, encoding.framerate))
                picture = kept.get();
        }
        if (!picture)
            continue;
        sincePicture = Clock::now();
        hasSincePicture = true;

        const int64_t timestamp = nextTimestamp();
        const FrameSize size{ picture->width, picture->height };
        capturedLately++;

        // Nothing is encoded of a size the encoder would add too much black to. The size is
        // still reported, and it is for whoever started this to have another codec chosen.
        const FrameSize codedSize = encoder.codedSize(size);
        const bool heldBack = !Media::paddingIsAcceptable(size, codedSize);

        const bool keyframeAsked = keyframeWanted.exchange(false);
        if (!suspendedNow && !heldBack) {
            encoder.setBitrate(usableBitrate(bitrate, settings.maxBitrate));
            if (keyframeAsked || wasSuspended)
                encoder.requestKeyframe();

            encoded.clear();
            const Media::VideoEncoder::Outcome outcome = encoder.encode(picture, timestamp, encoded);
            if (outcome == Media::VideoEncoder::Outcome::CardFramesRefused) {
                request.keepOnCard = false;
                screen.configure(request);
                av_frame_unref(frame.get());
                av_frame_unref(kept.get());
                continue;
            }
            if (outcome == Media::VideoEncoder::Outcome::Failed) {
                pipeline->failed.publish("No " + Discord::Voice::videoCodecName(settings.codec) + " encoder works on this machine.");
                return;
            }
        }
        wasSuspended = suspendedNow;

        // Again whenever a window has changed its size, which the picture follows.
        if (std::exchange(announcedSize, size) != size)
            pipeline->started.publish(size, encoding.framerate);

        if (!suspendedNow && !heldBack) {
            framesDropped += encoded.empty() ? 1 : 0;
            for (EncodedVideoFrame &each : encoded) {
                framesEncoded++;
                encodedLately++;
                bytesLately += each.data.size();
                pipeline->frameEncoded.publish(std::make_shared<const EncodedVideoFrame>(std::move(each)));
            }
        }

        const auto afterEncodeNow = Clock::now();
        const auto previewElapsed = hasSincePreview ? std::chrono::duration_cast<std::chrono::milliseconds>(afterEncodeNow - sincePreview).count() : 0;
        if (!hasSincePreview || previewElapsed >= PREVIEW_INTERVAL_MS) {
            sincePreview = afterEncodeNow;
            hasSincePreview = true;
            // A picture sent again because nothing new has come is the one the last preview is of.
            if (!std::exchange(previewIsOfThisPicture, true))
                preview = previews.make(picture);
            if (!preview.empty())
                pipeline->previewReady.publish(preview);
        }

        const auto statsElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(afterEncodeNow - sinceStats).count();
        if (statsElapsed >= STATS_INTERVAL_MS) {
            const double seconds = static_cast<double>(statsElapsed) / 1000.0;
            sinceStats = afterEncodeNow;
            const Media::VideoEncoder::Stats &encoderStats = encoder.stats();
            BroadcastStats stats;
            std::string encoderName = encoderStats.encoderName.empty() ? "no encoder yet" : encoderStats.encoderName;
            if (heldBack)
                encoderName = "none while " + Discord::Voice::videoCodecName(settings.codec) + " would add black to this size";
            stats.encoderName = encoderName + ", from " + screen.captureName();
            stats.hardware = encoderStats.hardware;
            stats.encodedSize = size;
            stats.capturedFps = capturedLately / seconds;
            stats.encodedFps = encodedLately / seconds;
            stats.averageEncodeMs = encoderStats.averageEncodeMs;
            stats.bitrateBps = static_cast<uint64_t>(static_cast<double>(bytesLately) * 8.0 / seconds);
            stats.framesEncoded = framesEncoded;
            stats.framesDropped = framesDropped;
            pipeline->statsUpdated.publish(stats);
            capturedLately = 0;
            encodedLately = 0;
            bytesLately = 0;
        }

        av_frame_unref(frame.get());
    }
}

BroadcastPipeline::BroadcastPipeline()
{
    worker = std::make_unique<Worker>(this);
}

BroadcastPipeline::~BroadcastPipeline() = default;

std::vector<Discord::Voice::VideoCodec> BroadcastPipeline::encodableCodecs()
{
    return Media::encodableVideoCodecs();
}

void BroadcastPipeline::start(const BroadcastSettings &settings)
{
    worker->start(settings);
}

void BroadcastPipeline::stop()
{
    worker->stop();
}

void BroadcastPipeline::requestKeyframe()
{
    worker->keyframeWanted = true;
}

void BroadcastPipeline::setBitrate(int bitrate)
{
    worker->bitrate = bitrate;
}

void BroadcastPipeline::setEncodingSuspended(bool suspended)
{
    worker->suspended = suspended;
}

} // namespace Styx::Core::Video
