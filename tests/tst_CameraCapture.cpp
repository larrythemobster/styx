// Ported from ouwou/Acheron upstream/video-streams (71b4907) tests/tst_CameraCapture.cpp to Catch2.
// Synthetic frame sources only - never opens the real camera.

#include "Core/Media/RgbScaler.hpp"
#include "Core/Media/VideoDecoder.hpp"
#include "Core/Media/VideoEncoder.hpp"
#include "Core/Video/CameraCapture.hpp"
#include "Core/Video/CameraPipeline.hpp"
#include "HardwareTests.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifdef __linux__
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

using namespace Styx;
using namespace Styx::Core::Video;
using Discord::Voice::EncodedVideoFrame;
using Discord::Voice::VideoCodec;
using Discord::Voice::videoCodecName;

namespace {

using Clock = std::chrono::steady_clock;

constexpr int BITRATE = 1'500'000;
constexpr int TICKS_PER_SECOND = 90000;

// The colour a synthetic camera shows when it shows one colour.
constexpr int COLOUR_RED = 40;
constexpr int COLOUR_GREEN = 200;
constexpr int COLOUR_BLUE = 60;
// And the two greys, as full-range luma, when it shows a dark half and a light half.
constexpr int DARK_GREY = 60;
constexpr int LIGHT_GREY = 200;
// The luma of the square a synthetic camera shows in its middle, a third of its height on a
// side, and of what is around it.
constexpr int SQUARE_LUMA = 220;
constexpr int AROUND_SQUARE_LUMA = 40;

using FramePointer = std::shared_ptr<AVFrame>;

FramePointer makeFrame() {
    return FramePointer(av_frame_alloc(), [](AVFrame *frame) { av_frame_free(&frame); });
}

struct Yuv {
    double y = 0;
    double u = 0;
    double v = 0;
};

Yuv inYuv(int red, int green, int blue, bool bt709, bool fullRange) {
    const double kr = bt709 ? 0.2126 : 0.299;
    const double kb = bt709 ? 0.0722 : 0.114;
    const double luma = kr * red + (1 - kr - kb) * green + kb * blue;
    const double u = (blue - luma) / (2 * (1 - kb));
    const double v = (red - luma) / (2 * (1 - kr));
    if (fullRange)
        return {luma, 128 + u, 128 + v};
    return {16 + 219 * luma / 255, 128 + 224 * u / 255, 128 + 224 * v / 255};
}

double distance(const Yuv &a, const Yuv &b) {
    return std::sqrt((a.y - b.y) * (a.y - b.y) + (a.u - b.u) * (a.u - b.u) + (a.v - b.v) * (a.v - b.v));
}

// What the camera that stands in for the real one is like, and what was done with it.
struct SyntheticCamera {
    enum class Shows {
        Movement,
        Square,
        TwoGreys,
        OneColour,
    };

    FrameSize size{640, 360};
    AVPixelFormat format = AV_PIX_FMT_YUV420P;
    bool fullRange = false;
    AVColorSpace colorspace = AVCOL_SPC_SMPTE170M;
    int framerate = 30;
    Shows shows = Shows::Movement;
    bool opens = true;
    int opensAfterMs = 0;
    std::optional<int> lostAfter;

    std::atomic<int> timesOpened{0};
    std::atomic<int> openNow{0};
    std::atomic<int> picturesGiven{0};
    std::mutex mutex;
    std::string openedWithId;

    void atComingPictures(std::initializer_list<std::function<void()>> each) {
        std::lock_guard lock(mutex);
        dueAtComingPictures.insert(dueAtComingPictures.end(), each);
    }

    void doWhatIsDueAtThisPicture() {
        std::function<void()> due;
        {
            std::lock_guard lock(mutex);
            if (dueAtComingPictures.empty())
                return;
            due = std::move(dueAtComingPictures.front());
            dueAtComingPictures.pop_front();
        }
        due();
    }

private:
    std::deque<std::function<void()>> dueAtComingPictures;
};

void paint(AVFrame *picture, const SyntheticCamera &camera, int index) {
    const AVPixFmtDescriptor *layout = av_pix_fmt_desc_get(camera.format);
    const int width = picture->width;
    const int height = picture->height;
    const int chromaWidth = AV_CEIL_RSHIFT(width, layout->log2_chroma_w);
    const int chromaHeight = AV_CEIL_RSHIFT(height, layout->log2_chroma_h);
    const Yuv colour = inYuv(COLOUR_RED, COLOUR_GREEN, COLOUR_BLUE, camera.colorspace == AVCOL_SPC_BT709, camera.fullRange);
    const int squareSide = height / 3;

    uint32_t noise = 12345u + static_cast<uint32_t>(index) * 2654435761u;
    for (int y = 0; y < height; y++) {
        uint8_t *row = picture->data[0] + y * picture->linesize[0];
        for (int x = 0; x < width; x++) {
            uint8_t value = 0;
            switch (camera.shows) {
            case SyntheticCamera::Shows::Movement:
                value = static_cast<uint8_t>((x + y) / 4 + index * 3);
                if (x >= width / 4 && x < width * 3 / 4 && y >= height / 3 && y < height * 2 / 3)
                    value = static_cast<uint8_t>(((x + 3 * index) * 7) ^ ((y + 2 * index) * 5));
                if (x < width / 4 && y < height / 6) {
                    noise = noise * 1664525u + 1013904223u;
                    value = static_cast<uint8_t>(96 + (noise >> 26));
                }
                break;
            case SyntheticCamera::Shows::Square:
                value = std::abs(2 * x - width) < squareSide && std::abs(2 * y - height) < squareSide ? SQUARE_LUMA : AROUND_SQUARE_LUMA;
                break;
            case SyntheticCamera::Shows::TwoGreys:
                value = x < width / 2 ? DARK_GREY : LIGHT_GREY;
                break;
            case SyntheticCamera::Shows::OneColour:
                value = static_cast<uint8_t>(std::lround(colour.y));
                break;
            }
            row[x] = value;
        }
    }

    for (int y = 0; y < chromaHeight; y++) {
        for (int x = 0; x < chromaWidth; x++) {
            uint8_t u = 128;
            uint8_t v = 128;
            if (camera.shows == SyntheticCamera::Shows::Movement) {
                u = static_cast<uint8_t>(128 + y / 4 + index * 2);
                v = static_cast<uint8_t>(64 + x / 4 + index * 5);
            } else if (camera.shows == SyntheticCamera::Shows::OneColour) {
                u = static_cast<uint8_t>(std::lround(colour.u));
                v = static_cast<uint8_t>(std::lround(colour.v));
            }
            picture->data[1][y * picture->linesize[1] + x] = u;
            picture->data[2][y * picture->linesize[2] + x] = v;
        }
    }
}

class SyntheticSource : public CameraFrameSource {
public:
    explicit SyntheticSource(std::shared_ptr<SyntheticCamera> camera) : camera(std::move(camera)) {}

    ~SyntheticSource() override {
        if (isOpen)
            camera->openNow--;
    }

    bool open(const std::string &deviceId) override {
        {
            std::lock_guard lock(camera->mutex);
            camera->openedWithId = deviceId;
        }
        camera->timesOpened++;
        std::this_thread::sleep_for(std::chrono::milliseconds(camera->opensAfterMs));
        if (!camera->opens)
            return false;

        const int different = camera->shows == SyntheticCamera::Shows::Movement ? 8 : 1;
        for (int index = 0; index < different; index++) {
            FramePointer picture = makeFrame();
            picture->format = camera->format;
            picture->width = camera->size.width;
            picture->height = camera->size.height;
            picture->colorspace = camera->colorspace;
            picture->color_range = camera->fullRange ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
            if (av_frame_get_buffer(picture.get(), 0) < 0)
                return false;
            paint(picture.get(), *camera, index);
            pictures.push_back(std::move(picture));
        }

        isOpen = true;
        camera->openNow++;
        openedAt = Clock::now();
        return true;
    }

    Status read(AVFrame *into, Clock::time_point &capturedAt) override {
        if (camera->lostAfter && given >= *camera->lostAfter)
            return Status::Ended;
        camera->doWhatIsDueAtThisPicture();

        const auto interval = std::chrono::duration_cast<Clock::duration>(std::chrono::microseconds(1'000'000 / camera->framerate));
        const int64_t newestTaken = (Clock::now() - openedAt) / interval;
        const int64_t index = std::max(lastGiven + 1, newestTaken);
        const Clock::time_point takenAt = openedAt + index * interval;
        std::this_thread::sleep_until(takenAt);

        av_frame_ref(into, pictures[static_cast<size_t>(index) % pictures.size()].get());
        capturedAt = takenAt;
        lastGiven = index;
        given++;
        camera->picturesGiven++;
        return Status::Frame;
    }

    std::string failure() const override { return isOpen ? "the synthetic camera was lost" : "the synthetic camera does not open"; }
    std::string description() const override { return "a synthetic camera"; }

private:
    std::shared_ptr<SyntheticCamera> camera;
    std::vector<FramePointer> pictures;
    bool isOpen = false;
    Clock::time_point openedAt;
    int64_t lastGiven = -1;
    int given = 0;
};

class KeepingSink : public Core::Media::VideoFrameSink {
public:
    void present(const AVFrame *frame) override {
        std::lock_guard lock(mutex);
        const FrameSize size{frame->width, frame->height};
        if (sizes.empty() || sizes.back() != size)
            sizes.push_back(size);
        av_frame_unref(last.get());
        av_frame_ref(last.get(), frame);
        frames++;
    }

    std::atomic<int> frames{0};
    std::mutex mutex;
    std::vector<FrameSize> sizes;
    FramePointer last = makeFrame();
};

struct Decoded {
    int pictures = 0;
    uint64_t errors = 0;
    uint64_t dropped = 0;
    std::vector<FrameSize> sizes;
    FramePointer last;
};

Decoded decodeAll(VideoCodec codec, const std::vector<EncodedFramePointer> &frames, size_t from = 0, size_t to = SIZE_MAX) {
    const auto sink = std::make_shared<KeepingSink>();
    Decoded decoded;
    int pushed = 0;
    {
        Core::Media::VideoDecoder decoder(false);
        decoder.setSink(sink);

        const auto start = Clock::now();
        const size_t limit = std::min(to, frames.size());
        for (size_t i = from; i < limit; i++) {
            decoder.push(codec, EncodedVideoFrame(*frames[i]));
            pushed++;
            while (pushed - sink->frames > 20 && std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count() < 10000)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        while (sink->frames < pushed && std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count() < 10000)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        decoded.errors = decoder.stats().errors;
        decoded.dropped = decoder.stats().dropped;
    }
    decoded.pictures = sink->frames;
    decoded.sizes = sink->sizes;
    decoded.last = sink->last;
    return decoded;
}

bool isPlanar420(const AVFrame *frame) {
    return frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P;
}

Yuv middleOf(const AVFrame *frame) {
    const int x = frame->width / 2;
    const int y = frame->height / 2;
    return {static_cast<double>(frame->data[0][y * frame->linesize[0] + x]),
            static_cast<double>(frame->data[1][(y / 2) * frame->linesize[1] + x / 2]),
            static_cast<double>(frame->data[2][(y / 2) * frame->linesize[2] + x / 2])};
}

FrameSize brightExtent(const AVFrame *frame) {
    constexpr int BRIGHT = (SQUARE_LUMA + AROUND_SQUARE_LUMA) / 2;
    int across = 0;
    int down = 0;
    for (int x = 0; x < frame->width; x++)
        across += frame->data[0][(frame->height / 2) * frame->linesize[0] + x] > BRIGHT ? 1 : 0;
    for (int y = 0; y < frame->height; y++)
        down += frame->data[0][y * frame->linesize[0] + frame->width / 2] > BRIGHT ? 1 : 0;
    return FrameSize{across, down};
}

Platform::ImageBuffer inRgb(const AVFrame *frame) {
    Platform::ImageBuffer image = Core::Media::RgbScaler::allocate(FrameSize{frame->width, frame->height});
    Core::Media::RgbScaler scaler;
    return scaler.scale(frame, image) ? image : Platform::ImageBuffer{};
}

struct Rgba { uint8_t r = 0, g = 0, b = 0, a = 0; };
Rgba pixelAt(const Platform::ImageBuffer &img, int x, int y) {
    if (x < 0 || x >= img.width || y < 0 || y >= img.height) return {};
    const size_t offset = (static_cast<size_t>(y) * img.width + x) * 4;
    return {img.pixels[offset], img.pixels[offset + 1], img.pixels[offset + 2], img.pixels[offset + 3]};
}

bool isGrey(Rgba pixel, int grey, int within) {
    return std::abs(int(pixel.r) - grey) <= within &&
           std::abs(int(pixel.g) - grey) <= within &&
           std::abs(int(pixel.b) - grey) <= within;
}

std::string describe(Rgba pixel) {
    return "(" + std::to_string(pixel.r) + ", " + std::to_string(pixel.g) + ", " + std::to_string(pixel.b) + ")";
}

std::vector<int> nalTypes(VideoCodec codec, const std::vector<uint8_t> &data) {
    std::vector<int> types;
    for (size_t i = 0; i + 3 < data.size(); i++) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            types.push_back(codec == VideoCodec::H265 ? (data[i + 3] >> 1) & 0x3F : data[i + 3] & 0x1F);
    }
    return types;
}

struct TemporalUnit {
    bool whole = false;
    bool keyFrame = false;
    bool hasSequenceHeader = false;
};

TemporalUnit temporalUnit(const std::vector<uint8_t> &data) {
    constexpr int SEQUENCE_HEADER = 1;
    constexpr int FRAME_HEADER = 3;
    constexpr int FRAME = 6;
    TemporalUnit unit;
    for (size_t at = 0; at < data.size();) {
        const int type = (data[at] >> 3) & 0x0F;
        size_t payload = at + ((data[at] & 0x04) ? 2 : 1);
        if ((data[at] & 0x80) || !(data[at] & 0x02))
            return {};
        size_t size = 0;
        uint8_t byte = 0x80;
        for (int shift = 0; byte & 0x80; shift += 7) {
            if (payload >= data.size() || shift > 28)
                return {};
            byte = data[payload++];
            size |= size_t(byte & 0x7F) << shift;
        }
        if (size > data.size() - payload)
            return {};
        if (type == SEQUENCE_HEADER && !unit.whole)
            unit.hasSequenceHeader = true;
        if ((type == FRAME || type == FRAME_HEADER) && !unit.whole && size > 0) {
            unit.whole = true;
            unit.keyFrame = !(data[payload] & 0x80) && ((data[payload] >> 5) & 0x03) == 0 && (data[payload] & 0x10);
        }
        at = payload + size;
    }
    return unit;
}

bool isWellFormed(VideoCodec codec, const EncodedVideoFrame &frame) {
    if (frame.data.empty())
        return false;
    if (codec == VideoCodec::VP8)
        return !(frame.data[0] & 1) == frame.keyframe;
    if (codec == VideoCodec::AV1) {
        const TemporalUnit unit = temporalUnit(frame.data);
        return unit.whole && unit.keyFrame == frame.keyframe && (!unit.keyFrame || unit.hasSequenceHeader);
    }

    const bool h265 = codec == VideoCodec::H265;
    const std::vector<int> types = nalTypes(codec, frame.data);
    const bool hasKeyPicture = std::any_of(types.begin(), types.end(), [h265](int type) { return h265 ? type >= 16 && type <= 23 : type == 5; });
    const bool startsWithStartCode = frame.data.size() >= 4 && frame.data[0] == 0 && frame.data[1] == 0 && (frame.data[2] == 1 || (frame.data[2] == 0 && frame.data[3] == 1));
    if (!startsWithStartCode || hasKeyPicture != frame.keyframe)
        return false;
    if (!frame.keyframe)
        return true;
    if (h265)
        return types.size() >= 4 && types[0] == 32 && types[1] == 33 && types[2] == 34;
    return types.size() >= 3 && types[0] == 7 && types[1] == 8;
}

bool arePicturesOfTheSizeSent(VideoCodec codec, const std::vector<FrameSize> &decoded, const FrameSize &sent) {
    if (decoded.size() != 1)
        return false;
    const int diffW = decoded.front().width - sent.width;
    const int diffH = decoded.front().height - sent.height;
    return (diffW == 0 && diffH == 0) || (codec == VideoCodec::AV1 && diffW >= 0 && diffW < 64 && diffH >= 0 && diffH < 16);
}

struct PipelineRecord {
    struct Start {
        FrameSize size;
        int framerate = 0;
        bool operator==(const Start &) const = default;
    };

    explicit PipelineRecord(CameraPipeline &pipeline) {
        clockStart = Clock::now();
        startedConn = pipeline.started.connect([this](const FrameSize &size, int framerate, int layer) {
            std::lock_guard lock(mutex);
            (layer == 0 ? starts : secondStarts).push_back({size, framerate});
        });
        frameEncodedConn = pipeline.frameEncoded.connect([this](const EncodedFramePointer &frame, int layer) {
            std::lock_guard lock(mutex);
            if (layer != 0) {
                secondFrames.push_back(frame);
                return;
            }
            frames.push_back(frame);
            if (firstFrameAt < 0) {
                firstFrameAt = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - clockStart).count();
            }
        });
        previewReadyConn = pipeline.previewReady.connect([this](const Platform::ImageBuffer &preview) {
            std::lock_guard lock(mutex);
            previews.push_back(preview);
        });
        statsUpdatedConn = pipeline.statsUpdated.connect([this](const BroadcastStats &update, int layer) {
            std::lock_guard lock(mutex);
            if (layer != 0)
                return;
            stats.push_back(update);
            statsAt.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - clockStart).count());
        });
        failedConn = pipeline.failed.connect([this](const std::string &reason) {
            std::lock_guard lock(mutex);
            failures.push_back(reason);
        });
    }

    [[nodiscard]] size_t keyFrameFrom(size_t from) const {
        std::lock_guard lock(mutex);
        size_t index = from;
        while (index < frames.size() && !frames[index]->keyframe)
            index++;
        return index;
    }

    [[nodiscard]] int keyFramesFrom(size_t from) const {
        std::lock_guard lock(mutex);
        if (from >= frames.size())
            return 0;
        return static_cast<int>(std::count_if(frames.begin() + static_cast<std::ptrdiff_t>(from), frames.end(), [](const EncodedFramePointer &frame) { return frame->keyframe; }));
    }

    [[nodiscard]] std::vector<int> ticksBetween(size_t from = 0) const {
        std::lock_guard lock(mutex);
        std::vector<int> ticks;
        if (from + 1 >= frames.size())
            return ticks;
        for (size_t i = from + 1; i < frames.size(); i++)
            ticks.push_back(static_cast<int32_t>(frames[i]->rtpTimestamp - frames[i - 1]->rtpTimestamp));
        return ticks;
    }

    mutable std::mutex mutex;
    Clock::time_point clockStart;
    int64_t firstFrameAt = -1;
    std::vector<EncodedFramePointer> frames;
    std::vector<EncodedFramePointer> secondFrames;
    std::vector<Start> secondStarts;
    std::vector<Platform::ImageBuffer> previews;
    std::vector<Start> starts;
    std::vector<BroadcastStats> stats;
    std::vector<int64_t> statsAt;
    std::vector<std::string> failures;

    Platform::Connection startedConn;
    Platform::Connection frameEncodedConn;
    Platform::Connection previewReadyConn;
    Platform::Connection statsUpdatedConn;
    Platform::Connection failedConn;
};

double framesPerSecond(const std::vector<int> &ticksBetween) {
    int64_t total = 0;
    for (int ticks : ticksBetween)
        total += ticks;
    return total > 0 ? static_cast<double>(ticksBetween.size()) * TICKS_PER_SECOND / static_cast<double>(total) : 0;
}

CameraSettings settingsFor(VideoCodec codec, const FrameSize &encodeSize, int framerate) {
    CameraSettings settings;
    settings.deviceId = "the camera of the test's own";
    settings.codec = codec;
    settings.encodeSize = encodeSize;
    settings.framerate = framerate;
    settings.bitrate = BITRATE;
    settings.maxBitrate = 4'000'000;
    return settings;
}

template <typename F>
bool waitFor(F &&pred, int timeoutMs = 5000) {
    const auto start = Clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count() < timeoutMs) {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

void waitMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

struct TestFixture {
    std::shared_ptr<SyntheticCamera> camera = std::make_shared<SyntheticCamera>();

    TestFixture() {
        setCameraFrameSourceFactory([made = camera] { return std::make_unique<SyntheticSource>(made); });
    }

    ~TestFixture() {
        setCameraFrameSourceFactory({});
    }
};

constexpr std::chrono::microseconds CAMERA_INTERVAL(33'333);
constexpr std::chrono::microseconds QUICKEST_READ(4'000);

#define SKIP_WITHOUT_AN_ENCODER(codecVar) \
    const auto encodableList = Core::Media::encodableVideoCodecs(); \
    if (encodableList.empty()) { \
        SKIP("this machine has no encoder"); \
    } \
    const auto codecVar = (std::find(encodableList.begin(), encodableList.end(), VideoCodec::H264) != encodableList.end()) \
        ? VideoCodec::H264 \
        : encodableList.front()

#define SKIP_A_CODEC_THAT_IS_NOT_ENCODED(codecVar) \
    if ((codecVar) == VideoCodec::Unknown) { \
        SKIP("this machine has no encoder"); \
    }

} // namespace

TEST_CASE("Cameras are listed the same every time", "[camera][capture]") {
    const auto start = Clock::now();
    const auto first = cameraDevices();
    const auto firstListingMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    (void)firstListingMs;

    std::vector<std::string> ids;
    for (const auto &each : first) {
        CHECK(!each.id.empty());
        CHECK(!each.name.empty());
        ids.push_back(each.id);
    }
    std::sort(ids.begin(), ids.end());
    CHECK(std::unique(ids.begin(), ids.end()) == ids.end());
    if (!cameraCaptureAvailable())
        CHECK(first.empty());

    for (int again = 0; again < 3; again++) {
        const auto listed = cameraDevices();
        REQUIRE(listed.size() == first.size());
        for (size_t i = 0; i < listed.size(); i++) {
            CHECK(listed[i].id == first[i].id);
            CHECK(listed[i].name == first[i].name);
        }
    }
}

TEST_CASE("A camera's own clock says when a picture was taken", "[camera][capture]") {
    using namespace std::chrono;
    constexpr int PICTURES = 300;
    constexpr int READ_VERY_LATE = 150;
    const Clock::time_point began = Clock::time_point(seconds(1000));
    const microseconds cameraBegan = seconds(77);

    CaptureClock clock;
    uint32_t noise = 1;
    std::vector<Clock::time_point> taken;
    int64_t readFurthestApart = 0;
    Clock::time_point lastReadAt;
    for (int index = 0; index < PICTURES; index++) {
        noise = noise * 1664525u + 1013904223u;
        microseconds late((noise >> 8) % 16'000);
        if (index == 0)
            late = microseconds(0);
        if (index == READ_VERY_LATE)
            late = milliseconds(200);
        const Clock::time_point readAt = began + index * CAMERA_INTERVAL + QUICKEST_READ + late;
        if (index > 0)
            readFurthestApart = std::max<int64_t>(readFurthestApart, duration_cast<microseconds>(readAt - lastReadAt).count());
        lastReadAt = readAt;

        const Clock::time_point at = clock.at(cameraBegan + index * CAMERA_INTERVAL, readAt);
        CHECK(at <= readAt);
        taken.push_back(at);
    }

    CHECK(readFurthestApart > 200'000);
    CHECK(taken[0] == began + QUICKEST_READ);
    for (size_t i = 1; i < taken.size(); i++) {
        const int64_t apart = duration_cast<microseconds>(taken[i] - taken[i - 1]).count();
        CHECK(std::abs(apart - CAMERA_INTERVAL.count()) <= 1'000);
    }

    const Clock::time_point later = began + PICTURES * CAMERA_INTERVAL + seconds(1);
    CHECK(clock.at(std::nullopt, later) == later);
    CHECK(clock.at(std::nullopt, later) > later);
}

TEST_CASE("A camera clock that runs slow is followed", "[camera][capture]") {
    using namespace std::chrono;
    constexpr int PICTURES = 36'000;
    const Clock::time_point began = Clock::time_point(seconds(1000));

    CaptureClock clock;
    for (int index = 0; index < PICTURES; index++) {
        const microseconds stamped = index * CAMERA_INTERVAL - index * CAMERA_INTERVAL / 10'000;
        const Clock::time_point readAt = began + index * CAMERA_INTERVAL + QUICKEST_READ;
        const int64_t behind = duration_cast<microseconds>(readAt - clock.at(stamped, readAt)).count();
        CHECK((behind >= 0 && behind <= 1'000));
    }
}

TEST_CASE("A camera clock that starts over is taken up again", "[camera][capture]") {
    using namespace std::chrono;
    constexpr int PICTURES = 90;
    constexpr int STARTS_OVER_AT = 30;
    constexpr int LEAPS_AHEAD_AT = 60;
    const Clock::time_point began = Clock::time_point(seconds(1000));

    CaptureClock clock;
    std::optional<Clock::time_point> last;
    for (int index = 0; index < PICTURES; index++) {
        microseconds stamped = seconds(500) + index * CAMERA_INTERVAL;
        if (index >= STARTS_OVER_AT)
            stamped -= seconds(500);
        if (index >= LEAPS_AHEAD_AT)
            stamped += hours(3);
        const Clock::time_point readAt = began + index * CAMERA_INTERVAL + QUICKEST_READ;
        const Clock::time_point at = clock.at(stamped, readAt);
        CHECK(at == readAt);
        CHECK((!last || at > *last));
        last = at;
    }
}

TEST_CASE("Pictures are encoded at the size asked for", "[camera][capture]") {
    TestFixture fixture;
    const auto encodable = Core::Media::encodableVideoCodecs();
    if (encodable.empty())
        SKIP("this machine has no encoder");

    for (VideoCodec codec : encodable) {
        DYNAMIC_SECTION("codec: " << videoCodecName(codec)) {
            fixture.camera->size = FrameSize{1280, 720};
            const FrameSize encodeSize{640, 360};

            CameraPipeline pipeline;
            PipelineRecord record(pipeline);
            const CameraSettings settings = settingsFor(codec, encodeSize, 30);
            pipeline.start(settings);

            REQUIRE(waitFor([&] {
                std::lock_guard lock(record.mutex);
                return record.frames.size() >= 30 || !record.failures.empty();
            }, 5000));
            {
                std::lock_guard lock(record.mutex);
                REQUIRE(record.failures.empty());
            }
            pipeline.stop();
            REQUIRE(waitFor([&] { return fixture.camera->openNow.load() == 0; }, 2000));

            CHECK(fixture.camera->timesOpened.load() == 1);
            CHECK(fixture.camera->openedWithId == settings.deviceId);
            REQUIRE(record.starts.size() == 1);
            CHECK(record.starts.front().size == encodeSize);
            CHECK(record.starts.front().framerate == 30);
            CHECK(record.frames[0]->keyframe);
            CHECK(record.keyFramesFrom(1) == 0);

            const std::vector<int> ticks = record.ticksBetween();
            for (int each : ticks) {
                CHECK(each > 0);
                CHECK(std::abs(each - 3000 * static_cast<int>(std::lround(each / 3000.0))) <= 2);
            }
            const double fps = framesPerSecond(ticks);
            CHECK(fps > 30 * 0.8);
            CHECK(fps < 30 * 1.05);

            for (const EncodedFramePointer &frame : record.frames)
                CHECK(isWellFormed(codec, *frame));
            const Decoded decoded = decodeAll(codec, record.frames);
            CHECK(decoded.errors == 0);
            CHECK(decoded.dropped == 0);
            CHECK(decoded.pictures == static_cast<int>(record.frames.size()));
            CHECK(arePicturesOfTheSizeSent(codec, decoded.sizes, encodeSize));
        }
    }
}

TEST_CASE("A camera of another shape keeps its shape", "[camera][capture]") {
    TestFixture fixture;
    SKIP_WITHOUT_AN_ENCODER(codec);

    struct Case {
        std::string name;
        FrameSize cameraSize;
        FrameSize encodeSize;
        FrameSize expected;
    };
    const std::vector<Case> cases = {
        {"4:3 inside 16:9", {640, 480}, {640, 360}, {480, 360}},
        {"16:9 inside 4:3", {1280, 720}, {640, 480}, {640, 360}},
        {"a small camera is not enlarged", {640, 480}, {1280, 720}, {640, 480}},
        {"odd sizes are made even", {640, 480}, {300, 169}, {224, 168}},
    };

    for (const auto &c : cases) {
        DYNAMIC_SECTION(c.name) {
            fixture.camera->size = c.cameraSize;
            fixture.camera->shows = SyntheticCamera::Shows::Square;

            CameraPipeline pipeline;
            PipelineRecord record(pipeline);
            pipeline.start(settingsFor(codec, c.encodeSize, 30));

            REQUIRE(waitFor([&] {
                std::lock_guard lock(record.mutex);
                return record.frames.size() >= 10 || !record.failures.empty();
            }, 5000));
            {
                std::lock_guard lock(record.mutex);
                REQUIRE(record.failures.empty());
            }
            pipeline.stop();

            REQUIRE(record.starts.size() == 1);
            CHECK(record.starts.front().size == c.expected);
            const Decoded decoded = decodeAll(codec, record.frames);
            CHECK(decoded.errors == 0);
            REQUIRE(decoded.sizes.size() == 1);
            CHECK(decoded.sizes.front() == c.expected);

            REQUIRE(decoded.last != nullptr);
            CHECK(isPlanar420(decoded.last.get()));
            const FrameSize square = brightExtent(decoded.last.get());
            const int side = c.cameraSize.height / 3 * c.expected.height / c.cameraSize.height;
            CHECK(std::abs(square.width - side) <= 3);
            CHECK(std::abs(square.height - side) <= 3);
        }
    }
}

TEST_CASE("Pictures are thinned to the frame rate", "[camera][capture]") {
    TestFixture fixture;
    SKIP_WITHOUT_AN_ENCODER(codec);

    struct Case {
        std::string name;
        int cameraRate;
        int wanted;
        int expected;
    };
    const std::vector<Case> cases = {
        {"30 of 30", 30, 30, 30},
        {"20 of 30", 30, 20, 20},
        {"a camera with fewer gives all it has", 15, 30, 15},
    };

    for (const auto &c : cases) {
        DYNAMIC_SECTION(c.name) {
            fixture.camera->framerate = c.cameraRate;

            CameraPipeline pipeline;
            PipelineRecord record(pipeline);
            pipeline.start(settingsFor(codec, FrameSize{640, 360}, c.wanted));
            const size_t enough = static_cast<size_t>(c.expected) * 3 / 2;

            REQUIRE(waitFor([&] {
                std::lock_guard lock(record.mutex);
                return record.frames.size() >= enough || !record.failures.empty();
            }, 6000));
            {
                std::lock_guard lock(record.mutex);
                REQUIRE(record.failures.empty());
            }
            pipeline.stop();

            REQUIRE(record.starts.size() == 1);
            CHECK(record.starts.front().framerate == c.wanted);

            const std::vector<int> ticks = record.ticksBetween();
            const double fps = framesPerSecond(ticks);
            CHECK(fps > c.expected * 0.9);
            CHECK(fps < c.expected * 1.05);

            const int closest = TICKS_PER_SECOND / c.expected - TICKS_PER_SECOND / c.cameraRate;
            for (int each : ticks)
                CHECK(each > closest);
        }
    }
}

TEST_CASE("A new size and rate start with a key frame", "[camera][capture]") {
    TestFixture fixture;
    SKIP_WITHOUT_AN_ENCODER(codec);
    const FrameSize larger{640, 360};
    const FrameSize smaller{320, 180};

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    pipeline.start(settingsFor(codec, larger, 30));
    REQUIRE(waitFor([&] {
        std::lock_guard lock(record.mutex);
        return record.frames.size() >= 15 || !record.failures.empty();
    }, 5000));
    {
        std::lock_guard lock(record.mutex);
        REQUIRE(record.failures.empty());
    }

    size_t atChange = 0;
    {
        std::lock_guard lock(record.mutex);
        atChange = record.frames.size();
    }
    pipeline.setEncoding(smaller, 15);
    REQUIRE(waitFor([&] {
        std::lock_guard lock(record.mutex);
        return record.frames.size() >= atChange + 12;
    }, 3000));
    const size_t changed = record.keyFrameFrom(atChange);
    CHECK(changed < atChange + 4);
    CHECK(record.keyFramesFrom(changed + 1) == 0);
    CHECK(record.keyFramesFrom(1) == 1);

    size_t atRepeat = 0;
    {
        std::lock_guard lock(record.mutex);
        atRepeat = record.frames.size();
    }
    pipeline.setEncoding(smaller, 15);
    REQUIRE(waitFor([&] {
        std::lock_guard lock(record.mutex);
        return record.frames.size() >= atRepeat + 8;
    }, 3000));
    const size_t repeated = record.keyFrameFrom(atRepeat);
    CHECK(repeated < atRepeat + 4);
    CHECK(record.keyFramesFrom(repeated + 1) == 0);
    pipeline.stop();
    {
        std::lock_guard lock(record.mutex);
        CHECK(record.failures.empty());
    }

    const std::vector<PipelineRecord::Start> expectedStarts = {{larger, 30}, {smaller, 15}};
    CHECK(record.starts == expectedStarts);
    const double fpsAfter = framesPerSecond(record.ticksBetween(changed));
    CHECK(fpsAfter > 15 * 0.9);
    CHECK(fpsAfter < 15 * 1.05);

    const Decoded before = decodeAll(codec, record.frames, 0, changed);
    CHECK(before.errors == 0);
    CHECK(before.pictures == static_cast<int>(changed));
    REQUIRE(before.sizes.size() == 1);
    CHECK(before.sizes.front() == larger);

    const Decoded after = decodeAll(codec, record.frames, changed);
    CHECK(after.errors == 0);
    CHECK(after.pictures == static_cast<int>(record.frames.size() - changed));
    REQUIRE(after.sizes.size() == 1);
    CHECK(after.sizes.front() == smaller);
}

TEST_CASE("A new bitrate and size together cost one key frame", "[camera][capture]") {
    TestFixture fixture;
    SKIP_WITHOUT_AN_ENCODER(codec);
    constexpr int CHANGES = 3;
    constexpr int LOWER_BITRATE = 300'000;
    const FrameSize larger{640, 360};
    const FrameSize smaller{480, 270};

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    pipeline.start(settingsFor(codec, larger, 30));
    REQUIRE(waitFor([&] {
        std::lock_guard lock(record.mutex);
        return record.frames.size() >= 15 || !record.failures.empty();
    }, 5000));
    {
        std::lock_guard lock(record.mutex);
        REQUIRE(record.failures.empty());
    }

    std::vector<size_t> changedAt = {0};
    std::vector<PipelineRecord::Start> expectedStarts = {{larger, 30}};
    for (int change = 0; change < CHANGES; change++) {
        const bool toSmaller = change % 2 == 0;
        const FrameSize size = toSmaller ? smaller : larger;
        const int bitrate = toSmaller ? LOWER_BITRATE : BITRATE;
        size_t atChange = 0;
        {
            std::lock_guard lock(record.mutex);
            atChange = record.frames.size();
        }

        const auto newBitrate = [&pipeline, bitrate] { pipeline.setBitrate(bitrate); };
        const auto newSize = [&pipeline, size] { pipeline.setEncoding(size, 30); };
        if (change % 3 == 0) {
            newBitrate();
            newSize();
        } else if (change % 3 == 1) {
            fixture.camera->atComingPictures({newBitrate, newSize});
        } else {
            fixture.camera->atComingPictures({newSize, newBitrate});
        }

        REQUIRE(waitFor([&] {
            std::lock_guard lock(record.mutex);
            return record.frames.size() >= atChange + 20;
        }, 3000));
        CHECK(record.keyFramesFrom(atChange) == 1);
        changedAt.push_back(record.keyFrameFrom(atChange));
        expectedStarts.push_back({size, 30});
    }

    waitMs(500);
    size_t measuredFrom = 0;
    {
        std::lock_guard lock(record.mutex);
        measuredFrom = record.frames.size();
    }
    waitMs(1000);
    size_t bytes = 0;
    {
        std::lock_guard lock(record.mutex);
        for (size_t i = measuredFrom; i < record.frames.size(); i++)
            bytes += record.frames[i]->data.size();
    }
    const double bitsPerSecond = static_cast<double>(bytes) * 8.0;
    pipeline.stop();
    {
        std::lock_guard lock(record.mutex);
        CHECK(record.failures.empty());
    }
    CHECK(bitsPerSecond > LOWER_BITRATE * 0.5);
    CHECK(bitsPerSecond < LOWER_BITRATE * 1.5);
    CHECK(record.keyFramesFrom(0) == CHANGES + 1);
    CHECK(record.starts == expectedStarts);

    changedAt.push_back(record.frames.size());
    for (size_t i = 0; i + 1 < changedAt.size(); i++) {
        const Decoded decoded = decodeAll(codec, record.frames, changedAt[i], changedAt[i + 1]);
        CHECK(decoded.errors == 0);
        CHECK(decoded.pictures == static_cast<int>(changedAt[i + 1] - changedAt[i]));
        REQUIRE(decoded.sizes.size() == 1);
        CHECK(decoded.sizes.front() == expectedStarts[i].size);
    }
}

TEST_CASE("What is asked for before the first frame shapes it", "[camera][capture]") {
    TestFixture fixture;
    SKIP_WITHOUT_AN_ENCODER(codec);
    constexpr int LOWER_BITRATE = 300'000;
    const FrameSize smaller{480, 270};

    struct Case {
        std::string name;
        int askedAfterMs;
        int betweenTheTwoMs;
    };
    const std::vector<Case> cases = {
        {"as soon as it is started", 0, 0},
        {"one and then the other while the camera is being opened", 100, 50},
    };

    for (const auto &c : cases) {
        DYNAMIC_SECTION(c.name) {
            fixture.camera->opensAfterMs = 400;

            CameraPipeline pipeline;
            PipelineRecord record(pipeline);
            pipeline.start(settingsFor(codec, FrameSize{640, 360}, 30));
            waitMs(c.askedAfterMs);
            pipeline.setBitrate(LOWER_BITRATE);
            waitMs(c.betweenTheTwoMs);
            pipeline.setEncoding(smaller, 15);
            {
                std::lock_guard lock(record.mutex);
                CHECK(record.frames.empty());
            }

            REQUIRE(waitFor([&] {
                std::lock_guard lock(record.mutex);
                return record.frames.size() >= 38 || !record.failures.empty();
            }, 6000));
            {
                std::lock_guard lock(record.mutex);
                REQUIRE(record.failures.empty());
            }
            pipeline.stop();

            CHECK(record.frames[0]->keyframe);
            CHECK(record.keyFramesFrom(1) == 0);
            const std::vector<PipelineRecord::Start> expectedStarts = {{smaller, 15}};
            CHECK(record.starts == expectedStarts);

            const std::vector<int> ticks = record.ticksBetween();
            const double fps = framesPerSecond(ticks);
            CHECK(fps > 15 * 0.9);
            CHECK(fps < 15 * 1.05);

            constexpr size_t SETTLED = 15;
            size_t bytes = 0;
            int64_t ticksMeasured = 0;
            for (size_t i = SETTLED; i < record.frames.size(); i++) {
                bytes += record.frames[i]->data.size();
                ticksMeasured += ticks[i - 1];
            }
            const double bitsPerSecond = static_cast<double>(bytes) * 8.0 * TICKS_PER_SECOND / static_cast<double>(ticksMeasured);
            CHECK(bitsPerSecond > LOWER_BITRATE * 0.5);
            CHECK(bitsPerSecond < LOWER_BITRATE * 1.5);

            const Decoded decoded = decodeAll(codec, record.frames);
            CHECK(decoded.errors == 0);
            CHECK(decoded.pictures == static_cast<int>(record.frames.size()));
            REQUIRE(decoded.sizes.size() == 1);
            CHECK(decoded.sizes.front() == smaller);
        }
    }
}

TEST_CASE("A key frame comes when asked for", "[camera][capture]") {
    TestFixture fixture;
    SKIP_WITHOUT_AN_ENCODER(codec);

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    pipeline.start(settingsFor(codec, FrameSize{640, 360}, 30));
    REQUIRE(waitFor([&] {
        std::lock_guard lock(record.mutex);
        return record.frames.size() >= 20 || !record.failures.empty();
    }, 5000));
    {
        std::lock_guard lock(record.mutex);
        REQUIRE(record.failures.empty());
    }
    CHECK(record.frames[0]->keyframe);
    CHECK(record.keyFramesFrom(1) == 0);

    size_t atRequest = 0;
    {
        std::lock_guard lock(record.mutex);
        atRequest = record.frames.size();
    }
    pipeline.requestKeyframe();
    REQUIRE(waitFor([&] {
        std::lock_guard lock(record.mutex);
        return record.frames.size() >= atRequest + 10;
    }, 2000));
    const size_t forced = record.keyFrameFrom(atRequest);
    CHECK(forced < atRequest + 4);
    CHECK(record.keyFramesFrom(forced + 1) == 0);
    pipeline.stop();

    const Decoded decoded = decodeAll(codec, record.frames, forced);
    CHECK(decoded.errors == 0);
    CHECK(decoded.pictures == static_cast<int>(record.frames.size() - forced));
}

TEST_CASE("Nothing is encoded while suspended", "[camera][capture]") {
    TestFixture fixture;
    SKIP_WITHOUT_AN_ENCODER(codec);

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    pipeline.start(settingsFor(codec, FrameSize{640, 360}, 30));
    REQUIRE(waitFor([&] {
        std::lock_guard lock(record.mutex);
        return record.frames.size() >= 15 || !record.failures.empty();
    }, 5000));
    {
        std::lock_guard lock(record.mutex);
        REQUIRE(record.failures.empty());
        CHECK(!record.previews.empty());
    }

    pipeline.setEncodingSuspended(true);
    waitMs(300);
    size_t whileSuspended = 0;
    int previewsBefore = 0;
    {
        std::lock_guard lock(record.mutex);
        whileSuspended = record.frames.size();
        previewsBefore = static_cast<int>(record.previews.size());
    }
    const int picturesBefore = fixture.camera->picturesGiven.load();
    pipeline.requestKeyframe();
    waitMs(1000);

    {
        std::lock_guard lock(record.mutex);
        CHECK(record.frames.size() == whileSuspended);
    }
    CHECK(fixture.camera->openNow.load() == 1);
    CHECK(fixture.camera->picturesGiven - picturesBefore >= 20);

    int previewsMeanwhile = 0;
    {
        std::lock_guard lock(record.mutex);
        previewsMeanwhile = static_cast<int>(record.previews.size()) - previewsBefore;
    }
    CHECK(previewsMeanwhile >= 10);
    CHECK(previewsMeanwhile <= 20);

    pipeline.setEncodingSuspended(false);
    REQUIRE(waitFor([&] {
        std::lock_guard lock(record.mutex);
        return record.frames.size() >= whileSuspended + 10;
    }, 2000));
    CHECK(record.frames[whileSuspended]->keyframe);
    CHECK(record.keyFramesFrom(whileSuspended + 1) == 0);
    pipeline.stop();
    {
        std::lock_guard lock(record.mutex);
        CHECK(record.failures.empty());
    }
    CHECK(record.starts.size() == 1);

    const Decoded decoded = decodeAll(codec, record.frames);
    CHECK(decoded.errors == 0);
    CHECK(decoded.pictures == static_cast<int>(record.frames.size()));
}

TEST_CASE("A second layer is encoded beside the first once it is asked for", "[camera][capture]") {
    TestFixture fixture;
    SKIP_WITHOUT_AN_ENCODER(codec);

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    pipeline.setEncoding(FrameSize{320, 180}, 15, 1);
    pipeline.setBitrate(150000, 1);
    pipeline.start(settingsFor(codec, FrameSize{640, 360}, 30));

    REQUIRE(waitFor([&] {
        std::lock_guard lock(record.mutex);
        return record.frames.size() >= 15 || !record.failures.empty();
    }, 5000));
    {
        std::lock_guard lock(record.mutex);
        REQUIRE(record.failures.empty());
        CHECK(record.secondFrames.empty());
        CHECK(record.secondStarts.empty());
    }

    size_t firstLayerBefore = 0;
    {
        std::lock_guard lock(record.mutex);
        firstLayerBefore = record.frames.size();
    }
    pipeline.setEncodingSuspended(false, 1);
    REQUIRE(waitFor([&] {
        std::lock_guard lock(record.mutex);
        return record.secondFrames.size() >= 10 || !record.failures.empty();
    }, 5000));
    {
        std::lock_guard lock(record.mutex);
        REQUIRE(record.failures.empty());
        CHECK(record.secondFrames[0]->keyframe);
        REQUIRE(record.secondStarts.size() == 1);
        CHECK(record.secondStarts.front().size == FrameSize{320, 180});
        CHECK(record.secondStarts.front().framerate == 15);
        CHECK(record.frames.size() > firstLayerBefore + 10);
    }
    CHECK(record.keyFramesFrom(firstLayerBefore) == 0);
    CHECK(record.starts.size() == 1);

    size_t secondLayerBefore = 0;
    size_t firstLayerThen = 0;
    {
        std::lock_guard lock(record.mutex);
        secondLayerBefore = record.secondFrames.size();
        firstLayerThen = record.frames.size();
    }
    pipeline.requestKeyframe(1);
    const auto keyFrameSince = [&record, secondLayerBefore] {
        std::lock_guard lock(record.mutex);
        return std::any_of(record.secondFrames.begin() + static_cast<std::ptrdiff_t>(secondLayerBefore), record.secondFrames.end(), [](const EncodedFramePointer &frame) { return frame->keyframe; });
    };
    REQUIRE(waitFor(keyFrameSince, 2000));
    CHECK(record.keyFramesFrom(firstLayerThen) == 0);

    pipeline.setEncodingSuspended(true, 1);
    waitMs(300);
    size_t secondLayerStopped = 0;
    size_t firstLayerMeanwhile = 0;
    {
        std::lock_guard lock(record.mutex);
        secondLayerStopped = record.secondFrames.size();
        firstLayerMeanwhile = record.frames.size();
    }
    waitMs(500);
    {
        std::lock_guard lock(record.mutex);
        CHECK(record.secondFrames.size() == secondLayerStopped);
        CHECK(record.frames.size() > firstLayerMeanwhile + 5);
    }
    pipeline.stop();
    {
        std::lock_guard lock(record.mutex);
        CHECK(record.failures.empty());
    }

    const Decoded first = decodeAll(codec, record.frames);
    CHECK(first.errors == 0);
    REQUIRE(first.sizes.size() == 1);
    CHECK(first.sizes.front() == FrameSize{640, 360});

    const Decoded second = decodeAll(codec, record.secondFrames);
    CHECK(second.errors == 0);
    CHECK(second.pictures == static_cast<int>(record.secondFrames.size()));
    REQUIRE(second.sizes.size() == 1);
    CHECK(second.sizes.front() == FrameSize{320, 180});
}

TEST_CASE("A camera opened while suspended is announced", "[camera][capture]") {
    TestFixture fixture;
    SKIP_WITHOUT_AN_ENCODER(codec);
    fixture.camera->size = FrameSize{640, 480};

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    pipeline.setEncodingSuspended(true);
    pipeline.start(settingsFor(codec, FrameSize{640, 360}, 30));

    REQUIRE(waitFor([&] {
        std::lock_guard lock(record.mutex);
        return !record.starts.empty() || !record.failures.empty();
    }, 5000));
    {
        std::lock_guard lock(record.mutex);
        REQUIRE(record.failures.empty());
        REQUIRE(!record.starts.empty());
        CHECK(record.starts.front().size == FrameSize{480, 360});
    }

    REQUIRE(waitFor([&] {
        std::lock_guard lock(record.mutex);
        return record.previews.size() >= 5;
    }, 2000));
    {
        std::lock_guard lock(record.mutex);
        CHECK(record.frames.empty());
    }

    pipeline.setEncodingSuspended(false);
    REQUIRE(waitFor([&] {
        std::lock_guard lock(record.mutex);
        return record.frames.size() >= 5;
    }, 3000));
    CHECK(record.frames[0]->keyframe);
    CHECK(record.starts.size() == 1);
    pipeline.stop();

    const Decoded decoded = decodeAll(codec, record.frames);
    CHECK(decoded.errors == 0);
    REQUIRE(decoded.sizes.size() == 1);
    CHECK(decoded.sizes.front() == FrameSize{480, 360});
}

TEST_CASE("A full-range camera keeps its brightness", "[camera][capture]") {
    TestFixture fixture;
    const auto encodable = Core::Media::encodableVideoCodecs();
    if (encodable.empty())
        SKIP("this machine has no encoder");

    for (VideoCodec codec : encodable) {
        DYNAMIC_SECTION("codec: " << videoCodecName(codec)) {
            constexpr int WITHIN = 4;
            fixture.camera->format = AV_PIX_FMT_YUVJ422P;
            fixture.camera->fullRange = true;
            fixture.camera->shows = SyntheticCamera::Shows::TwoGreys;

            CameraPipeline pipeline;
            PipelineRecord record(pipeline);
            pipeline.start(settingsFor(codec, FrameSize{640, 360}, 30));

            REQUIRE(waitFor([&] {
                std::lock_guard lock(record.mutex);
                return (record.frames.size() >= 10 && !record.previews.empty()) || !record.failures.empty();
            }, 5000));
            {
                std::lock_guard lock(record.mutex);
                REQUIRE(record.failures.empty());
            }
            pipeline.stop();

            const Decoded decoded = decodeAll(codec, record.frames);
            CHECK(decoded.errors == 0);
            CHECK(arePicturesOfTheSizeSent(codec, decoded.sizes, FrameSize{640, 360}));

            const Platform::ImageBuffer sent = inRgb(decoded.last.get());
            REQUIRE(!sent.empty());
            const Rgba dark = pixelAt(sent, sent.width / 4, sent.height / 2);
            const Rgba light = pixelAt(sent, sent.width * 3 / 4, sent.height / 2);
            CHECK(isGrey(dark, DARK_GREY, WITHIN));
            CHECK(isGrey(light, LIGHT_GREY, WITHIN));

            Platform::ImageBuffer preview;
            {
                std::lock_guard lock(record.mutex);
                preview = record.previews.back();
            }
            const Rgba previewDark = pixelAt(preview, preview.width / 4, preview.height / 2);
            const Rgba previewLight = pixelAt(preview, preview.width * 3 / 4, preview.height / 2);
            CHECK(isGrey(previewDark, DARK_GREY, WITHIN));
            CHECK(isGrey(previewLight, LIGHT_GREY, WITHIN));
        }
    }
}

TEST_CASE("A camera's colours are what a viewer takes them for", "[camera][capture]") {
    TestFixture fixture;
    const auto encodable = Core::Media::encodableVideoCodecs();
    if (encodable.empty())
        SKIP("this machine has no encoder");

    for (VideoCodec codec : encodable) {
        for (const FrameSize &size : {FrameSize{1280, 720}, FrameSize{640, 360}}) {
            for (AVColorSpace cameraColorspace : {AVCOL_SPC_SMPTE170M, AVCOL_SPC_BT709}) {
                const std::string name = videoCodecName(codec) + " at " + std::to_string(size.width) + "x" + std::to_string(size.height) + " from " + (cameraColorspace == AVCOL_SPC_BT709 ? "BT.709" : "BT.601");
                DYNAMIC_SECTION(name) {
                    fixture.camera->size = FrameSize{1280, 720};
                    fixture.camera->format = AV_PIX_FMT_YUVJ422P;
                    fixture.camera->fullRange = true;
                    fixture.camera->colorspace = cameraColorspace;
                    fixture.camera->shows = SyntheticCamera::Shows::OneColour;

                    CameraPipeline pipeline;
                    PipelineRecord record(pipeline);
                    pipeline.start(settingsFor(codec, size, 30));

                    REQUIRE(waitFor([&] {
                        std::lock_guard lock(record.mutex);
                        return record.frames.size() >= 10 || !record.failures.empty();
                    }, 5000));
                    {
                        std::lock_guard lock(record.mutex);
                        REQUIRE(record.failures.empty());
                    }
                    pipeline.stop();

                    const Decoded decoded = decodeAll(codec, record.frames);
                    CHECK(decoded.errors == 0);
                    CHECK(arePicturesOfTheSizeSent(codec, decoded.sizes, size));
                    REQUIRE(decoded.last != nullptr);
                    CHECK(isPlanar420(decoded.last.get()));

                    const bool sentAsBt709 = codec != VideoCodec::VP8 && size.height >= 720;
                    const Yuv middle = middleOf(decoded.last.get());
                    const Yuv right = inYuv(COLOUR_RED, COLOUR_GREEN, COLOUR_BLUE, sentAsBt709, false);
                    const Yuv wrong = inYuv(COLOUR_RED, COLOUR_GREEN, COLOUR_BLUE, !sentAsBt709, false);

                    CHECK(distance(middle, right) < 6);
                    CHECK(distance(middle, wrong) > 12);
                    CHECK(decoded.last->color_range != AVCOL_RANGE_JPEG);

                    if (codec != VideoCodec::VP8) {
                        CHECK(Core::Media::usesBt709(decoded.last.get()) == sentAsBt709);
                        const Rgba shown = pixelAt(inRgb(decoded.last.get()), size.width / 2, size.height / 2);
                        CHECK(std::abs(int(shown.r) - COLOUR_RED) <= 6);
                        CHECK(std::abs(int(shown.g) - COLOUR_GREEN) <= 6);
                        CHECK(std::abs(int(shown.b) - COLOUR_BLUE) <= 6);
                    }
                }
            }
        }
    }
}

TEST_CASE("The preview is small and frequent", "[camera][capture]") {
    TestFixture fixture;
    SKIP_WITHOUT_AN_ENCODER(codec);

    struct Case {
        std::string name;
        FrameSize cameraSize;
        FrameSize expected;
    };
    const std::vector<Case> cases = {
        {"1280x720", {1280, 720}, {640, 360}},
        {"4:3", {640, 480}, {480, 360}},
        {"a small camera is not enlarged", {320, 240}, {320, 240}},
    };

    for (const auto &c : cases) {
        DYNAMIC_SECTION(c.name) {
            fixture.camera->size = c.cameraSize;
            fixture.camera->shows = SyntheticCamera::Shows::Square;

            CameraPipeline pipeline;
            PipelineRecord record(pipeline);
            pipeline.start(settingsFor(codec, FrameSize{320, 180}, 30));

            REQUIRE(waitFor([&] {
                std::lock_guard lock(record.mutex);
                return !record.previews.empty() || !record.failures.empty();
            }, 5000));
            {
                std::lock_guard lock(record.mutex);
                REQUIRE(record.failures.empty());
            }

            if (c.cameraSize == FrameSize{1280, 720}) {
                int before = 0;
                {
                    std::lock_guard lock(record.mutex);
                    before = static_cast<int>(record.previews.size());
                }
                waitMs(2000);
                int inTwoSeconds = 0;
                {
                    std::lock_guard lock(record.mutex);
                    inTwoSeconds = static_cast<int>(record.previews.size()) - before;
                }
                CHECK(inTwoSeconds >= 24);
                CHECK(inTwoSeconds <= 34);
            } else {
                REQUIRE(waitFor([&] {
                    std::lock_guard lock(record.mutex);
                    return record.previews.size() >= 2;
                }, 2000));
            }
            pipeline.stop();

            std::vector<Platform::ImageBuffer> previewsCopy;
            {
                std::lock_guard lock(record.mutex);
                previewsCopy = record.previews;
            }
            for (const auto &preview : previewsCopy) {
                CHECK(!preview.empty());
                CHECK(preview.width <= 640);
                CHECK(preview.height <= 360);
                CHECK(FrameSize{preview.width, preview.height} == c.expected);
            }
            REQUIRE(previewsCopy.size() >= 2);

            const Platform::ImageBuffer &last = previewsCopy.back();
            int across = 0;
            int down = 0;
            for (int x = 0; x < last.width; x++)
                across += pixelAt(last, x, last.height / 2).g > 128 ? 1 : 0;
            for (int y = 0; y < last.height; y++)
                down += pixelAt(last, last.width / 2, y).g > 128 ? 1 : 0;
            const int side = c.cameraSize.height / 3 * c.expected.height / c.cameraSize.height;
            CHECK(std::abs(across - side) <= 3);
            CHECK(std::abs(down - side) <= 3);
        }
    }
}

TEST_CASE("The bitrate is followed", "[camera][capture]") {
    TestFixture fixture;
    SKIP_WITHOUT_AN_ENCODER(codec);
    constexpr int LOWER_BITRATE = 300'000;
    constexpr int SETTLING_MS = 600;
    constexpr int MEASURED_MS = 1000;

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    CameraSettings settings = settingsFor(codec, FrameSize{640, 360}, 30);
    settings.bitrate = 9'000'000;
    settings.maxBitrate = BITRATE;
    pipeline.start(settings);

    REQUIRE(waitFor([&] {
        std::lock_guard lock(record.mutex);
        return !record.frames.empty() || !record.failures.empty();
    }, 5000));
    {
        std::lock_guard lock(record.mutex);
        REQUIRE(record.failures.empty());
    }

    const auto bitsPerSecond = [&record] {
        waitMs(SETTLING_MS);
        size_t from = 0;
        {
            std::lock_guard lock(record.mutex);
            from = record.frames.size();
        }
        waitMs(MEASURED_MS);
        size_t bytes = 0;
        {
            std::lock_guard lock(record.mutex);
            for (size_t i = from; i < record.frames.size(); i++)
                bytes += record.frames[i]->data.size();
        }
        return static_cast<double>(bytes) * 8.0 * 1000.0 / MEASURED_MS;
    };

    const double before = bitsPerSecond();
    pipeline.setBitrate(LOWER_BITRATE);
    const double after = bitsPerSecond();
    pipeline.stop();

    CHECK(before > BITRATE * 0.5);
    CHECK(before < BITRATE * 1.4);
    CHECK(after > LOWER_BITRATE * 0.5);
    CHECK(after < LOWER_BITRATE * 1.5);
    {
        std::lock_guard lock(record.mutex);
        CHECK(record.failures.empty());
    }

    const Decoded decoded = decodeAll(codec, record.frames);
    CHECK(decoded.errors == 0);
}

TEST_CASE("Statistics come every second", "[camera][capture]") {
    TestFixture fixture;
    SKIP_WITHOUT_AN_ENCODER(codec);
    fixture.camera->size = FrameSize{640, 480};

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    pipeline.start(settingsFor(codec, FrameSize{640, 360}, 15));

    REQUIRE(waitFor([&] {
        std::lock_guard lock(record.mutex);
        return record.stats.size() >= 3 || !record.failures.empty();
    }, 6000));
    {
        std::lock_guard lock(record.mutex);
        REQUIRE(record.failures.empty());
    }
    pipeline.stop();

    std::vector<int64_t> statsAtCopy;
    std::vector<BroadcastStats> statsCopy;
    {
        std::lock_guard lock(record.mutex);
        statsAtCopy = record.statsAt;
        statsCopy = record.stats;
    }
    REQUIRE(statsCopy.size() >= 3);
    for (int i = 1; i < 3; i++) {
        const int64_t apart = statsAtCopy[i] - statsAtCopy[i - 1];
        CHECK(apart > 800);
        CHECK(apart < 1300);
    }

    const BroadcastStats last = statsCopy[2];
    CHECK(last.encoderName.ends_with(", from a synthetic camera"));
    CHECK(!last.encoderName.starts_with("no encoder yet"));
    CHECK(last.encodedSize == FrameSize{480, 360});
    CHECK(last.capturedFps > 30 * 0.85);
    CHECK(last.capturedFps < 30 * 1.15);
    CHECK(last.encodedFps > 15 * 0.85);
    CHECK(last.encodedFps < 15 * 1.15);
    CHECK(last.bitrateBps > 0);
    CHECK(last.averageEncodeMs > 0);
    CHECK(last.framesEncoded >= 30);
    CHECK(last.framesEncoded <= static_cast<uint64_t>(record.frames.size()));
}

TEST_CASE("A camera that is lost fails", "[camera][capture]") {
    TestFixture fixture;
    SKIP_WITHOUT_AN_ENCODER(codec);
    fixture.camera->lostAfter = 12;

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    pipeline.start(settingsFor(codec, FrameSize{640, 360}, 30));

    REQUIRE(waitFor([&] {
        std::lock_guard lock(record.mutex);
        return record.failures.size() == 1;
    }, 5000));
    {
        std::lock_guard lock(record.mutex);
        CHECK(record.failures.front() == "the synthetic camera was lost");
    }
    REQUIRE(waitFor([&] { return fixture.camera->openNow.load() == 0; }, 2000));

    size_t frames = 0;
    {
        std::lock_guard lock(record.mutex);
        frames = record.frames.size();
    }
    CHECK(frames >= 1);
    CHECK(frames <= 12);

    waitMs(500);
    {
        std::lock_guard lock(record.mutex);
        CHECK(record.frames.size() == frames);
        CHECK(record.failures.size() == 1);
    }
    CHECK(fixture.camera->timesOpened.load() == 1);

    const Decoded decoded = decodeAll(codec, record.frames);
    CHECK(decoded.errors == 0);
    CHECK(decoded.pictures == static_cast<int>(frames));
}

TEST_CASE("A camera that does not open fails", "[camera][capture]") {
    TestFixture fixture;
    SKIP_WITHOUT_AN_ENCODER(codec);
    fixture.camera->opens = false;

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    pipeline.start(settingsFor(codec, FrameSize{640, 360}, 30));

    REQUIRE(waitFor([&] {
        std::lock_guard lock(record.mutex);
        return record.failures.size() == 1;
    }, 5000));
    {
        std::lock_guard lock(record.mutex);
        CHECK(record.failures.front() == "the synthetic camera does not open");
    }
    waitMs(300);
    {
        std::lock_guard lock(record.mutex);
        CHECK(record.frames.empty());
        CHECK(record.starts.empty());
        CHECK(record.previews.empty());
    }
    CHECK(fixture.camera->timesOpened.load() == 1);
}

TEST_CASE("No camera is opened without an encoder", "[camera][capture]") {
    TestFixture fixture;
    VideoCodec codec = VideoCodec::Unknown;
    const auto encodable = Core::Media::encodableVideoCodecs();
    for (VideoCodec each : {VideoCodec::H265, VideoCodec::AV1}) {
        if (std::find(encodable.begin(), encodable.end(), each) == encodable.end())
            codec = each;
    }

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    pipeline.start(settingsFor(codec, FrameSize{640, 360}, 30));

    REQUIRE(waitFor([&] {
        std::lock_guard lock(record.mutex);
        return record.failures.size() == 1;
    }, 5000));
    {
        std::lock_guard lock(record.mutex);
        CHECK(record.failures.front().find(videoCodecName(codec)) != std::string::npos);
    }
    waitMs(300);
    CHECK(fixture.camera->timesOpened.load() == 0);
    {
        std::lock_guard lock(record.mutex);
        CHECK(record.frames.empty());
        CHECK(record.starts.empty());
    }
}

TEST_CASE("Stopping closes the camera and it opens again", "[camera][capture]") {
    TestFixture fixture;
    SKIP_WITHOUT_AN_ENCODER(codec);

    CameraPipeline pipeline;
    PipelineRecord record(pipeline);
    const CameraSettings settings = settingsFor(codec, FrameSize{640, 360}, 30);
    pipeline.start(settings);

    REQUIRE(waitFor([&] {
        std::lock_guard lock(record.mutex);
        return record.frames.size() >= 10 || !record.failures.empty();
    }, 5000));
    {
        std::lock_guard lock(record.mutex);
        REQUIRE(record.failures.empty());
    }
    CHECK(fixture.camera->openNow.load() == 1);

    pipeline.stop();
    REQUIRE(waitFor([&] { return fixture.camera->openNow.load() == 0; }, 2000));
    waitMs(100);
    size_t afterStop = 0;
    int previewsAfterStop = 0;
    {
        std::lock_guard lock(record.mutex);
        afterStop = record.frames.size();
        previewsAfterStop = static_cast<int>(record.previews.size());
    }
    waitMs(400);
    {
        std::lock_guard lock(record.mutex);
        CHECK(record.frames.size() == afterStop);
        CHECK(static_cast<int>(record.previews.size()) == previewsAfterStop);
    }

    uint32_t lastTimestamp = 0;
    {
        std::lock_guard lock(record.mutex);
        lastTimestamp = record.frames.back()->rtpTimestamp;
        record.frames.clear();
    }
    pipeline.start(settings);
    REQUIRE(waitFor([&] {
        std::lock_guard lock(record.mutex);
        return record.frames.size() >= 5 || !record.failures.empty();
    }, 5000));
    {
        std::lock_guard lock(record.mutex);
        CHECK(record.failures.empty());
        CHECK(record.starts.size() == 2);
        CHECK(record.frames[0]->keyframe);
        CHECK(static_cast<int32_t>(record.frames[0]->rtpTimestamp - lastTimestamp) > 0);
    }
    CHECK(fixture.camera->timesOpened.load() == 2);

    {
        std::lock_guard lock(record.mutex);
        record.frames.clear();
    }
    pipeline.start(settings);
    REQUIRE(waitFor([&] { return fixture.camera->timesOpened.load() == 3; }, 2000));
    REQUIRE(waitFor([&] { return fixture.camera->openNow.load() == 1; }, 2000));
    {
        std::lock_guard lock(record.mutex);
        CHECK(record.failures.empty());
    }
}
