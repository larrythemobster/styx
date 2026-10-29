// Faithful port of ouwou/acheron upstream/video-streams (71b4907) tests/tst_VideoEncode.cpp to Catch2.
#include "Core/Media/HardwareDevices.hpp"
#include "Core/Media/VideoDecoder.hpp"
#include "Core/Media/VideoEncoder.hpp"
#include "Core/Video/BroadcastPipeline.hpp"
#include "Core/Video/CardScaler.hpp"
#include "Core/Video/CaptureSource.hpp"
#include "Core/Video/ScreenCapture.hpp"
#include "Core/Video/VideoTypes.hpp"
#include "Discord/Voice/VideoPacketizer.hpp"
#include "Discord/Voice/VideoDepacketizer.hpp"
#include "Discord/Voice/VideoReceiver.hpp"
#include "HardwareTests.hpp"
#include "Platform/Image.hpp"
#include "Platform/Settings.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
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
#include <d3d11.h>
#include <dxgi.h>
extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace Styx;
using namespace Styx::Discord::Voice;
using Styx::Core::Media::VideoEncoder;
using Styx::Core::Video::FrameSize;
using Styx::Core::Video::CaptureRect;
using Styx::Core::Video::CaptureSource;
using Styx::Core::Video::CaptureMethod;

namespace {

constexpr int WIDTH = 640;
constexpr int HEIGHT = 360;
constexpr int FRAMERATE = 30;
constexpr int TICKS_PER_FRAME = 90000 / FRAMERATE;
constexpr int BITRATE = 1'500'000;

// In the order the voice server prefers them.
constexpr std::array CODECS = { VideoCodec::AV1, VideoCodec::H265, VideoCodec::H264, VideoCodec::VP8 };

// The colour of the patch in the middle of a synthetic RGB picture.
constexpr int PATCH_RED = 40;
constexpr int PATCH_GREEN = 200;
constexpr int PATCH_BLUE = 60;

#ifdef _WIN32
constexpr COLORREF SHARED_COLOUR = RGB(PATCH_RED, PATCH_GREEN, PATCH_BLUE);
constexpr COLORREF COVERING_COLOUR = RGB(220, 40, 200);
#endif

struct EncoderCase
{
    VideoCodec codec = VideoCodec::Unknown;
    // Empty for whichever encoder the probe settled on.
    std::string encoderName;
    // For the tests that take one: the format of the pictures, or the tallest picture to send.
    int detail = 0;
};

bool isOfTheGraphicsCard(const std::string &encoderName)
{
    for (const char *ending : { "_amf", "_nvenc", "_qsv", "_vaapi", " hardware" }) {
        if (encoderName.ends_with(ending))
            return true;
    }
    return false;
}

// The encoders of a codec that every test of one encoder runs on: the graphics card's only
// where its tests are asked for.
std::vector<std::string> encodersToTest(VideoCodec codec)
{
    std::vector<std::string> names = Core::Media::videoEncoderNames(codec);
    if (!hardwareTestsAreAskedFor()) {
        std::erase_if(names, isOfTheGraphicsCard);
    }
    return names;
}

std::vector<EncoderCase> encoderCases()
{
    std::vector<EncoderCase> cases;
    for (VideoCodec codec : CODECS) {
        for (const std::string &name : encodersToTest(codec))
            cases.push_back(EncoderCase{ codec, name });
    }
    return cases;
}

struct Yuv
{
    double y = 0;
    double u = 0;
    double v = 0;
};

// What a colour comes to in video-range YUV under either set of coefficients.
Yuv inYuv(int red, int green, int blue, bool bt709)
{
    const double kr = bt709 ? 0.2126 : 0.299;
    const double kb = bt709 ? 0.0722 : 0.114;
    const double luma = kr * red + (1 - kr - kb) * green + kb * blue;
    return { 16 + 219 * luma / 255, 128 + 224 * (blue - luma) / (2 * (1 - kb)) / 255, 128 + 224 * (red - luma) / (2 * (1 - kr)) / 255 };
}

Yuv patchInYuv(bool bt709)
{
    return inYuv(PATCH_RED, PATCH_GREEN, PATCH_BLUE, bt709);
}

double distance(const Yuv &a, const Yuv &b)
{
    return std::sqrt((a.y - b.y) * (a.y - b.y) + (a.u - b.u) * (a.u - b.u) + (a.v - b.v) * (a.v - b.v));
}

// Counts decoded pictures, and keeps the colours in the middle and at the top of the last one.
// Each picture is shown to `inspect` with its place among them, on the decoder's thread.
class CountingSink : public Core::Media::VideoFrameSink
{
public:
    static constexpr int TOP_ROW = 4;

    void present(const AVFrame *frame) override
    {
        if (inspect)
            inspect(frame, frames);
        width = frame->width;
        height = frame->height;
        format = frame->format;
        colorspace = frame->colorspace;
        range = frame->color_range;
        shapeWidth = frame->sample_aspect_ratio.num;
        shapeHeight = frame->sample_aspect_ratio.den;
        if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P) {
            const int x = frame->width / 2;
            const int y = frame->height / 2;
            middleY = frame->data[0][y * frame->linesize[0] + x];
            middleU = frame->data[1][(y / 2) * frame->linesize[1] + x / 2];
            middleV = frame->data[2][(y / 2) * frame->linesize[2] + x / 2];
            topY = frame->data[0][TOP_ROW * frame->linesize[0] + x];
            topU = frame->data[1][(TOP_ROW / 2) * frame->linesize[1] + x / 2];
            topV = frame->data[2][(TOP_ROW / 2) * frame->linesize[2] + x / 2];
        }
        frames++;
    }

    std::function<void(const AVFrame *, int)> inspect;
    std::atomic<int> frames{ 0 };
    std::atomic<int> width{ 0 };
    std::atomic<int> height{ 0 };
    std::atomic<int> format{ AV_PIX_FMT_NONE };
    std::atomic<int> colorspace{ AVCOL_SPC_UNSPECIFIED };
    std::atomic<int> range{ AVCOL_RANGE_UNSPECIFIED };
    // The frame's sample aspect ratio, which is how ffmpeg passes on the size AV1 says a
    // picture is to be shown at when that is not the size it was coded at.
    std::atomic<int> shapeWidth{ 0 };
    std::atomic<int> shapeHeight{ 1 };
    std::atomic<int> middleY{ 0 };
    std::atomic<int> middleU{ 0 };
    std::atomic<int> middleV{ 0 };
    std::atomic<int> topY{ 0 };
    std::atomic<int> topU{ 0 };
    std::atomic<int> topV{ 0 };
};

// A gradient that drifts, a patch of fine texture that slides across it, and a band of noise
// that is new in every picture: enough that an encoder has to spend what it is given.
void paintYuv(AVFrame *picture, int index)
{
    const int width = picture->width;
    const int height = picture->height;

    uint32_t noise = 12345u + static_cast<uint32_t>(index) * 2654435761u;
    for (int y = 0; y < height; y++) {
        uint8_t *row = picture->data[0] + y * picture->linesize[0];
        for (int x = 0; x < width; x++) {
            uint8_t value = static_cast<uint8_t>((x + y) / 4 + index * 3);
            if (x >= width / 4 && x < width * 3 / 4 && y >= height / 3 && y < height * 2 / 3)
                value = static_cast<uint8_t>(((x + 3 * index) * 7) ^ ((y + 2 * index) * 5));
            if (x < width / 4 && y < height / 6) {
                noise = noise * 1664525u + 1013904223u;
                value = static_cast<uint8_t>(96 + (noise >> 26));
            }
            row[x] = value;
        }
    }

    const bool interleaved = picture->format == AV_PIX_FMT_NV12;
    for (int y = 0; y < height / 2; y++) {
        for (int x = 0; x < width / 2; x++) {
            const auto u = static_cast<uint8_t>(128 + y / 4 + index * 2);
            const auto v = static_cast<uint8_t>(64 + x / 4 + index * 5);
            if (interleaved) {
                picture->data[1][y * picture->linesize[1] + 2 * x] = u;
                picture->data[1][y * picture->linesize[1] + 2 * x + 1] = v;
            } else {
                picture->data[1][y * picture->linesize[1] + x] = u;
                picture->data[2][y * picture->linesize[2] + x] = v;
            }
        }
    }
}

// Drifting gradients around a patch of one colour, which is what the colour checks look at.
void paintBgra(AVFrame *picture, int index)
{
    const int width = picture->width;
    const int height = picture->height;
    for (int y = 0; y < height; y++) {
        uint8_t *row = picture->data[0] + y * picture->linesize[0];
        for (int x = 0; x < width; x++) {
            const bool inPatch = std::abs(x - width / 2) < width / 8 && std::abs(y - height / 2) < height / 8;
            row[4 * x] = inPatch ? PATCH_BLUE : static_cast<uint8_t>(x / 2 + index * 3);
            row[4 * x + 1] = inPatch ? PATCH_GREEN : static_cast<uint8_t>(y / 2 + index * 2);
            row[4 * x + 2] = inPatch ? PATCH_RED : static_cast<uint8_t>((x + y) / 4);
            row[4 * x + 3] = 255;
        }
    }
}

void paint(AVFrame *picture, int index)
{
    av_frame_make_writable(picture);
    if (picture->format == AV_PIX_FMT_BGRA)
        paintBgra(picture, index);
    else
        paintYuv(picture, index);
}

AVFrame *allocatePicture(int format, int width, int height)
{
    AVFrame *picture = av_frame_alloc();
    picture->format = format;
    picture->width = width;
    picture->height = height;
    av_frame_get_buffer(picture, 0);
    return picture;
}

VideoEncoder::Settings settingsFor(const EncoderCase &encoderCase, int bitrate)
{
    VideoEncoder::Settings settings;
    settings.codec = encoderCase.codec;
    settings.framerate = FRAMERATE;
    settings.bitrate = bitrate;
    settings.encoderName = encoderCase.encoderName;
    return settings;
}

// One encoder fed synthetic pictures from memory, with what each call gave back.
class EncodeRun
{
public:
    EncodeRun(const EncoderCase &encoderCase, int width, int height, int bitrate)
        : encoder(settingsFor(encoderCase, bitrate)), picture(allocatePicture(encoder.softwareFormat(), width, height))
    {
    }

    ~EncodeRun() { av_frame_free(&picture); }

    void paintNext() { paint(picture, sent); }

    // False when the encoder could not be used at all.
    bool encodeNext(bool painting = true)
    {
        if (painting)
            paintNext();
        const size_t before = frames.size();
        const uint32_t timestamp = static_cast<uint32_t>(sent) * TICKS_PER_FRAME;

        if (encoder.encode(picture, timestamp, frames) != VideoEncoder::Outcome::Encoded)
            return false;

        // A frame belongs to the picture that went in with the same call, or it was held back.
        producedItsOwnFrame.push_back(frames.size() == before + 1 && frames.back().rtpTimestamp == timestamp);
        sent++;
        return true;
    }

    [[nodiscard]] double bitsPerSecond(size_t from, size_t to, int pictures) const
    {
        size_t bytes = 0;
        for (size_t i = from; i < to; i++)
            bytes += frames[i].data.size();
        return static_cast<double>(bytes) * 8.0 * FRAMERATE / pictures;
    }

    VideoEncoder encoder;
    std::vector<EncodedVideoFrame> frames;
    std::vector<bool> producedItsOwnFrame;
    int sent = 0;

private:
    AVFrame *picture;
};

// The graphics card's device, and what pictures on it are: Direct3D 11's on Windows, and
// elsewhere CUDA's for NVIDIA's encoders and VAAPI's for the others.
AVHWDeviceType cardDevice([[maybe_unused]] const std::string &encoderName = {})
{
#ifdef _WIN32
    return AV_HWDEVICE_TYPE_D3D11VA;
#else
    return encoderName.ends_with("_nvenc") ? AV_HWDEVICE_TYPE_CUDA : AV_HWDEVICE_TYPE_VAAPI;
#endif
}

AVPixelFormat cardFormat(AVHWDeviceType device)
{
    if (device == AV_HWDEVICE_TYPE_D3D11VA)
        return AV_PIX_FMT_D3D11;
    return device == AV_HWDEVICE_TYPE_CUDA ? AV_PIX_FMT_CUDA : AV_PIX_FMT_VAAPI;
}

// What an encoder's name ends with when its pictures reached it on such a device.
std::string onDevice(AVHWDeviceType device)
{
    return "(" + std::string(av_hwdevice_get_type_name(device)) + ")";
}

// Frames for pictures on the card, or null when it will not have them. `poolSize` of them are
// made beforehand, or each as it is asked for when that is none.
AVBufferRef *framesOnCard(AVBufferRef *device, AVPixelFormat format, const FrameSize &size, int poolSize = 0)
{
    AVBufferRef *frames = av_hwframe_ctx_alloc(device);
    auto *context = reinterpret_cast<AVHWFramesContext *>(frames->data);
    context->format = cardFormat(reinterpret_cast<AVHWDeviceContext *>(device->data)->type);
    context->sw_format = format;
    context->width = size.width;
    context->height = size.height;
    context->initial_pool_size = poolSize;
#ifdef _WIN32
    static_cast<AVD3D11VAFramesContext *>(context->hwctx)->BindFlags = D3D11_BIND_RENDER_TARGET;
#endif
    if (av_hwframe_ctx_init(frames) < 0)
        av_buffer_unref(&frames);
    return frames;
}

// Synthetic pictures put on the graphics card the way captured ones are found there.
class CardPictures
{
public:
    CardPictures(AVPixelFormat format, int width, int height, int colorspace, AVHWDeviceType type = cardDevice())
        : device(onDevice(type)), colorspace(colorspace), inMemory(allocatePicture(format, width, height)), onCard(av_frame_alloc())
    {
        AVBufferRef *made = nullptr;
        if (!Core::Media::hardwareDeviceCanBeAskedFor(type) || av_hwdevice_ctx_create(&made, type, nullptr, nullptr, 0) < 0)
            return;
        frames = framesOnCard(made, format, FrameSize{width, height});
        av_buffer_unref(&made);
    }

    ~CardPictures()
    {
        av_frame_free(&inMemory);
        av_frame_free(&onCard);
        av_buffer_unref(&frames);
    }

    [[nodiscard]] bool usable() const { return frames != nullptr; }

    AVFrame *next(int index)
    {
        paint(inMemory, index);
        av_frame_unref(onCard);
        if (av_hwframe_get_buffer(frames, onCard, 0) < 0 || av_hwframe_transfer_data(onCard, inMemory, 0) < 0)
            return nullptr;
        onCard->colorspace = static_cast<AVColorSpace>(colorspace);
        onCard->color_range = AVCOL_RANGE_MPEG;
        return onCard;
    }

    const std::string device;

private:
    int colorspace;
    AVBufferRef *frames = nullptr;
    AVFrame *inMemory;
    AVFrame *onCard;
};

bool startsWithStartCode(const std::vector<uint8_t> &data)
{
    if (data.size() >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1)
        return true;
    return data.size() >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1;
}

// The types of an Annex B frame's NAL units, as H264 or as H265 numbers them.
std::vector<int> nalTypes(VideoCodec codec, const std::vector<uint8_t> &data)
{
    std::vector<int> types;
    for (size_t i = 0; i + 3 < data.size(); i++) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            types.push_back(codec == VideoCodec::H265 ? (data[i + 3] >> 1) & 0x3F : data[i + 3] & 0x1F);
    }
    return types;
}

constexpr int H264_SPS = 7;
constexpr int H264_PPS = 8;
constexpr int H264_IDR = 5;
constexpr int H265_VPS = 32;
constexpr int H265_SPS = 33;
constexpr int H265_PPS = 34;
constexpr int H265_FIRST_IRAP = 16;
constexpr int H265_LAST_IRAP = 23;
constexpr int OBU_SEQUENCE_HEADER = 1;
constexpr int OBU_TEMPORAL_DELIMITER = 2;
constexpr int OBU_FRAME_HEADER = 3;
constexpr int OBU_FRAME = 6;

struct Obu
{
    int type = 0;
    bool saysItsSize = false;
    size_t payload = 0;
    size_t size = 0;
};

// The OBUs of an AV1 temporal unit, or none when it is not made of whole ones.
std::vector<Obu> obusOf(const std::vector<uint8_t> &data)
{
    std::vector<Obu> obus;
    for (size_t at = 0; at < data.size();) {
        Obu obu;
        obu.type = (data[at] >> 3) & 0x0F;
        obu.saysItsSize = data[at] & 0x02;
        obu.payload = at + ((data[at] & 0x04) ? 2 : 1);
        if ((data[at] & 0x80) || obu.payload > data.size())
            return {};
        obu.size = data.size() - obu.payload;
        if (obu.saysItsSize) {
            obu.size = 0;
            uint8_t byte = 0x80;
            for (int shift = 0; byte & 0x80; shift += 7) {
                if (obu.payload >= data.size() || shift > 28)
                    return {};
                byte = data[obu.payload++];
                obu.size |= size_t(byte & 0x7F) << shift;
            }
        }
        if (obu.size > data.size() - obu.payload)
            return {};
        obus.push_back(obu);
        at = obu.payload + obu.size;
    }
    return obus;
}

std::string unitTypes(VideoCodec codec, const EncodedVideoFrame &frame)
{
    std::string types;
    if (codec == VideoCodec::AV1) {
        for (const Obu &obu : obusOf(frame.data)) {
            if (!types.empty()) types += " ";
            types += std::to_string(obu.type);
        }
    } else if (codec != VideoCodec::VP8) {
        for (int type : nalTypes(codec, frame.data)) {
            if (!types.empty()) types += " ";
            types += std::to_string(type);
        }
    }
    return types;
}

// What a key frame says its profile is: the profile_idc of an H264 SPS, the general_profile_idc
// of an H265 one, the seq_profile of an AV1 sequence header. Or -1.
int profileOf(VideoCodec codec, const std::vector<uint8_t> &data)
{
    if (codec == VideoCodec::AV1) {
        for (const Obu &obu : obusOf(data)) {
            if (obu.type == OBU_SEQUENCE_HEADER && obu.size > 0)
                return data[obu.payload] >> 5;
        }
        return -1;
    }
    for (size_t i = 0; i + 6 < data.size(); i++) {
        if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1)
            continue;
        if (codec == VideoCodec::H264 && (data[i + 3] & 0x1F) == H264_SPS)
            return data[i + 4];
        if (codec == VideoCodec::H265 && ((data[i + 3] >> 1) & 0x3F) == H265_SPS)
            return data[i + 6] & 0x1F;
    }
    return -1;
}

// A key frame of H264 or H265 is Annex B and opens with its parameter sets, one of AV1 has its
// sequence header before its picture and every OBU says its size, and a VP8 frame says itself
// what it is. Whether a frame is a key frame is checked against the frame.
bool isWellFormed(VideoCodec codec, const EncodedVideoFrame &frame)
{
    if (frame.data.empty())
        return false;
    if (codec == VideoCodec::VP8)
        return !(frame.data[0] & 1) == frame.keyframe;

    if (codec == VideoCodec::AV1) {
        bool hasSequenceHeader = false;
        const Obu *picture = nullptr;
        const std::vector<Obu> obus = obusOf(frame.data);
        for (const Obu &obu : obus) {
            if (!obu.saysItsSize)
                return false;
            if (obu.type == OBU_SEQUENCE_HEADER && !picture)
                hasSequenceHeader = true;
            if ((obu.type == OBU_FRAME || obu.type == OBU_FRAME_HEADER) && !picture)
                picture = &obu;
        }
        if (!picture || picture->size == 0)
            return false;
        // show_existing_frame, the two bits of frame_type, where zero is a key frame, and show_frame.
        const uint8_t opening = frame.data[picture->payload];
        const bool keyFrame = !(opening & 0x80) && ((opening >> 5) & 0x03) == 0 && (opening & 0x10);
        return keyFrame == frame.keyframe && (!keyFrame || hasSequenceHeader);
    }

    const bool h265 = codec == VideoCodec::H265;
    const std::vector<int> types = nalTypes(codec, frame.data);
    const bool hasKeyPicture = std::any_of(types.begin(), types.end(), [h265](int type) { return h265 ? type >= H265_FIRST_IRAP && type <= H265_LAST_IRAP : type == H264_IDR; });
    if (!startsWithStartCode(frame.data) || hasKeyPicture != frame.keyframe)
        return false;
    if (!frame.keyframe)
        return true;
    if (h265)
        return types.size() >= 4 && types[0] == H265_VPS && types[1] == H265_SPS && types[2] == H265_PPS;
    return types.size() >= 3 && types[0] == H264_SPS && types[1] == H264_PPS;
}

struct Decoded
{
    int pictures = 0;
    int width = 0;
    int height = 0;
    uint64_t errors = 0;
    uint64_t dropped = 0;
    int format = AV_PIX_FMT_NONE;
    int colorspace = AVCOL_SPC_UNSPECIFIED;
    int range = AVCOL_RANGE_UNSPECIFIED;
    // Of a sample: other than square when the stream wants its pictures shown at another size
    // than they were coded at.
    AVRational shape = { 0, 1 };
    Yuv middle;
    Yuv top;
};

// Whether what was decoded is the picture that was sent. A graphics card that codes AV1 only
// in whole blocks, as a Radeon of the 7000 series does in ones of 64 by 16, sends the picture
// with what fills its last blocks: larger than it was, and nothing in the stream says by how
// much. See whatAnEncoderMakesOfASizeItCannotCode.
bool isThePictureSent(VideoCodec codec, const Decoded &decoded, const FrameSize &sent)
{
    if (FrameSize{decoded.width, decoded.height} == sent)
        return true;
    return codec == VideoCodec::AV1 && decoded.width >= sent.width && decoded.height >= sent.height && decoded.width - sent.width < 64 && decoded.height - sent.height < 16;
}

// Decodes the frames in order, in software, and shows each picture to `inspect` with the
// place among the frames of the one it was decoded from.
template <typename Frames>
Decoded decodeAll(VideoCodec codec, const Frames &frames, std::function<void(const AVFrame *, int)> inspect = {})
{
    auto sink = std::make_shared<CountingSink>();
    sink->inspect = std::move(inspect);
    Core::Media::VideoDecoder decoder(false);
    decoder.setSink(sink);

    int pushed = 0;
    const auto start = std::chrono::steady_clock::now();
    for (const auto &frame : frames) {
        decoder.push(codec, EncodedVideoFrame(*frame));
        pushed++;
        // The decoder throws its queue away when it falls far behind.
        while (pushed - sink->frames > 20 && std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() < 10000)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    while (sink->frames < pushed && std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() < 10000)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    Decoded decoded;
    decoded.pictures = sink->frames;
    decoded.width = sink->width;
    decoded.height = sink->height;
    decoded.errors = decoder.stats().errors;
    decoded.dropped = decoder.stats().dropped;
    decoded.format = sink->format;
    decoded.colorspace = sink->colorspace;
    decoded.range = sink->range;
    decoded.shape = { sink->shapeWidth, sink->shapeHeight };
    decoded.middle = { static_cast<double>(sink->middleY), static_cast<double>(sink->middleU), static_cast<double>(sink->middleV) };
    decoded.top = { static_cast<double>(sink->topY), static_cast<double>(sink->topU), static_cast<double>(sink->topV) };
    return decoded;
}

Decoded decodeAll(VideoCodec codec, const std::vector<EncodedVideoFrame> &frames, std::function<void(const AVFrame *, int)> inspect = {})
{
    std::vector<const EncodedVideoFrame *> pointers;
    for (const EncodedVideoFrame &frame : frames)
        pointers.push_back(&frame);
    return decodeAll(codec, pointers, std::move(inspect));
}

std::vector<uint8_t> joined(const std::vector<std::vector<uint8_t>> &parts)
{
    std::vector<uint8_t> whole;
    for (const std::vector<uint8_t> &part : parts)
        whole.insert(whole.end(), part.begin(), part.end());
    return whole;
}

std::vector<uint8_t> annexB(const std::vector<std::vector<uint8_t>> &nals)
{
    std::vector<uint8_t> frame;
    for (const std::vector<uint8_t> &nal : nals) {
        frame.insert(frame.end(), { 0, 0, 0, 1 });
        frame.insert(frame.end(), nal.begin(), nal.end());
    }
    return frame;
}

// An OBU that says its size, or one that leaves it out, as the last of a temporal unit may.
std::vector<uint8_t> obu(int type, const std::vector<uint8_t> &payload, bool saysItsSize = true)
{
    std::vector<uint8_t> whole = { static_cast<uint8_t>((type << 3) | (saysItsSize ? 0x02 : 0)) };
    for (size_t size = payload.size(); saysItsSize; size >>= 7) {
        whole.push_back(static_cast<uint8_t>((size & 0x7F) | (size > 0x7F ? 0x80 : 0)));
        if (size <= 0x7F)
            break;
    }
    whole.insert(whole.end(), payload.begin(), payload.end());
    return whole;
}

// The inside of an AV1 sequence header for pictures of a size, as far as the size: of a level
// that brings a tier with it or not, with a delay before pictures are shown or not, and with
// the times they are shown at or not.
std::vector<uint8_t> sequenceHeader(const FrameSize &size, int level, bool displayDelay = false, bool timingInfo = false)
{
    std::vector<uint8_t> header;
    int written = 0;
    const auto put = [&](uint32_t value, int count) {
        for (int bit = count - 1; bit >= 0; bit--, written++) {
            if (written % 8 == 0)
                header.push_back(0);
            header.back() |= static_cast<uint8_t>(((value >> bit) & 1) << (7 - written % 8));
        }
    };
    put(0, 5);
    put(timingInfo ? 1 : 0, 1);
    put(displayDelay ? 1 : 0, 1);
    put(0, 5 + 12);
    put(static_cast<uint32_t>(level), 5);
    if (level > 7)
        put(0, 1);
    if (displayDelay)
        put(0b10011, 5);
    put(15, 4);
    put(15, 4);
    put(static_cast<uint32_t>(size.width - 1), 16);
    put(static_cast<uint32_t>(size.height - 1), 16);
    return header;
}

struct Finished
{
    bool whole = false;
    bool keyframe = false;
    std::vector<uint8_t> data;
};

Finished finish(Core::Media::EncodedFrameFinisher &finisher, const std::vector<uint8_t> &data)
{
    EncodedVideoFrame frame;
    frame.data = data;
    Finished finished;
    finished.whole = finisher.finish(frame);
    finished.keyframe = frame.keyframe;
    finished.data = frame.data;
    return finished;
}

void remember(Core::Media::EncodedFrameFinisher &finisher, const std::vector<uint8_t> &extradata)
{
    finisher.rememberParameterSets(extradata.data(), extradata.size());
}

// Stamps one colour over the middle of a YUV picture.
void stampPatch(AVFrame *picture, const Yuv &colour)
{
    const bool interleaved = picture->format == AV_PIX_FMT_NV12;
    for (int y = picture->height * 3 / 8; y < picture->height * 5 / 8; y++) {
        for (int x = picture->width * 3 / 8; x < picture->width * 5 / 8; x++) {
            picture->data[0][y * picture->linesize[0] + x] = static_cast<uint8_t>(std::lround(colour.y));
            if (interleaved) {
                picture->data[1][(y / 2) * picture->linesize[1] + (x / 2) * 2] = static_cast<uint8_t>(std::lround(colour.u));
                picture->data[1][(y / 2) * picture->linesize[1] + (x / 2) * 2 + 1] = static_cast<uint8_t>(std::lround(colour.v));
            } else {
                picture->data[1][(y / 2) * picture->linesize[1] + x / 2] = static_cast<uint8_t>(std::lround(colour.u));
                picture->data[2][(y / 2) * picture->linesize[2] + x / 2] = static_cast<uint8_t>(std::lround(colour.v));
            }
        }
    }
}

static std::vector<std::string> gaveNoAnswer;

void warmUpEncoders()
{
    static bool done = false;
    if (done) return;
    done = true;
    for (VideoCodec codec : CODECS) {
        for (const std::string &name : encodersToTest(codec)) {
            const auto trialStart = std::chrono::steady_clock::now();
            const bool works = Core::Media::videoEncoderWorks(name);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - trialStart).count();
            if (!works && elapsed > 9000)
                gaveNoAnswer.push_back(name);
        }
    }
}

#ifdef _WIN32
void measureInDesktopPixels(bool desktopPixels)
{
    using SetAwareness = DPI_AWARENESS_CONTEXT(WINAPI *)(DPI_AWARENESS_CONTEXT);
    static const auto setAwareness = reinterpret_cast<SetAwareness>(reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext")));
    if (setAwareness)
        setAwareness(desktopPixels ? DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 : DPI_AWARENESS_CONTEXT_UNAWARE);
}

struct QuietScreen
{
    CaptureRect whole;
    CaptureRect workArea;

    [[nodiscard]] POINT cornerFor(const FrameSize &room) const
    {
        constexpr int MARGIN = 24;
        return POINT{std::max(workArea.x, workArea.x + workArea.width - room.width - MARGIN), std::max(workArea.y, workArea.y + workArea.height - room.height - MARGIN)};
    }
};

BOOL CALLBACK takeQuietScreen(HMONITOR monitor, HDC, LPRECT, LPARAM found)
{
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (!GetMonitorInfoW(monitor, &info) || (info.dwFlags & MONITORINFOF_PRIMARY) || !EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) || mode.dmDisplayOrientation != DMDO_DEFAULT)
        return TRUE;

    const auto rect = [](const RECT &native) { return CaptureRect{native.left, native.top, native.right - native.left, native.bottom - native.top}; };
    *reinterpret_cast<std::optional<QuietScreen> *>(found) = QuietScreen{ rect(info.rcMonitor), rect(info.rcWork) };
    return FALSE;
}

std::optional<QuietScreen> quietScreen()
{
    std::optional<QuietScreen> found;
    EnumDisplayMonitors(nullptr, nullptr, takeQuietScreen, reinterpret_cast<LPARAM>(&found));
    return found;
}

#define SKIP_WITHOUT_A_QUIET_SCREEN(screen) \
    const std::optional<QuietScreen> screen = quietScreen(); \
    if (!screen) \
        SKIP("the windows this opens go on a second screen, out of the way, and this machine has one")

class TestWindow
{
public:
    struct Look
    {
        FrameSize inside;
        POINT at;
        COLORREF colour = SHARED_COLOUR;
        bool framed = true;
        bool scaledByWindows = false;
    };

    explicit TestWindow(const Look &look) : look(look), thread([this] { run(); })
    {
        while (!window && !failedToOpen)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (!window)
            return;
        if (look.framed)
            resizeInside(look.inside);
        moveTo(look.at);
        ShowWindow(window, SW_SHOWNOACTIVATE);
    }

    ~TestWindow()
    {
        if (isOpen())
            PostMessageW(window, WM_CLOSE, 0, 0);
        if (thread.joinable())
            thread.join();
    }

    [[nodiscard]] HWND handle() const { return window; }
    [[nodiscard]] bool isOpen() const { return window && IsWindow(window); }
    [[nodiscard]] std::string sourceId() const { return "window:" + std::to_string(reinterpret_cast<uintptr_t>(window.load())); }

    [[nodiscard]] CaptureRect insideOnScreen() const
    {
        RECT inside = {};
        POINT corner = {};
        GetClientRect(window, &inside);
        ClientToScreen(window, &corner);
        return CaptureRect{corner.x, corner.y, inside.right, inside.bottom};
    }

    void setAnimated(bool on) { animated = on; }

    void resizeInside(const FrameSize &size)
    {
        RECT whole = {};
        GetWindowRect(window, &whole);
        const CaptureRect current = insideOnScreen();
        const int grownW = (whole.right - whole.left) + size.width - current.width;
        const int grownH = (whole.bottom - whole.top) + size.height - current.height;
        SetWindowPos(window, nullptr, 0, 0, grownW, grownH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void moveTo(const POINT &at) { SetWindowPos(window, nullptr, at.x, at.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE); }
    void show() { ShowWindowAsync(window, SW_SHOWNOACTIVATE); }
    void hide() { ShowWindowAsync(window, SW_HIDE); }
    void close() { PostMessageW(window, WM_CLOSE, 0, 0); }
    bool keepOutOfCaptures(bool keep) { return SetWindowDisplayAffinity(window, keep ? WDA_MONITOR : WDA_NONE) != FALSE; }
    void setCloaked(bool cloaked)
    {
        constexpr DWORD CLOAK_ATTRIBUTE = 13;
        using SetAttribute = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
        static const auto setAttribute = reinterpret_cast<SetAttribute>(reinterpret_cast<void *>(GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmSetWindowAttribute")));
        const BOOL value = cloaked;
        if (setAttribute)
            setAttribute(window, CLOAK_ATTRIBUTE, &value, sizeof(value));
    }
    bool reachPastAnOuterEdge(const CaptureRect &screen, int dist)
    {
        const CaptureRect current = insideOnScreen();
        return SetWindowPos(window, nullptr, screen.right() - dist, current.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
    }

private:
    static void fill(HDC context, const RECT &area, COLORREF colour)
    {
        const HBRUSH brush = CreateSolidBrush(colour);
        FillRect(context, &area, brush);
        DeleteObject(brush);
    }

    static LRESULT CALLBACK dispatch(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        constexpr RECT CORNER = { 0, 0, 24, 24 };
        if (message == WM_NCCREATE)
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<const CREATESTRUCTW *>(lParam)->lpCreateParams));
        auto *self = reinterpret_cast<TestWindow *>(GetWindowLongPtrW(window, GWLP_USERDATA));

        switch (message) {
        case WM_TIMER:
            if (self && self->animated) {
                self->tick++;
                InvalidateRect(window, &CORNER, FALSE);
                UpdateWindow(window);
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT painting;
            const HDC context = BeginPaint(window, &painting);
            RECT inside = {};
            GetClientRect(window, &inside);
            if (self) {
                fill(context, inside, self->look.colour);
                fill(context, CORNER, RGB(self->tick * 37 & 255, self->tick * 91 & 255, self->tick * 13 & 255));
            }
            EndPaint(window, &painting);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void run()
    {
        static const wchar_t *const CLASS_NAME = L"StyxCaptureTestWindow";
        measureInDesktopPixels(!look.scaledByWindows);

        WNDCLASSW description = {};
        description.style = CS_HREDRAW | CS_VREDRAW;
        description.lpfnWndProc = dispatch;
        description.hInstance = GetModuleHandleW(nullptr);
        description.lpszClassName = CLASS_NAME;
        RegisterClassW(&description);

        const HWND owner = CreateWindowExW(0, CLASS_NAME, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, description.hInstance, nullptr);
        const HWND made = CreateWindowExW(WS_EX_TOPMOST, CLASS_NAME, L"Styx capture test", look.framed ? WS_OVERLAPPEDWINDOW : WS_POPUP, look.at.x, look.at.y, look.inside.width, look.inside.height, owner, nullptr, description.hInstance, this);
        if (!made) {
            failedToOpen = true;
            return;
        }
        SetTimer(made, 1, 16, nullptr);
        window = made;

        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    const Look look;
    std::atomic<HWND> window{ nullptr };
    std::atomic<bool> failedToOpen{ false };
    std::atomic<bool> animated{ true };
    int tick = 0;
    std::thread thread;
};
#endif

FrameSize scaledKeepAspectRatio(FrameSize size, FrameSize bound)
{
    if (size.width <= 0 || size.height <= 0 || bound.width <= 0 || bound.height <= 0) return {};
    const auto wAtBoundH = std::int64_t(bound.height) * size.width / size.height;
    if (wAtBoundH <= bound.width) {
        return {int(wAtBoundH), bound.height};
    } else {
        return {bound.width, int(std::int64_t(bound.width) * size.height / size.width)};
    }
}
struct Rgb { int red; int green; int blue; };
constexpr std::array QUARTER_COLOURS = { Rgb{ 200, 40, 40 }, Rgb{ 40, 200, 60 }, Rgb{ 40, 60, 220 }, Rgb{ 128, 128, 128 } };
constexpr int BLACK_AT_MOST = 40;
constexpr double COLOUR_SURVIVES = 8;

inline Yuv inYuv(const Rgb &c, bool bt709) { return inYuv(c.red, c.green, c.blue, bt709); }
inline Yuv inYuv(COLORREF c, bool bt709) { return inYuv(GetRValue(c), GetGValue(c), GetBValue(c), bt709); }

inline AVFrame *quartersOnCard(AVBufferRef *device, FrameSize size)
{
    AVFrame *inMemory = allocatePicture(AV_PIX_FMT_BGRA, size.width, size.height);
    for (int y = 0; y < size.height; y++) {
        uint8_t *row = inMemory->data[0] + y * inMemory->linesize[0];
        const bool lower = y >= size.height / 2;
        for (int x = 0; x < size.width; x++) {
            const Rgb colour = QUARTER_COLOURS[(lower ? 2 : 0) + (x >= size.width / 2 ? 1 : 0)];
            row[4 * x] = static_cast<uint8_t>(colour.blue);
            row[4 * x + 1] = static_cast<uint8_t>(colour.green);
            row[4 * x + 2] = static_cast<uint8_t>(colour.red);
            row[4 * x + 3] = lower ? 0 : 255;
        }
    }

    AVBufferRef *frames = framesOnCard(device, AV_PIX_FMT_BGRA, size);
    AVFrame *onCard = av_frame_alloc();
    if (!frames || av_hwframe_get_buffer(frames, onCard, 0) < 0 || av_hwframe_transfer_data(onCard, inMemory, 0) < 0) {
        av_frame_free(&onCard);
    }
    if (onCard)
        onCard->color_range = AVCOL_RANGE_MPEG;
    av_buffer_unref(&frames);
    av_frame_free(&inMemory);
    return onCard;
}

inline AVPixelFormat formatOnCard(const AVFrame *card)
{
    return card->hw_frames_ctx ? reinterpret_cast<const AVHWFramesContext *>(card->hw_frames_ctx->data)->sw_format : AV_PIX_FMT_NONE;
}

inline Yuv colourAt(const AVFrame *nv12, int x, int y)
{
    const uint8_t *chroma = nv12->data[1] + (y / 2) * nv12->linesize[1] + (x / 2) * 2;
    return { static_cast<double>(nv12->data[0][y * nv12->linesize[0] + x]), static_cast<double>(chroma[0]), static_cast<double>(chroma[1]) };
}

inline bool looksLike(const Yuv &seen, COLORREF colour)
{
    constexpr double CLOSE = 16;
    return distance(seen, inYuv(colour, true)) < CLOSE || distance(seen, inYuv(colour, false)) < CLOSE;
}

inline double offItsOwnCoefficients(const Decoded &decoded, const Yuv &seen, COLORREF colour)
{
    const bool named = decoded.colorspace == AVCOL_SPC_BT709 || decoded.colorspace == AVCOL_SPC_SMPTE170M || decoded.colorspace == AVCOL_SPC_BT470BG;
    return distance(seen, inYuv(colour, named ? decoded.colorspace == AVCOL_SPC_BT709 : decoded.height >= 720));
}

template <typename Pred>
bool tryUntil(Pred &&pred, int timeoutMs = 3000)
{
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() < timeoutMs) {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

struct PipelineRecord
{
    explicit PipelineRecord(Core::Video::BroadcastPipeline &pipeline)
    {
        clockStart = std::chrono::steady_clock::now();
        connections.push_back(pipeline.started.connect([this](const FrameSize &size, int) {
            std::lock_guard lock(mutex);
            starts.push_back(size);
        }));
        connections.push_back(pipeline.frameEncoded.connect([this](const Core::Video::EncodedFramePointer &frame) {
            std::lock_guard lock(mutex);
            frames.push_back(frame);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - clockStart).count();
            arrivedAt.push_back(elapsed);
        }));
        connections.push_back(pipeline.previewReady.connect([this](const Platform::ImageBuffer &preview) {
            std::lock_guard lock(mutex);
            previews.push_back(preview);
        }));
        connections.push_back(pipeline.statsUpdated.connect([this](const Core::Video::BroadcastStats &update) {
            std::lock_guard lock(mutex);
            stats.push_back(update);
        }));
        connections.push_back(pipeline.captureStalled.connect([this](bool stalled) {
            std::lock_guard lock(mutex);
            stalls.push_back(stalled);
        }));
        connections.push_back(pipeline.sourceClosed.connect([this] {
            std::lock_guard lock(mutex);
            closes++;
        }));
        connections.push_back(pipeline.failed.connect([this](const std::string &reason) {
            std::lock_guard lock(mutex);
            failures.push_back(reason);
        }));
    }

    [[nodiscard]] std::vector<Core::Video::EncodedFramePointer> framesBefore(size_t end) const
    {
        std::lock_guard lock(mutex);
        return std::vector<Core::Video::EncodedFramePointer>(frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(end));
    }

    [[nodiscard]] size_t keyFrameFrom(size_t from) const
    {
        std::lock_guard lock(mutex);
        size_t index = from;
        while (index < frames.size() && !frames[index]->keyframe)
            index++;
        return index;
    }

    std::chrono::steady_clock::time_point clockStart;
    [[nodiscard]] int64_t elapsedMs() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - clockStart).count();
    }

    mutable std::mutex mutex;
    std::vector<Platform::Connection> connections;
    std::vector<Core::Video::EncodedFramePointer> frames;
    std::vector<int64_t> arrivedAt;
    std::vector<Platform::ImageBuffer> previews;
    std::vector<FrameSize> starts;
    std::vector<Core::Video::BroadcastStats> stats;
    std::vector<bool> stalls;
    std::vector<std::string> failures;
    int closes = 0;
};

struct PathCounts
{
    int64_t cutOut = -1;
    int64_t drawn = -1;
};

inline PathCounts pathCounts(const Core::Video::BroadcastStats &stats)
{
    size_t dda = stats.encoderName.find("ddagrab ");
    size_t pw = stats.encoderName.find(" + PrintWindow ");
    size_t fr = stats.encoderName.find(" frames");
    if (dda != std::string::npos && pw != std::string::npos && fr != std::string::npos && dda < pw && pw < fr) {
        try {
            int64_t cutOut = std::stoll(stats.encoderName.substr(dda + 8, pw - (dda + 8)));
            int64_t drawn = std::stoll(stats.encoderName.substr(pw + 15, fr - (pw + 15)));
            return { cutOut, drawn };
        } catch (...) {}
    }
    return {};
}

inline std::vector<VideoCodec> codecsSharedAt(const FrameSize &size)
{
    return Core::Media::shareCodecs(Core::Media::encodableVideoCodecs(), Core::Media::videoEncoderFor(VideoCodec::AV1), size);
}

inline bool reachesTheEncoderOnTheCard(VideoCodec codec, bool scaled, bool byGraphicsCapture)
{
    const VideoEncoder chosen(settingsFor({ codec, "" }, BITRATE));
    const bool withoutTheProcessor = chosen.takesRgbCardFrames() && (!scaled || byGraphicsCapture);
    return chosen.takesCardFrames() && (withoutTheProcessor || Core::Video::CardScaler::usable());
}

inline bool aScreenIsOnAGraphicsCard()
{
#ifdef _WIN32
    using CreateFactory = HRESULT(WINAPI *)(REFIID, void **);
    static const auto createFactory = reinterpret_cast<CreateFactory>(reinterpret_cast<void *>(GetProcAddress(LoadLibraryW(L"dxgi.dll"), "CreateDXGIFactory1")));
    IDXGIFactory1 *factory = nullptr;
    if (!createFactory || FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
        return false;

    bool found = false;
    IDXGIAdapter *adapter = nullptr;
    for (UINT index = 0; !found && SUCCEEDED(factory->EnumAdapters(index, &adapter)); index++) {
        IDXGIOutput *output = nullptr;
        if (SUCCEEDED(adapter->EnumOutputs(0, &output))) {
            output->Release();
            AVBufferRef *device = nullptr;
            const std::string name = std::to_string(index);
            found = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, name.c_str(), nullptr, 0) >= 0;
            av_buffer_unref(&device);
        }
        adapter->Release();
    }
    factory->Release();
    return found;
#else
    return false;
#endif
}
} // namespace

TEST_CASE("The probe gives the same answer every time", "[video][encode]")
{
    const std::vector<VideoCodec> first = Core::Media::encodableVideoCodecs();
    for (int i = 0; i < 3; i++)
        REQUIRE(Core::Media::encodableVideoCodecs() == first);

    // Best first, as the voice server has them: AV1, H265, H264, VP8.
    std::vector<VideoCodec> expected;
    for (VideoCodec codec : CODECS) {
        if (std::find(first.begin(), first.end(), codec) != first.end())
            expected.push_back(codec);
    }
    REQUIRE(first == expected);

    // A codec is encodable exactly when one of its encoders works, and then it can be encoded.
    for (VideoCodec codec : CODECS) {
        const std::vector<std::string> names = Core::Media::videoEncoderNames(codec);
        const bool oneWorks = std::any_of(names.begin(), names.end(), [](const std::string &name) { return Core::Media::videoEncoderWorks(name); });
        REQUIRE((std::find(first.begin(), first.end(), codec) != first.end()) == oneWorks);

        EncodeRun run({ codec, {} }, WIDTH, HEIGHT, BITRATE);
        REQUIRE(run.encoder.hasEncoder() == oneWorks);
        REQUIRE(run.encodeNext() == oneWorks);

        // The encoder its pictures go to is the first of them that works.
        const std::string chosen = Core::Media::videoEncoderFor(codec);
        const auto firstThatWorks = std::find_if(names.begin(), names.end(), [](const std::string &name) { return Core::Media::videoEncoderWorks(name); });
        REQUIRE(chosen == (firstThatWorks == names.end() ? std::string() : *firstThatWorks));
        if (oneWorks)
            REQUIRE(run.encoder.stats().encoderName.starts_with(chosen));
    }
    EncodeRun unsupported({ VideoCodec::Unknown, {} }, WIDTH, HEIGHT, BITRATE);
    REQUIRE(!unsupported.encodeNext());

    // AV1 and H265 are the graphics card's or nobody's.
    for (VideoCodec codec : { VideoCodec::AV1, VideoCodec::H265 }) {
        for (const std::string &name : Core::Media::videoEncoderNames(codec)) {
            if (!Core::Media::videoEncoderWorks(name))
                continue;
            EncodeRun run({ codec, name }, WIDTH, HEIGHT, BITRATE);
            REQUIRE(run.encodeNext());
            REQUIRE(run.encoder.stats().hardware);
        }
    }
}

TEST_CASE("Encodable codecs are offered for sending", "[video][encode]")
{
    const std::vector<VideoCodec> decodable = Core::Media::decodableVideoCodecs();
    const std::vector<VideoCodec> encodable = Core::Media::encodableVideoCodecs();
    const std::vector<Core::Media::CodecOffer> offered = Core::Media::offeredVideoCodecs(encodable);
    REQUIRE(offered.size() == decodable.size());
    for (size_t i = 0; i < offered.size(); i++) {
        REQUIRE(offered[i].name == videoCodecName(decodable[i]));
        REQUIRE(offered[i].decode == true);
        REQUIRE(offered[i].encode == (std::find(encodable.begin(), encodable.end(), decodable[i]) != encodable.end()));
        REQUIRE(offered[i].payloadType == 101 + 2 * static_cast<int>(i));
    }
    for (const auto &codec : Core::Media::offeredVideoCodecs())
        REQUIRE(codec.encode == false);
}

TEST_CASE("A codec turned off is not sent with", "[video][encode]")
{
    using namespace Core::Media;
    const auto before = Platform::Settings::instance().getJson("video/send_codecs_off", nlohmann::json::array());
    struct RestoreGuard {
        nlohmann::json saved;
        ~RestoreGuard() { Platform::Settings::instance().setJson("video/send_codecs_off", saved); }
    } restore{before};
    Platform::Settings::instance().setJson("video/send_codecs_off", nlohmann::json::array());

    const std::vector<VideoCodec> encodable = { VideoCodec::AV1, VideoCodec::H264, VideoCodec::VP8 };
    REQUIRE(allowedSendCodecs(encodable) == encodable);

    setSendCodecAllowed(VideoCodec::AV1, false);
    setSendCodecAllowed(VideoCodec::H265, false);
    REQUIRE(!sendCodecAllowed(VideoCodec::AV1));
    REQUIRE(sendCodecAllowed(VideoCodec::H264));
    REQUIRE(allowedSendCodecs(encodable) == std::vector<VideoCodec>({ VideoCodec::H264, VideoCodec::VP8 }));

    // Turned off twice it is off once, and on again it is as it was.
    setSendCodecAllowed(VideoCodec::AV1, false);
    setSendCodecAllowed(VideoCodec::AV1, true);
    REQUIRE(sendCodecAllowed(VideoCodec::AV1));
    REQUIRE(allowedSendCodecs(encodable) == encodable);
    REQUIRE(!sendCodecAllowed(VideoCodec::H265));
}

TEST_CASE("AV1 from AMF is coded in whole blocks", "[video][encode]")
{
    using Core::Media::codedSize;
    const std::string amf = "av1_amf";
    REQUIRE(codedSize(amf, FrameSize{1920, 1080}) == FrameSize{1920, 1082});
    REQUIRE(codedSize(amf, FrameSize{640, 360}) == FrameSize{640, 362});
    REQUIRE(codedSize(amf, FrameSize{960, 540}) == FrameSize{960, 544});
    REQUIRE(codedSize(amf, FrameSize{1000, 700}) == FrameSize{1024, 704});
    REQUIRE(codedSize(amf, FrameSize{1280, 720}) == FrameSize{1280, 720});
    REQUIRE(codedSize(amf, FrameSize{2560, 1440}) == FrameSize{2560, 1440});
    REQUIRE(codedSize(amf, FrameSize{1452, 752}) == FrameSize{1472, 752});

    // Every other encoder codes what it is given, but for VAAPI's, which is asked on each machine.
    std::vector<std::string> others = { std::string(), "av1_nvenc", "av1_qsv", "av1_mf hardware" };
    for (VideoCodec codec : CODECS) {
        for (const std::string &name : Core::Media::videoEncoderNames(codec))
            others.push_back(name);
    }
    std::erase(others, amf);
    std::erase(others, std::string("av1_vaapi"));
    REQUIRE((std::find(others.begin(), others.end(), "libvpx") != others.end()) == (avcodec_find_encoder_by_name("libvpx") != nullptr));
    for (const std::string &name : others) {
        INFO(name);
        for (const FrameSize &size : { FrameSize{1920, 1080}, FrameSize{640, 360}, FrameSize{1000, 700}, FrameSize{1452, 752} })
            REQUIRE(codedSize(name, size) == size);
    }
}

TEST_CASE("Black columns are not acceptable and a few rows are", "[video][encode]")
{
    using Core::Media::paddingIsAcceptable;
    REQUIRE(paddingIsAcceptable(FrameSize{1280, 720}, FrameSize{1280, 720}));
    REQUIRE(paddingIsAcceptable(FrameSize{1280, 720}, FrameSize{1280, 728}));
    REQUIRE(!paddingIsAcceptable(FrameSize{1280, 720}, FrameSize{1280, 730}));
    REQUIRE(!paddingIsAcceptable(FrameSize{1280, 720}, FrameSize{1282, 720}));
    REQUIRE(!paddingIsAcceptable(FrameSize{1280, 720}, FrameSize{1344, 722}));

    const auto accepted = [](const std::string &encoder, const FrameSize &size) { return paddingIsAcceptable(size, Core::Media::codedSize(encoder, size)); };
    const std::string amf = "av1_amf";
    REQUIRE(accepted(amf, FrameSize{1920, 1080}));
    REQUIRE(accepted(amf, FrameSize{640, 360}));
    REQUIRE(accepted(amf, FrameSize{960, 540}));
    REQUIRE(accepted(amf, FrameSize{1280, 720}));
    REQUIRE(accepted(amf, FrameSize{2560, 1440}));
    REQUIRE(!accepted(amf, FrameSize{1000, 700}));
    REQUIRE(!accepted(amf, FrameSize{1452, 752}));
    // The right width, and fourteen rows short of a whole block.
    REQUIRE(!accepted(amf, FrameSize{1280, 722}));

    for (const std::string &exact : { "av1_nvenc", "av1_qsv", "hevc_amf", "libvpx" }) {
        INFO(exact);
        for (const FrameSize &size : { FrameSize{1920, 1080}, FrameSize{1000, 700}, FrameSize{1452, 752}, FrameSize{1280, 722} })
            REQUIRE(accepted(exact, size));
    }
}

TEST_CASE("AV1 is offered only where it adds no black", "[video][encode]")
{
    using Core::Media::cameraCodecs;
    using Core::Media::shareCodecs;
    const std::string amf = "av1_amf";
    const std::string nvenc = "av1_nvenc";
    const std::vector<VideoCodec> all = { VideoCodec::AV1, VideoCodec::H265, VideoCodec::H264, VideoCodec::VP8 };
    const std::vector<VideoCodec> withoutAv1 = { VideoCodec::H265, VideoCodec::H264, VideoCodec::VP8 };

    REQUIRE(shareCodecs(all, amf, FrameSize{1920, 1080}) == all);
    REQUIRE(shareCodecs(all, amf, FrameSize{1280, 720}) == all);
    REQUIRE(shareCodecs(all, amf, FrameSize{1452, 752}) == withoutAv1);
    REQUIRE(shareCodecs(all, amf, FrameSize{1000, 700}) == withoutAv1);
    REQUIRE(shareCodecs(all, nvenc, FrameSize{1452, 752}) == all);
    REQUIRE(shareCodecs(withoutAv1, amf, FrameSize{1452, 752}) == withoutAv1);
    // Asked of what a share already offers, a codec that was left out once stays out.
    REQUIRE(shareCodecs(shareCodecs(all, amf, FrameSize{1452, 752}), amf, FrameSize{1920, 1080}) == withoutAv1);

    REQUIRE(cameraCodecs(all, amf) == withoutAv1);
    REQUIRE(cameraCodecs(all, nvenc) == all);
    REQUIRE(cameraCodecs(all, "av1_qsv") == all);
    REQUIRE(cameraCodecs(withoutAv1, amf) == withoutAv1);
    REQUIRE(cameraCodecs({ VideoCodec::VP8 }, amf) == std::vector<VideoCodec>{ VideoCodec::VP8 });
}

TEST_CASE("AV1 from VAAPI is asked what sizes it codes", "[video][encode]")
{
    using Core::Media::av1CodedSize;
    using Core::Media::codesEverySizeAsItIs;
    using Core::Media::Av1SizeProbe;
    const std::vector<uint8_t> delimiter = obu(OBU_TEMPORAL_DELIMITER, {});
    const std::vector<uint8_t> picture = obu(OBU_FRAME, { 0x10, 0x00, 0x55 });

    REQUIRE(av1CodedSize(joined({ obu(OBU_SEQUENCE_HEADER, sequenceHeader(FrameSize{640, 360}, 5)), picture })) == FrameSize{640, 360});
    REQUIRE(av1CodedSize(joined({ delimiter, obu(OBU_SEQUENCE_HEADER, sequenceHeader(FrameSize{1920, 1082}, 13)), picture })) == FrameSize{1920, 1082});
    REQUIRE(av1CodedSize(joined({ delimiter, obu(OBU_SEQUENCE_HEADER, sequenceHeader(FrameSize{364, 196}, 0, true)), picture })) == FrameSize{364, 196});
    REQUIRE(av1CodedSize(obu(OBU_SEQUENCE_HEADER, sequenceHeader(FrameSize{8192, 4352}, 19, true), false)) == FrameSize{8192, 4352});

    // No sequence header, one that stops short, one that goes on to say when pictures are shown,
    // and what is no OBU at all.
    std::vector<uint8_t> cutShort = sequenceHeader(FrameSize{640, 360}, 5);
    cutShort.resize(5);
    REQUIRE(av1CodedSize(joined({ delimiter, picture })) == FrameSize{});
    REQUIRE(av1CodedSize(joined({ delimiter, obu(OBU_SEQUENCE_HEADER, cutShort), picture })) == FrameSize{});
    REQUIRE(av1CodedSize(obu(OBU_SEQUENCE_HEADER, sequenceHeader(FrameSize{640, 360}, 5, false, true))) == FrameSize{});
    REQUIRE(av1CodedSize({ 0xB2, 0x01, 0x10 }) == FrameSize{});
    REQUIRE(av1CodedSize({}) == FrameSize{});

    // The trial's own size tells whole blocks of 64 by 16 from the rest, and its other one
    // blocks of 8 by 2 as well. Only an encoder that changes neither codes every size as it is.
    const FrameSize trial{640, 360};
    const FrameSize odd{364, 196};
    REQUIRE(codesEverySizeAsItIs({ Av1SizeProbe{ trial, trial }, Av1SizeProbe{ odd, odd } }));
    REQUIRE(!codesEverySizeAsItIs({ Av1SizeProbe{ trial, FrameSize{640, 362} }, Av1SizeProbe{ odd, FrameSize{384, 208} } }));
    REQUIRE(!codesEverySizeAsItIs({ Av1SizeProbe{ trial, trial }, Av1SizeProbe{ odd, FrameSize{368, 196} } }));
    REQUIRE(!codesEverySizeAsItIs({ Av1SizeProbe{ trial, FrameSize{} }, Av1SizeProbe{ odd, FrameSize{} } }));
    REQUIRE(!codesEverySizeAsItIs({ Av1SizeProbe{ trial, trial }, Av1SizeProbe{ odd, FrameSize{} } }));
    REQUIRE(!codesEverySizeAsItIs({}));
}

TEST_CASE("A frame is made whole whatever its encoder left out", "[video][encode]")
{
    using Core::Media::EncodedFrameFinisher;
    {
        const std::vector<uint8_t> sps = { 0x67, 0x64, 0x1F }, pps = { 0x68, 0xEE }, laterSps = { 0x67, 0x4D, 0x28 }, laterPps = { 0x68, 0xCE };
        const std::vector<uint8_t> delimiter = { 0x09, 0x10 }, idr = { 0x65, 0x88, 0x80 }, slice = { 0x41, 0x9A, 0x20 };
        EncodedFrameFinisher finisher(VideoCodec::H264);
        // A key frame whose parameter sets were never seen cannot be sent.
        REQUIRE(!finish(finisher, annexB({ idr })).whole);

        remember(finisher, annexB({ sps, pps }));
        Finished finished = finish(finisher, annexB({ delimiter, idr }));
        REQUIRE((finished.whole && finished.keyframe));
        REQUIRE(finished.data == annexB({ sps, pps, idr }));

        finished = finish(finisher, annexB({ delimiter, slice }));
        REQUIRE((finished.whole && !finished.keyframe));
        REQUIRE(finished.data == annexB({ delimiter, slice }));

        // A key frame that brings them is left as it is, and they are what the next one gets.
        finished = finish(finisher, annexB({ laterSps, laterPps, idr }));
        REQUIRE((finished.whole && finished.keyframe));
        REQUIRE(finished.data == annexB({ laterSps, laterPps, idr }));
        REQUIRE(finish(finisher, annexB({ idr })).data == annexB({ laterSps, laterPps, idr }));

        // With lengths in place of start codes, as in an MP4 file.
        REQUIRE(!finish(finisher, { 0, 0, 0, 3, 0x65, 0x88, 0x80 }).whole);
        REQUIRE(!finish(finisher, {}).whole);
    }
    {
        const std::vector<uint8_t> vps = { 0x40, 0x01, 0x0C }, sps = { 0x42, 0x01, 0x01 }, pps = { 0x44, 0x01, 0xC1 }, laterSps = { 0x42, 0x01, 0x02 }, laterPps = { 0x44, 0x01, 0xC2 };
        const std::vector<uint8_t> delimiter = { 0x46, 0x01, 0x50 }, sei = { 0x4E, 0x01, 0x05 }, idr = { 0x26, 0x01, 0xAF }, cleanRandomAccess = { 0x2A, 0x01, 0xAF }, trailing = { 0x02, 0x01, 0xD0 };
        EncodedFrameFinisher finisher(VideoCodec::H265);
        remember(finisher, annexB({ sps, pps }));
        REQUIRE(!finish(finisher, annexB({ idr })).whole);

        remember(finisher, annexB({ vps, sps, pps }));
        Finished finished = finish(finisher, annexB({ delimiter, sei, idr }));
        REQUIRE((finished.whole && finished.keyframe));
        REQUIRE(finished.data == annexB({ vps, sps, pps, sei, idr }));

        // Every picture a decoder can start at is a key frame, not only an IDR.
        finished = finish(finisher, annexB({ cleanRandomAccess }));
        REQUIRE((finished.whole && finished.keyframe));
        REQUIRE(finished.data == annexB({ vps, sps, pps, cleanRandomAccess }));

        finished = finish(finisher, annexB({ delimiter, trailing }));
        REQUIRE((finished.whole && !finished.keyframe));
        REQUIRE(finished.data == annexB({ delimiter, trailing }));

        REQUIRE(finish(finisher, annexB({ vps, sps, pps, idr })).data == annexB({ vps, sps, pps, idr }));
        // One that brings some of them gets the rest, in the order they belong in.
        REQUIRE(finish(finisher, annexB({ laterSps, laterPps, idr })).data == annexB({ vps, laterSps, laterPps, idr }));
    }
    {
        constexpr int TEMPORAL_DELIMITER = OBU_TEMPORAL_DELIMITER;
        constexpr int TILE_GROUP = 4;
        const std::vector<uint8_t> sequence = { 0x00, 0x00, 0x00, 0x42, 0xAB }, laterSequence = { 0x00, 0x00, 0x00, 0x6A, 0xAB };
        // What a frame header opens with: a key frame that is shown, a frame predicted from
        // others, and an earlier frame shown again.
        const std::vector<uint8_t> key = { 0x10, 0x00, 0x55 }, predicted = { 0x30, 0x11 }, shownAgain = { 0x90 };
        const std::vector<uint8_t> delimiter = obu(TEMPORAL_DELIMITER, {});
        EncodedFrameFinisher finisher(VideoCodec::AV1);
        REQUIRE(!finish(finisher, joined({ delimiter, obu(OBU_FRAME, key) })).whole);

        remember(finisher, obu(OBU_SEQUENCE_HEADER, sequence));
        Finished finished = finish(finisher, joined({ delimiter, obu(OBU_FRAME, key) }));
        REQUIRE((finished.whole && finished.keyframe));
        REQUIRE(finished.data == joined({ delimiter, obu(OBU_SEQUENCE_HEADER, sequence), obu(OBU_FRAME, key) }));

        finished = finish(finisher, joined({ delimiter, obu(OBU_FRAME, predicted) }));
        REQUIRE((finished.whole && !finished.keyframe));
        REQUIRE(finished.data == joined({ delimiter, obu(OBU_FRAME, predicted) }));
        finished = finish(finisher, joined({ delimiter, obu(OBU_FRAME_HEADER, shownAgain) }));
        REQUIRE((finished.whole && !finished.keyframe));

        // A frame may come as its header and its tiles apart.
        finished = finish(finisher, joined({ obu(OBU_FRAME_HEADER, key), obu(TILE_GROUP, { 0x77, 0x78 }) }));
        REQUIRE((finished.whole && finished.keyframe));
        REQUIRE(finished.data == joined({ obu(OBU_SEQUENCE_HEADER, sequence), obu(OBU_FRAME_HEADER, key), obu(TILE_GROUP, { 0x77, 0x78 }) }));

        finished = finish(finisher, joined({ delimiter, obu(OBU_SEQUENCE_HEADER, laterSequence), obu(OBU_FRAME, key) }));
        REQUIRE((finished.whole && finished.keyframe));
        REQUIRE(finished.data == joined({ delimiter, obu(OBU_SEQUENCE_HEADER, laterSequence), obu(OBU_FRAME, key) }));
        REQUIRE(finish(finisher, obu(OBU_FRAME, key)).data == joined({ obu(OBU_SEQUENCE_HEADER, laterSequence), obu(OBU_FRAME, key) }));

        // The last OBU may leave its size out, and is given it: long enough here that the size
        // takes two bytes.
        std::vector<uint8_t> longKey(300, 0x5A);
        longKey[0] = key[0];
        finished = finish(finisher, joined({ delimiter, obu(OBU_FRAME, longKey, false) }));
        REQUIRE((finished.whole && finished.keyframe));
        REQUIRE(finished.data == joined({ delimiter, obu(OBU_SEQUENCE_HEADER, laterSequence), obu(OBU_FRAME, longKey) }));
        finished = finish(finisher, joined({ delimiter, obu(OBU_FRAME, predicted, false) }));
        REQUIRE((finished.whole && !finished.keyframe));
        REQUIRE(finished.data == joined({ delimiter, obu(OBU_FRAME, predicted) }));

        // Not OBUs: the bit that is always clear is set, a size runs past the end, no picture.
        REQUIRE(!finish(finisher, { 0xB2, 0x01, 0x10 }).whole);
        REQUIRE(!finish(finisher, { 0x32, 0x09, 0x10 }).whole);
        REQUIRE(!finish(finisher, delimiter).whole);

        // Extradata may be the sequence header as an MP4 file keeps it, behind four bytes.
        EncodedFrameFinisher fromConfiguration(VideoCodec::AV1);
        remember(fromConfiguration, joined({ { 0x81, 0x00, 0x0C, 0x00 }, obu(OBU_SEQUENCE_HEADER, sequence) }));
        REQUIRE(finish(fromConfiguration, obu(OBU_FRAME, key)).data == joined({ obu(OBU_SEQUENCE_HEADER, sequence), obu(OBU_FRAME, key) }));

        // A stream of still pictures has key frames only, and its frame headers do not say so.
        const std::vector<uint8_t> stills = { 0x18, 0x00 };
        finished = finish(fromConfiguration, joined({ obu(OBU_SEQUENCE_HEADER, stills), obu(OBU_FRAME, predicted) }));
        REQUIRE((finished.whole && finished.keyframe));
    }
    {
        EncodedFrameFinisher finisher(VideoCodec::VP8);
        REQUIRE(finish(finisher, { 0x10, 0x02, 0x00 }).keyframe);
        Finished finished = finish(finisher, { 0x11, 0x02, 0x00 });
        REQUIRE((finished.whole && !finished.keyframe));
        EncodedFrameFinisher unknown;
        REQUIRE(!finish(unknown, { 0x10, 0x02, 0x00 }).whole);
    }
}

TEST_CASE("The probe turns down what cannot keep up", "[video][encode]")
{
    warmUpEncoders();
    const auto cases = encoderCases();
    if (cases.empty())
        SKIP("no encoders available to test");

    for (const auto &encoderCase : cases) {
        DYNAMIC_SECTION("encoder: " << encoderCase.encoderName) {
            if (std::find(gaveNoAnswer.begin(), gaveNoAnswer.end(), encoderCase.encoderName) != gaveNoAnswer.end())
                SKIP("the probe gave this one up, and a second thread is not left inside it");
            constexpr int PICTURES = 30;
            constexpr int SETTLING = 10;
            constexpr int PROBE_BITRATE = 1'000'000;
            const int answersWithinMs = Core::Media::videoEncoderWorks(encoderCase.encoderName) ? 15000 : 3000;

            struct Seen
            {
                bool opens = true;
                int frames = 0;
                int sent = 0;
                int withTheirPicture = 0;
                bool startsWithAKeyFrame = false;
                double rate = 0;
            };

            const auto seen = std::make_shared<std::promise<Seen>>();
            std::future<Seen> finished = seen->get_future();
            std::thread([seen, encoderCase] {
                Seen result;
                EncodeRun run(encoderCase, WIDTH, HEIGHT, PROBE_BITRATE);
                size_t settled = 0;
                for (int index = 0; index < PICTURES && result.opens; index++) {
                    if (index == SETTLING)
                        settled = run.frames.size();
                    result.opens = run.encodeNext();
                }
                result.frames = static_cast<int>(run.frames.size());
                result.sent = run.sent;
                result.withTheirPicture = static_cast<int>(std::count(run.producedItsOwnFrame.begin(), run.producedItsOwnFrame.end(), true));
                result.startsWithAKeyFrame = !run.frames.empty() && run.frames[0].keyframe;
                result.rate = run.bitsPerSecond(settled, run.frames.size(), PICTURES - SETTLING);
                seen->set_value(result);
            }).detach();

            if (finished.wait_for(std::chrono::milliseconds(answersWithinMs)) != std::future_status::ready) {
                REQUIRE(!Core::Media::videoEncoderWorks(encoderCase.encoderName));
                return;
            }
            const Seen result = finished.get();
            const bool keepsUp = result.opens && result.withTheirPicture == PICTURES && result.startsWithAKeyFrame;
            const bool holdsTheRate = result.rate > PROBE_BITRATE * 0.25 && result.rate < PROBE_BITRATE * 2.0;
            REQUIRE(Core::Media::videoEncoderWorks(encoderCase.encoderName) == (keepsUp && holdsTheRate));
        }
    }
}

TEST_CASE("Every picture comes out at once and decodes", "[video][encode]")
{
    const auto cases = encoderCases();
    if (cases.empty())
        SKIP("no encoders available to test");

    for (const auto &encoderCase : cases) {
        DYNAMIC_SECTION("encoder: " << encoderCase.encoderName) {
            if (!encoderCase.encoderName.empty() && !Core::Media::videoEncoderWorks(encoderCase.encoderName))
                return;
            constexpr int PICTURES = 75;
            const std::vector<int> forced = { 20, 21, 47 };

            EncodeRun run(encoderCase, WIDTH, HEIGHT, BITRATE);
            for (int index = 0; index < PICTURES; index++) {
                if (std::find(forced.begin(), forced.end(), index) != forced.end())
                    run.encoder.requestKeyframe();
                INFO("picture " << index << " could not be encoded");
                REQUIRE(run.encodeNext());
            }

            if (encoderCase.codec != VideoCodec::VP8) {
                const int profile = profileOf(encoderCase.codec, run.frames[0].data);
                REQUIRE((encoderCase.codec == VideoCodec::H264 ? profile == 77 || profile == 100 : profile == (encoderCase.codec == VideoCodec::H265 ? 1 : 0)));
            }

            if (run.encoder.stats().encoderName == "libx264")
                REQUIRE(nalTypes(encoderCase.codec, run.frames[1].data).size() == size_t(1));

            // One picture in, its frame out of the same call: nothing is held back.
            REQUIRE(run.frames.size() == size_t(PICTURES));
            for (int index = 0; index < PICTURES; index++) {
                INFO("picture " << index << " did not come out with its own call");
                REQUIRE(run.producedItsOwnFrame[static_cast<size_t>(index)]);
            }
            REQUIRE(run.encoder.stats().opened == 1);
            REQUIRE(run.encoder.stats().encoded == PICTURES);

            // Key frames where they were asked for and nowhere else: none come by themselves.
            for (int index = 0; index < PICTURES; index++) {
                const bool expected = index == 0 || std::find(forced.begin(), forced.end(), index) != forced.end();
                INFO("frame " << index << ": key frame " << run.frames[static_cast<size_t>(index)].keyframe << ", expected " << expected);
                REQUIRE(run.frames[static_cast<size_t>(index)].keyframe == expected);
            }
            for (const EncodedVideoFrame &frame : run.frames)
                REQUIRE(isWellFormed(encoderCase.codec, frame));

            const Decoded whole = decodeAll(encoderCase.codec, run.frames);
            REQUIRE(whole.errors == 0);
            REQUIRE(whole.dropped == 0);
            REQUIRE(whole.pictures == PICTURES);
            REQUIRE(isThePictureSent(encoderCase.codec, whole, FrameSize{WIDTH, HEIGHT}));
            REQUIRE((whole.format == AV_PIX_FMT_YUV420P || whole.format == AV_PIX_FMT_YUVJ420P));

            // A key frame needs nothing that came before it, and what follows needs only the key frame.
            for (size_t index = 0; index < run.frames.size(); index++) {
                if (!run.frames[index].keyframe)
                    continue;
                const Decoded alone = decodeAll(encoderCase.codec, std::vector<EncodedVideoFrame>{ run.frames[index] });
                REQUIRE(alone.errors == 0);
                REQUIRE(alone.pictures == 1);
                REQUIRE(isThePictureSent(encoderCase.codec, alone, FrameSize{WIDTH, HEIGHT}));

                const std::vector<EncodedVideoFrame> fromHere(run.frames.begin() + static_cast<std::ptrdiff_t>(index), run.frames.end());
                const Decoded rest = decodeAll(encoderCase.codec, fromHere);
                REQUIRE(rest.errors == 0);
                REQUIRE(rest.pictures == static_cast<int>(fromHere.size()));
            }
        }
    }
}

TEST_CASE("A changed bitrate is followed", "[video][encode]")
{
    const auto cases = encoderCases();
    if (cases.empty())
        SKIP("no encoders available to test");

    for (const auto &encoderCase : cases) {
        DYNAMIC_SECTION("encoder: " << encoderCase.encoderName) {
            if (!encoderCase.encoderName.empty() && !Core::Media::videoEncoderWorks(encoderCase.encoderName))
                return;
            constexpr int PICTURES_PER_RATE = 90;
            constexpr int SETTLING = 30;
            constexpr int LOWER_BITRATE = 500'000;

            EncodeRun run(encoderCase, WIDTH, HEIGHT, BITRATE);
            size_t settledBefore = 0;
            for (int index = 0; index < PICTURES_PER_RATE; index++) {
                if (index == SETTLING)
                    settledBefore = run.frames.size();
                REQUIRE(run.encodeNext());
            }
            const size_t atChange = run.frames.size();

            run.encoder.setBitrate(LOWER_BITRATE);
            size_t settledAfter = 0;
            for (int index = 0; index < PICTURES_PER_RATE; index++) {
                if (index == SETTLING)
                    settledAfter = run.frames.size();
                REQUIRE(run.encodeNext());
            }

            const double before = run.bitsPerSecond(settledBefore, atChange, PICTURES_PER_RATE - SETTLING);
            const double after = run.bitsPerSecond(settledAfter, run.frames.size(), PICTURES_PER_RATE - SETTLING);

            REQUIRE(before > BITRATE * 0.7);
            REQUIRE(before < BITRATE * 1.3);
            REQUIRE(after > LOWER_BITRATE * 0.7);
            REQUIRE(after < LOWER_BITRATE * 1.3);

            if (run.encoder.stats().opened > 1)
                REQUIRE(run.frames[atChange].keyframe);
            if (run.encoder.stats().encoderName == "libx264")
                REQUIRE((run.encoder.stats().opened == 1 && !run.frames[atChange].keyframe));
            for (const EncodedVideoFrame &frame : run.frames)
                REQUIRE(isWellFormed(encoderCase.codec, frame));
            const Decoded decoded = decodeAll(encoderCase.codec, run.frames);
            REQUIRE(decoded.errors == 0);
            REQUIRE(decoded.dropped == 0);
            REQUIRE(decoded.pictures == static_cast<int>(run.frames.size()));
        }
    }
}

TEST_CASE("Small moves of the rate do not start an encoder over", "[video][encode]")
{
    const auto cases = encoderCases();
    if (cases.empty())
        SKIP("no encoders available to test");

    for (const auto &encoderCase : cases) {
        DYNAMIC_SECTION("encoder: " << encoderCase.encoderName) {
            if (!encoderCase.encoderName.empty() && !Core::Media::videoEncoderWorks(encoderCase.encoderName))
                return;
            constexpr int PICTURES_PER_RATE = 6;

            EncodeRun run(encoderCase, WIDTH, HEIGHT, BITRATE);
            const auto openedAt = [&run](int bitrate) {
                run.encoder.setBitrate(bitrate);
                for (int index = 0; index < PICTURES_PER_RATE; index++) {
                    if (!run.encodeNext())
                        return uint64_t(0);
                }
                return static_cast<uint64_t>(run.encoder.stats().opened);
            };

            const uint64_t atFirst = openedAt(BITRATE);
            REQUIRE(atFirst == 1);

            const uint64_t lowered = openedAt(BITRATE * 95 / 100);
            REQUIRE((lowered == atFirst || lowered == atFirst + 1));
            const bool startsOver = lowered > atFirst;
            REQUIRE(openedAt(BITRATE * 90 / 100) == lowered);
            REQUIRE(openedAt(BITRATE * 88 / 100) == lowered);
            REQUIRE(openedAt(BITRATE) == lowered);

            REQUIRE(openedAt(BITRATE * 80 / 100) == lowered + (startsOver ? 1 : 0));
            REQUIRE(openedAt(BITRATE * 120 / 100) == lowered + (startsOver ? 2 : 0));

            for (const EncodedVideoFrame &frame : run.frames)
                REQUIRE(isWellFormed(encoderCase.codec, frame));
            const Decoded decoded = decodeAll(encoderCase.codec, run.frames);
            REQUIRE(decoded.errors == 0);
        }
    }
}

TEST_CASE("A still picture is not filled up to the rate", "[video][encode]")
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    std::vector<EncoderCase> cases;
    for (VideoCodec codec : CODECS) {
        for (const std::string &name : Core::Media::videoEncoderNames(codec)) {
            if (name.ends_with("_vaapi"))
                cases.push_back(EncoderCase{ codec, name });
        }
    }
    if (cases.empty())
        SKIP("none of Mesa's encoders is here");

    for (const auto &encoderCase : cases) {
        DYNAMIC_SECTION("encoder: " << encoderCase.encoderName) {
            if (!encoderCase.encoderName.empty() && !Core::Media::videoEncoderWorks(encoderCase.encoderName))
                return;
            constexpr int PICTURES = 90;
            constexpr int SETTLING = 30;

            EncodeRun run(encoderCase, 1280, 720, BITRATE);
            run.paintNext();
            size_t settled = 0;
            for (int index = 0; index < PICTURES; index++) {
                if (index == SETTLING)
                    settled = run.frames.size();
                REQUIRE(run.encodeNext(false));
            }
            const double rate = run.bitsPerSecond(settled, run.frames.size(), PICTURES - SETTLING);
            REQUIRE(rate < BITRATE / 2);
        }
    }
}

TEST_CASE("A changed picture size starts a new stream", "[video][encode]")
{
    const auto cases = encoderCases();
    if (cases.empty())
        SKIP("no encoders available to test");

    for (const auto &encoderCase : cases) {
        DYNAMIC_SECTION("encoder: " << encoderCase.encoderName) {
            if (!encoderCase.encoderName.empty() && !Core::Media::videoEncoderWorks(encoderCase.encoderName))
                return;
            constexpr int PICTURES = 10;

            EncodeRun run(encoderCase, WIDTH, HEIGHT, BITRATE);
            REQUIRE(run.encodeNext());

            AVFrame *larger = allocatePicture(run.encoder.softwareFormat(), 960, 540);
            std::vector<EncodedVideoFrame> frames;
            for (int index = 0; index < PICTURES; index++) {
                paint(larger, index);
                REQUIRE(run.encoder.encode(larger, (index + 1) * TICKS_PER_FRAME, frames) == VideoEncoder::Outcome::Encoded);
            }
            av_frame_free(&larger);

            REQUIRE(frames.size() == size_t(PICTURES));
            REQUIRE(frames[0].keyframe);
            REQUIRE(!frames[1].keyframe);
            const Decoded decoded = decodeAll(encoderCase.codec, frames);
            REQUIRE(decoded.errors == 0);
            REQUIRE(decoded.pictures == PICTURES);
            REQUIRE(isThePictureSent(encoderCase.codec, decoded, FrameSize{960, 540}));
        }
    }
}

TEST_CASE("No key frame comes unasked", "[video][encode]")
{
    const auto cases = encoderCases();
    if (cases.empty())
        SKIP("no encoders available to test");

    for (const auto &encoderCase : cases) {
        DYNAMIC_SECTION("encoder: " << encoderCase.encoderName) {
            if (!encoderCase.encoderName.empty() && !Core::Media::videoEncoderWorks(encoderCase.encoderName))
                return;
            constexpr int PICTURES = 330;

            EncodeRun run(encoderCase, WIDTH, HEIGHT, BITRATE);
            for (int index = 0; index < PICTURES; index++)
                REQUIRE(run.encodeNext());
            REQUIRE(run.frames[0].keyframe);
            REQUIRE(std::count_if(run.frames.begin(), run.frames.end(), [](const EncodedVideoFrame &frame) { return frame.keyframe; }) == 1);
            REQUIRE(run.encoder.stats().opened == 1);
        }
    }
}

TEST_CASE("The stream says what colours it is in", "[video][encode]")
{
    const auto cases = encoderCases();
    if (cases.empty())
        SKIP("no encoders available to test");

    for (const auto &baseCase : cases) {
        for (int colorspace : { AVCOL_SPC_BT709, AVCOL_SPC_SMPTE170M }) {
            EncoderCase encoderCase = baseCase;
            encoderCase.detail = colorspace;
            DYNAMIC_SECTION(encoderCase.encoderName + (colorspace == AVCOL_SPC_BT709 ? ", BT.709" : ", BT.601")) {
                if (!encoderCase.encoderName.empty() && !Core::Media::videoEncoderWorks(encoderCase.encoderName))
                    return;
                constexpr int PICTURES = 20;
                const auto sent = static_cast<AVColorSpace>(encoderCase.detail);
                const Yuv patch = patchInYuv(sent == AVCOL_SPC_BT709);

                VideoEncoder encoder(settingsFor(encoderCase, BITRATE));
                AVFrame *picture = allocatePicture(encoder.softwareFormat(), WIDTH, HEIGHT);
                std::vector<EncodedVideoFrame> frames;
                for (int index = 0; index < PICTURES; index++) {
                    av_frame_make_writable(picture);
                    paintYuv(picture, index);
                    stampPatch(picture, patch);
                    picture->colorspace = sent;
                    picture->color_range = AVCOL_RANGE_MPEG;
                    REQUIRE(encoder.encode(picture, index * TICKS_PER_FRAME, frames) == VideoEncoder::Outcome::Encoded);
                }
                av_frame_free(&picture);

                const Decoded decoded = decodeAll(encoderCase.codec, frames);
                REQUIRE(decoded.errors == 0);
                REQUIRE(decoded.pictures == static_cast<int>(frames.size()));
                REQUIRE(distance(decoded.middle, patch) < 6);
                REQUIRE(decoded.range != AVCOL_RANGE_JPEG);

                if (encoderCase.codec == VideoCodec::VP8)
                    return;
                if (sent == AVCOL_SPC_BT709)
                    REQUIRE(decoded.colorspace == static_cast<int>(AVCOL_SPC_BT709));
                else
                    REQUIRE((decoded.colorspace == AVCOL_SPC_SMPTE170M || decoded.colorspace == AVCOL_SPC_BT470BG || decoded.colorspace == AVCOL_SPC_UNSPECIFIED));
            }
        }
    }
}

TEST_CASE("A size is coded as it is said to be", "[video][encode]")
{
    const auto cases = encoderCases();
    if (cases.empty())
        SKIP("no encoders available to test");

    for (const auto &encoderCase : cases) {
        DYNAMIC_SECTION("encoder: " << encoderCase.encoderName) {
            if (!encoderCase.encoderName.empty() && !Core::Media::videoEncoderWorks(encoderCase.encoderName))
                return;
            constexpr int PICTURES = 5;

            for (const FrameSize &size : { FrameSize{1920, 1080}, FrameSize{640, 360}, FrameSize{1000, 700} }) {
                EncodeRun run(encoderCase, size.width, size.height, 3'000'000);
                for (int index = 0; index < PICTURES; index++)
                    REQUIRE(run.encodeNext());
                const Decoded decoded = decodeAll(encoderCase.codec, run.frames);
                REQUIRE(decoded.errors == 0);
                REQUIRE(decoded.pictures == static_cast<int>(run.frames.size()));
                REQUIRE(isThePictureSent(encoderCase.codec, decoded, size));
                REQUIRE(FrameSize{decoded.width, decoded.height} == Core::Media::codedSize(encoderCase.encoderName, size));
            }
        }
    }
}

TEST_CASE("Pictures on the graphics card are encoded there", "[video][encode]")
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    for (VideoCodec codec : CODECS) {
        for (const std::string &name : Core::Media::videoEncoderNames(codec)) {
            for (int format : { AV_PIX_FMT_NV12, AV_PIX_FMT_BGRA }) {
                EncoderCase encoderCase{ codec, name, format };
                DYNAMIC_SECTION(name + (format == AV_PIX_FMT_BGRA ? ", BGRA" : ", NV12")) {
                    if (!encoderCase.encoderName.empty() && !Core::Media::videoEncoderWorks(encoderCase.encoderName))
                        return;
                    constexpr int PICTURES = 40;
                    constexpr int FORCED = 20;
                    const bool rgb = encoderCase.detail == AV_PIX_FMT_BGRA;

                    CardPictures pictures(static_cast<AVPixelFormat>(encoderCase.detail), WIDTH, HEIGHT, AVCOL_SPC_BT709, cardDevice(encoderCase.encoderName));
                    if (!pictures.usable())
                        SKIP("the graphics card has no device that takes these pictures");

                    VideoEncoder encoder(settingsFor(encoderCase, BITRATE));
                    AVFrame *fromMemory = allocatePicture(encoder.softwareFormat(), WIDTH, HEIGHT);
                    paint(fromMemory, 0);
                    std::vector<EncodedVideoFrame> frames;

                    if (!(rgb ? encoder.takesRgbCardFrames() : encoder.takesCardFrames())) {
                        AVFrame *picture = pictures.next(0);
                        REQUIRE(picture != nullptr);
                        REQUIRE(encoder.encode(picture, 0, frames) == VideoEncoder::Outcome::CardFramesRefused);
                        REQUIRE(frames.empty());
                        REQUIRE(encoder.encode(fromMemory, TICKS_PER_FRAME, frames) == VideoEncoder::Outcome::Encoded);
                        av_frame_free(&fromMemory);
                        REQUIRE(frames.size() == 1);
                        REQUIRE(frames[0].keyframe);
                        return;
                    }

                    for (int index = 0; index < PICTURES; index++) {
                        if (index == FORCED)
                            encoder.requestKeyframe();
                        AVFrame *picture = pictures.next(index);
                        REQUIRE(picture != nullptr);
                        REQUIRE(encoder.encode(picture, index * TICKS_PER_FRAME, frames) == VideoEncoder::Outcome::Encoded);
                        REQUIRE(frames.size() == size_t(index + 1));
                        REQUIRE(frames.back().rtpTimestamp == uint32_t(index * TICKS_PER_FRAME));
                    }
                    REQUIRE(encoder.stats().encoderName.ends_with(pictures.device));
                    REQUIRE(encoder.stats().opened == 1);
                    for (int index = 0; index < PICTURES; index++) {
                        REQUIRE(frames[static_cast<size_t>(index)].keyframe == (index == 0 || index == FORCED));
                        REQUIRE(isWellFormed(encoderCase.codec, frames[static_cast<size_t>(index)]));
                    }

                    const Decoded decoded = decodeAll(encoderCase.codec, frames);
                    REQUIRE(decoded.errors == 0);
                    REQUIRE(decoded.pictures == PICTURES);
                    REQUIRE(isThePictureSent(encoderCase.codec, decoded, FrameSize{WIDTH, HEIGHT}));
                    if (rgb) {
                        REQUIRE(distance(decoded.middle, patchInYuv(true)) < 6);
                        REQUIRE(distance(decoded.middle, patchInYuv(false)) > 12);
                        REQUIRE(decoded.colorspace == static_cast<int>(AVCOL_SPC_BT709));
                    }

                    REQUIRE(encoder.encode(fromMemory, PICTURES * TICKS_PER_FRAME, frames) == VideoEncoder::Outcome::Encoded);
                    av_frame_free(&fromMemory);
                    REQUIRE(frames.size() == size_t(PICTURES + 1));
                    REQUIRE(frames.back().keyframe);
                    REQUIRE_FALSE(encoder.stats().encoderName.ends_with(pictures.device));
                }
            }
        }
    }
}

TEST_CASE("The card scales and converts pictures", "[video][encode]")
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    using Core::Video::CardScaler;
    constexpr double CLOSE = 2.5;
    constexpr double FAR_OFF = 5;
    const FrameSize size{ 480, 360 };
    const CaptureRect all{ 0, 0, size.width, size.height };

    for (bool bt601 : { false, true }) {
        DYNAMIC_SECTION(std::string(bt601 ? "to BT.601" : "to BT.709")) {
            const CardScaler::Output output = { size, bt601, false };

            AVBufferRef *device = nullptr;
            if (!Core::Media::hardwareDeviceCanBeAskedFor(cardDevice()) || av_hwdevice_ctx_create(&device, cardDevice(), nullptr, nullptr, 0) < 0)
                SKIP("the graphics card has no device for pictures that stay on it");
            AVFrame *large = quartersOnCard(device, FrameSize{ 1280, 960 });
            AVFrame *another = quartersOnCard(device, FrameSize{ 1280, 960 });
            AVFrame *lesser = quartersOnCard(device, FrameSize{ 640, 480 });
            AVFrame *scaled = av_frame_alloc();
            AVFrame *inMemory = av_frame_alloc();
            auto release = [&] {
                av_frame_free(&inMemory);
                av_frame_free(&scaled);
                av_frame_free(&lesser);
                av_frame_free(&another);
                av_frame_free(&large);
                av_buffer_unref(&device);
            };
            REQUIRE((large && another && lesser));

            CardScaler scaler;
            if (!scaler.scale(large, output, scaled)) {
                REQUIRE_FALSE(CardScaler::usable());
                release();
                SKIP("this machine's graphics card does not scale pictures");
            }
            REQUIRE(CardScaler::usable());

            const auto shows = [&](int perMilleX, int perMilleY, int quarter, const char *where) {
                const int x = size.width * perMilleX / 1000;
                const int y = size.height * perMilleY / 1000;
                const Yuv seen = colourAt(inMemory, x, y);
                const Yuv expected = quarter < 0 ? Yuv{ 16, 128, 128 } : inYuv(QUARTER_COLOURS[static_cast<size_t>(quarter)], !bt601);
                const double off = distance(seen, expected);
                const double offTheOther = quarter < 0 ? 0 : distance(seen, inYuv(QUARTER_COLOURS[static_cast<size_t>(quarter)], bt601));
                return off < CLOSE && (quarter < 0 || quarter == 3 || offTheOther > FAR_OFF);
            };
            const auto takenOffTheCard = [&] {
                av_frame_unref(inMemory);
                return av_hwframe_transfer_data(inMemory, scaled, 0) >= 0 && inMemory->format == AV_PIX_FMT_NV12 && FrameSize{ inMemory->width, inMemory->height } == size;
            };

            REQUIRE(scaled->format == static_cast<int>(cardFormat(cardDevice())));
            REQUIRE(formatOnCard(scaled) == AV_PIX_FMT_NV12);
            REQUIRE(FrameSize{ scaled->width, scaled->height } == size);
            REQUIRE(reinterpret_cast<const AVHWFramesContext *>(scaled->hw_frames_ctx->data)->device_ref->data == device->data);
            REQUIRE(static_cast<int>(scaled->colorspace) == static_cast<int>(bt601 ? AVCOL_SPC_SMPTE170M : AVCOL_SPC_BT709));
            REQUIRE(static_cast<int>(scaled->color_range) == static_cast<int>(AVCOL_RANGE_MPEG));
            const uint8_t *const kind = scaled->hw_frames_ctx->data;

            REQUIRE(takenOffTheCard());
            REQUIRE(shows(250, 250, 0, "the whole picture, top left"));
            REQUIRE(shows(750, 250, 1, "the whole picture, top right"));
            REQUIRE(shows(250, 750, 2, "the whole picture, bottom left"));
            REQUIRE(shows(750, 750, 3, "the whole picture, bottom right"));

            REQUIRE(scaler.scale(large, CaptureRect{ 640, 0, 640, 960 }, output, all, scaled));
            REQUIRE(takenOffTheCard());
            REQUIRE(shows(100, 250, 1, "the right half, top left"));
            REQUIRE(shows(900, 250, 1, "the right half, top right"));
            REQUIRE(shows(100, 750, 3, "the right half, bottom left"));
            REQUIRE(shows(900, 750, 3, "the right half, bottom right"));
            REQUIRE(scaler.scale(large, CaptureRect{ 0, 480, 640, 480 }, output, all, scaled));
            REQUIRE(takenOffTheCard());
            REQUIRE(shows(100, 100, 2, "the bottom left quarter, top left"));
            REQUIRE(shows(900, 900, 2, "the bottom left quarter, bottom right"));

            large->crop_left = 640;
            REQUIRE(scaler.scale(large, output, scaled));
            large->crop_left = 0;
            REQUIRE(takenOffTheCard());
            REQUIRE(shows(100, 250, 1, "cropped to its right half, top left"));
            REQUIRE(shows(900, 750, 3, "cropped to its right half, bottom right"));
            REQUIRE(scaled->hw_frames_ctx->data == kind);

#ifdef _WIN32
            REQUIRE(scaler.scale(large, CaptureRect{ 0, 0, 1280, 960 }, output, CaptureRect{ 120, 90, 240, 180 }, scaled));
            REQUIRE(takenOffTheCard());
            REQUIRE(shows(375, 375, 0, "placed in the middle, its top left"));
            REQUIRE(shows(625, 625, 3, "placed in the middle, its bottom right"));
            REQUIRE(shows(100, 100, -1, "placed in the middle, outside it at the top left"));
            REQUIRE(shows(900, 500, -1, "placed in the middle, outside it on the right"));
            REQUIRE(scaled->hw_frames_ctx->data == kind);

            REQUIRE(scaler.scale(lesser, output, scaled));
            REQUIRE(scaled->hw_frames_ctx->data == kind);
            REQUIRE(takenOffTheCard());
            REQUIRE(shows(250, 250, 0, "the smaller source, top left"));
            REQUIRE(shows(750, 750, 3, "the smaller source, bottom right"));
            REQUIRE(scaler.scale(large, output, scaled));
            REQUIRE(scaled->hw_frames_ctx->data == kind);
            const AVFrame *const other = lesser;
#else
            REQUIRE_FALSE(scaler.scale(large, CaptureRect{ 0, 0, 1280, 960 }, output, CaptureRect{ 120, 90, 240, 180 }, scaled));
            REQUIRE(CardScaler::usable());
            REQUIRE(scaler.scale(another, output, scaled));
            REQUIRE(scaled->hw_frames_ctx->data == kind);
            REQUIRE(scaler.scale(lesser, output, scaled));
            REQUIRE(takenOffTheCard());
            REQUIRE(shows(250, 250, 0, "the smaller source, top left"));
            REQUIRE(shows(750, 750, 3, "the smaller source, bottom right"));
            const AVFrame *const other = another;
#endif

            for (int index = 0; index < 8; index++)
                REQUIRE(scaler.scale(index % 2 ? other : large, output, scaled));

            release();
        }
    }
}

TEST_CASE("Pictures scaled on the card are encoded there", "[video][encode]")
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    using Core::Video::CardScaler;
    constexpr int PICTURES = 40;
    const Core::Video::CardScaler::Output output = { FrameSize{ WIDTH, HEIGHT }, false, false };

    for (VideoCodec codec : CODECS) {
        for (const std::string &name : Core::Media::videoEncoderNames(codec)) {
            EncoderCase encoderCase{ codec, name };
            DYNAMIC_SECTION(name) {
                if (!encoderCase.encoderName.empty() && !Core::Media::videoEncoderWorks(encoderCase.encoderName))
                    return;

                VideoEncoder encoder(settingsFor(encoderCase, BITRATE));
                if (!encoder.takesCardFrames())
                    SKIP("this encoder takes no pictures from the graphics card");
                if (cardDevice(encoderCase.encoderName) != cardDevice())
                    SKIP("this encoder takes pictures of another kind than the card's scaler makes");
                CardPictures pictures(AV_PIX_FMT_BGRA, 2 * WIDTH, 2 * HEIGHT, AVCOL_SPC_RGB);
                if (!pictures.usable())
                    SKIP("the graphics card has no device that takes these pictures");

                CardScaler scaler;
                AVFrame *scaled = av_frame_alloc();
                std::vector<EncodedVideoFrame> frames;
                bool failedScaling = false;
                for (int index = 0; index < PICTURES; index++) {
                    AVFrame *picture = pictures.next(index);
                    REQUIRE(picture);
                    if (!scaler.scale(picture, output, scaled)) {
                        REQUIRE_FALSE(CardScaler::usable());
                        failedScaling = true;
                        break;
                    }
                    REQUIRE(formatOnCard(scaled) == AV_PIX_FMT_NV12);
                    REQUIRE(encoder.encode(scaled, index * TICKS_PER_FRAME, frames) == VideoEncoder::Outcome::Encoded);
                    REQUIRE(frames.size() == size_t(index + 1));
                }
                av_frame_free(&scaled);
                if (failedScaling)
                    SKIP("this machine's graphics card does not scale pictures");

                REQUIRE(encoder.stats().encoderName.ends_with(pictures.device));
                REQUIRE(encoder.stats().opened == 1);
                for (int index = 0; index < PICTURES; index++) {
                    REQUIRE(frames[static_cast<size_t>(index)].keyframe == (index == 0));
                    REQUIRE(isWellFormed(encoderCase.codec, frames[static_cast<size_t>(index)]));
                }

                const Decoded decoded = decodeAll(encoderCase.codec, frames);
                REQUIRE(decoded.errors == 0);
                REQUIRE(decoded.pictures == PICTURES);
                REQUIRE(isThePictureSent(encoderCase.codec, decoded, output.size));
                REQUIRE(distance(decoded.middle, patchInYuv(true)) < 6);
                REQUIRE(distance(decoded.middle, patchInYuv(false)) > 12);
                REQUIRE(decoded.colorspace == static_cast<int>(AVCOL_SPC_BT709));
            }
        }
    }
}

TEST_CASE("Screens are listed with thumbnails", "[video][capture]")
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    if (!Core::Video::screenCaptureAvailable()) {
        REQUIRE(Core::Video::captureSources().empty());
        SKIP("this build or this machine cannot capture a screen");
    }
    const auto sources = Core::Video::captureSources();
    if (sources.empty())
        SKIP("no screen is attached");

    const FrameSize bound{320, 180};
    for (size_t i = 0; i < sources.size(); i++) {
        REQUIRE(sources[i].id == "screen:" + std::to_string(i));
        REQUIRE_FALSE(sources[i].name.empty());
        REQUIRE((sources[i].geometry.width >= 320 && sources[i].geometry.height >= 200));

        const auto thumbnail = Core::Video::captureThumbnail(sources[i], bound);
        REQUIRE_FALSE(thumbnail.empty());
        const FrameSize expected = scaledKeepAspectRatio({sources[i].geometry.width, sources[i].geometry.height}, bound);
        REQUIRE(thumbnail.width == expected.width);
        REQUIRE(thumbnail.height == expected.height);
        REQUIRE(thumbnail.channels == 4);
        REQUIRE(thumbnail.pixels[3] == 0xFF);
    }
    REQUIRE(Core::Video::captureThumbnail({}, bound).empty());
}

TEST_CASE("The pipeline shares the screen", "[video][encode]")
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    using namespace Core::Video;
    const std::vector<VideoCodec> encodable = Core::Media::encodableVideoCodecs();
    if (encodable.empty())
        SKIP("this machine has no encoder");
    if (!screenCaptureAvailable())
        SKIP("this build or this machine cannot capture a screen");

    std::vector<std::pair<EncoderCase, CaptureMethod>> cases;
    for (VideoCodec codec : encodable)
        cases.push_back({ EncoderCase{ codec, "", 720 }, CaptureMethod::Auto });
    if (!encodable.empty()) {
        cases.push_back({ EncoderCase{ encodable.front(), "", 0 }, CaptureMethod::Auto });
        cases.push_back({ EncoderCase{ encodable.front(), "", 720 }, CaptureMethod::GraphicsCapture });
        cases.push_back({ EncoderCase{ encodable.front(), "", 0 }, CaptureMethod::GraphicsCapture });
        cases.push_back({ EncoderCase{ encodable.front(), "", 720 }, CaptureMethod::DesktopDuplication });
    }

    for (const auto &[encoderCase, method] : cases) {
        std::string sectionName = Discord::Voice::videoCodecName(encoderCase.codec) +
            (encoderCase.detail == 0 ? " at screen's own size" : " at 720p") +
            (method == CaptureMethod::GraphicsCapture ? " by gfxcapture" :
             method == CaptureMethod::DesktopDuplication ? " by ddagrab" : " auto");
        DYNAMIC_SECTION(sectionName) {
            if (!captureMethodUsable(method))
                SKIP("this build or this system does not have the capture method");
            const auto sources = captureSources();
            if (sources.empty())
                SKIP("no screen is attached");
            if (!aScreenIsOnAGraphicsCard())
                SKIP("no screen of this machine is on a graphics card");

            setCaptureMethod(method);
            CaptureSource source = sources.front();
#ifdef _WIN32
            std::unique_ptr<TestWindow> stirring;
            {
                SKIP_WITHOUT_A_QUIET_SCREEN(quiet);
                auto shared = std::find_if(sources.begin(), sources.end(), [&quiet](const CaptureSource &each) { return each.geometry == quiet->whole; });
                REQUIRE(shared != sources.end());
                source = *shared;
                stirring = std::make_unique<TestWindow>(TestWindow::Look{ FrameSize{440, 330}, POINT{ quiet->whole.center().x - 220, quiet->whole.center().y - 190 } });
                REQUIRE(stirring->isOpen());
                REQUIRE(stirring->insideOnScreen().adjusted(40, 40, -40, -40).contains(quiet->whole.center()));
            }
#endif
            const FrameSize expectedSize = encodeSizeFor(source.geometry.size(), encoderCase.detail);
            const std::vector<VideoCodec> shared = codecsSharedAt(expectedSize);
            if (std::find(shared.begin(), shared.end(), encoderCase.codec) == shared.end() && method == CaptureMethod::Auto)
                SKIP("a screen of this size is not shared in this codec, whose encoder would add black to it");
            const VideoCodec codec = std::find(shared.begin(), shared.end(), encoderCase.codec) != shared.end() ? encoderCase.codec : shared.front();

            BroadcastPipeline pipeline;
            PipelineRecord record(pipeline);

            BroadcastSettings settings;
            settings.sourceId = source.id;
            settings.maxHeight = encoderCase.detail;
            settings.framerate = FRAMERATE;
            settings.codec = codec;
            settings.bitrate = 2'500'000;
            settings.maxBitrate = 4'000'000;

            pipeline.start(settings);
            REQUIRE(tryUntil([&] {
                std::lock_guard lock(record.mutex);
                return record.frames.size() >= 20 || !record.failures.empty();
            }, 10000));
            {
                std::lock_guard lock(record.mutex);
                REQUIRE(record.failures.empty());
                REQUIRE(record.starts.size() == 1);
                REQUIRE(record.starts.front() == expectedSize);
                REQUIRE(record.frames[0]->keyframe);
                REQUIRE_FALSE(record.frames[1]->keyframe);
                for (size_t i = 1; i < 20; i++)
                    REQUIRE(static_cast<int32_t>(record.frames[i]->rtpTimestamp - record.frames[i - 1]->rtpTimestamp) > 0);
                const double ticksPerFrame = static_cast<double>(record.frames[19]->rtpTimestamp - record.frames[0]->rtpTimestamp) / 19.0;
                REQUIRE(ticksPerFrame > TICKS_PER_FRAME * 0.8);
                REQUIRE(ticksPerFrame < TICKS_PER_FRAME * 1.25);
            }

            size_t atRequest = 0;
            {
                std::lock_guard lock(record.mutex);
                atRequest = record.frames.size();
            }
            pipeline.requestKeyframe();
            REQUIRE(tryUntil([&] {
                std::lock_guard lock(record.mutex);
                return record.frames.size() >= atRequest + 6;
            }, 1500));
            {
                std::lock_guard lock(record.mutex);
                auto forced = std::find_if(record.frames.begin() + static_cast<std::ptrdiff_t>(atRequest), record.frames.end(), [](const EncodedFramePointer &f) { return f->keyframe; });
                REQUIRE(forced != record.frames.end());
                REQUIRE(forced - record.frames.begin() < static_cast<std::ptrdiff_t>(atRequest) + 4);
            }

            REQUIRE(tryUntil([&] {
                std::lock_guard lock(record.mutex);
                return !record.previews.empty();
            }, 2000));
            pipeline.setEncodingSuspended(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            size_t whileSuspended = 0;
            size_t previewsBefore = 0;
            {
                std::lock_guard lock(record.mutex);
                whileSuspended = record.frames.size();
                previewsBefore = record.previews.size();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
            {
                std::lock_guard lock(record.mutex);
                REQUIRE(record.frames.size() == whileSuspended);
                REQUIRE(record.previews.size() >= previewsBefore + 2);
            }

            pipeline.setEncodingSuspended(false);
            REQUIRE(tryUntil([&] {
                std::lock_guard lock(record.mutex);
                return record.frames.size() >= whileSuspended + 10;
            }, 1500));
            {
                std::lock_guard lock(record.mutex);
                REQUIRE(record.frames[whileSuspended]->keyframe);
                REQUIRE_FALSE(record.frames[whileSuspended + 1]->keyframe);
            }

            pipeline.setBitrate(1'000'000);
            size_t atNewRate = 0;
            {
                std::lock_guard lock(record.mutex);
                atNewRate = record.frames.size();
            }
            REQUIRE(tryUntil([&] {
                std::lock_guard lock(record.mutex);
                return record.frames.size() >= atNewRate + 15;
            }, 1500));

            {
                std::lock_guard lock(record.mutex);
                for (const auto &preview : record.previews) {
                    REQUIRE_FALSE(preview.empty());
                    REQUIRE((preview.width <= 512 && preview.height <= 288));
                }
            }

            size_t statsBefore = 0;
            {
                std::lock_guard lock(record.mutex);
                statsBefore = record.stats.size();
            }
            REQUIRE(tryUntil([&] {
                std::lock_guard lock(record.mutex);
                return record.stats.size() >= statsBefore + 2;
            }, 3500));
            BroadcastStats lastStats;
            {
                std::lock_guard lock(record.mutex);
                lastStats = record.stats.back();
            }
            REQUIRE(lastStats.encodedSize == expectedSize);
            constexpr double FEWEST = 0.6;
            REQUIRE(lastStats.capturedFps > FRAMERATE * FEWEST);
            REQUIRE(lastStats.capturedFps < FRAMERATE * 1.2);
#ifdef _WIN32
            REQUIRE(lastStats.encoderName.ends_with(method == CaptureMethod::GraphicsCapture ? ", from gfxcapture" : ", from ddagrab"));
            REQUIRE((lastStats.encoderName.find(onDevice(cardDevice())) != std::string::npos) == reachesTheEncoderOnTheCard(codec, expectedSize != source.geometry.size(), method == CaptureMethod::GraphicsCapture));
#endif

            pipeline.stop();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            size_t afterStop = 0;
            {
                std::lock_guard lock(record.mutex);
                afterStop = record.frames.size();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            {
                std::lock_guard lock(record.mutex);
                REQUIRE(record.frames.size() == afterStop);
                REQUIRE(record.failures.empty());
                REQUIRE(record.stalls.empty());
                for (const auto &frame : record.frames)
                    REQUIRE(isWellFormed(codec, *frame));
            }

            std::vector<EncodedFramePointer> snapshot;
            {
                std::lock_guard lock(record.mutex);
                snapshot = record.frames;
            }
            const Decoded decoded = decodeAll(codec, snapshot);
            REQUIRE(decoded.errors == 0);
            REQUIRE(decoded.dropped == 0);
            REQUIRE(decoded.pictures == static_cast<int>(snapshot.size()));
            REQUIRE(isThePictureSent(codec, decoded, expectedSize));
#ifdef _WIN32
            if (stirring) {
                REQUIRE(offItsOwnCoefficients(decoded, decoded.middle, SHARED_COLOUR) < COLOUR_SURVIVES);
            }
#endif

            const uint32_t lastTimestamp = snapshot.back()->rtpTimestamp;
            {
                std::lock_guard lock(record.mutex);
                record.frames.clear();
            }
            pipeline.start(settings);
            REQUIRE(tryUntil([&] {
                std::lock_guard lock(record.mutex);
                return record.frames.size() >= 5 || !record.failures.empty();
            }, 3000));
            {
                std::lock_guard lock(record.mutex);
                REQUIRE(record.failures.empty());
                REQUIRE(record.starts.size() == 2);
                REQUIRE(record.frames[0]->keyframe);
                REQUIRE(static_cast<int32_t>(record.frames[0]->rtpTimestamp - lastTimestamp) > 0);
            }
            pipeline.stop();
        }
    }
}

TEST_CASE("A scaled screen is handed over as its encoder takes it", "[video][encode]")
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    using namespace Core::Video;
    constexpr int PICTURES = 15;
    constexpr int MAX_HEIGHT = 720;

    for (CaptureMethod method : { CaptureMethod::DesktopDuplication, CaptureMethod::GraphicsCapture }) {
        DYNAMIC_SECTION(std::string(method == CaptureMethod::DesktopDuplication ? "by duplication" : "by graphics capture")) {
            if (!screenCaptureAvailable() || !captureMethodUsable(method))
                SKIP("this build or this Windows cannot capture a screen this way");
            const auto sources = captureSources();
            if (sources.empty())
                SKIP("no screen is attached");
            if (!aScreenIsOnAGraphicsCard())
                SKIP("no screen of this machine is on a graphics card");

            CaptureSource source = sources.front();
            std::unique_ptr<TestWindow> stirring;
            {
                SKIP_WITHOUT_A_QUIET_SCREEN(quiet);
                auto shared = std::find_if(sources.begin(), sources.end(), [&quiet](const CaptureSource &each) { return each.geometry == quiet->whole; });
                REQUIRE(shared != sources.end());
                source = *shared;
                stirring = std::make_unique<TestWindow>(TestWindow::Look{ FrameSize{96, 96}, quiet->cornerFor(FrameSize{160, 180}) });
            }
            const FrameSize size = encodeSizeFor(source.geometry.size(), MAX_HEIGHT);
            if (size == source.geometry.size())
                SKIP("the screen is no larger than 720p, so nothing is scaled");

            struct Way { const char *name; bool keepOnCard; bool rgbOnCard; };
            const std::array ways = {
                Way{ "read back and scaled in software", false, false },
                Way{ "kept on the card for an encoder that takes NV12 and the screen's own BGRA", true, true },
                Way{ "kept on the card for an encoder that takes only NV12", true, false },
            };

            const auto encodable = Core::Media::encodableVideoCodecs();
            const bool anEncoderTakesCardFrames = std::any_of(encodable.begin(), encodable.end(), [](VideoCodec codec) {
                return VideoEncoder(settingsFor({ codec, "" }, BITRATE)).takesCardFrames();
            });

            for (const Way &way : ways) {
                if (way.keepOnCard && !anEncoderTakesCardFrames)
                    continue;
                ScreenCapture::Request request;
                request.sourceId = source.id;
                request.method = method;
                request.maxHeight = MAX_HEIGHT;
                request.framerate = FRAMERATE;
                request.softwareFormat = AV_PIX_FMT_NV12;
                request.keepOnCard = way.keepOnCard;
                request.rgbOnCard = way.rgbOnCard;
                ScreenCapture capture;
                capture.configure(request);

                AVFrame *picture = av_frame_alloc();
                int read = 0;
                int onCard = 0;
                int nv12 = 0;
                const auto start = std::chrono::steady_clock::now();
                while (read < PICTURES && std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() < 10000) {
                    const ScreenCapture::Status status = capture.read(picture);
                    REQUIRE(status != ScreenCapture::Status::Failed);
                    REQUIRE(status != ScreenCapture::Status::Closed);
                    if (status != ScreenCapture::Status::Frame)
                        continue;
                    REQUIRE(FrameSize{ picture->width, picture->height } == size);
                    REQUIRE(static_cast<int>(picture->color_range) == static_cast<int>(AVCOL_RANGE_MPEG));
                    REQUIRE(static_cast<int>(picture->colorspace) == static_cast<int>(AVCOL_SPC_BT709));
                    onCard += picture->hw_frames_ctx ? 1 : 0;
                    nv12 += (picture->hw_frames_ctx ? formatOnCard(picture) : static_cast<AVPixelFormat>(picture->format)) == AV_PIX_FMT_NV12 ? 1 : 0;
                    av_frame_unref(picture);
                    read++;
                }
                av_frame_free(&picture);
                REQUIRE(read == PICTURES);

                const bool scaledByTheSource = method == CaptureMethod::GraphicsCapture && way.rgbOnCard;
                REQUIRE(onCard == (way.keepOnCard && (scaledByTheSource || CardScaler::usable()) ? read : 0));
                REQUIRE(nv12 == (scaledByTheSource ? 0 : read));
            }
        }
    }
}

TEST_CASE("A screen that is not there fails", "[video][encode]")
{
    using namespace Core::Video;
    const auto encodable = Core::Media::encodableVideoCodecs();
    if (encodable.empty())
        SKIP("this machine has no encoder");

    BroadcastPipeline pipeline;
    PipelineRecord record(pipeline);

    BroadcastSettings settings;
    settings.sourceId = "screen:99";
    settings.codec = encodable.front();
    settings.bitrate = 1'000'000;
    pipeline.start(settings);

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    if (screenCaptureAvailable()) {
        std::lock_guard lock(record.mutex);
        REQUIRE(record.failures.empty());
    }
    REQUIRE(tryUntil([&] {
        std::lock_guard lock(record.mutex);
        return record.failures.size() == 1;
    }, ScreenCapture::GIVE_UP_AFTER_MS + 2000));
    {
        std::lock_guard lock(record.mutex);
        REQUIRE_FALSE(record.failures.front().empty());
        REQUIRE(record.frames.empty());
        REQUIRE(record.starts.empty());
    }
    pipeline.stop();
}

TEST_CASE("A PipeWire stream that is not there fails", "[video][encode]")
{
    SKIP("Linux only (PipeWire/DBus)");
}

TEST_CASE("Pictures the desktop leaves on the card are shared from there", "[video][encode]")
{
    SKIP("Linux only (PipeWire/DBus)");
}

TEST_CASE("The real screen is shared on the card and in memory", "[video][encode]")
{
    SKIP("Linux only (PipeWire/DBus)");
}

TEST_CASE("The capture method is remembered", "[video][capture]")
{
    using namespace Core::Video;
    const std::string key = "video/capture_method";
    const std::string before = Platform::Settings::instance().getString(key, "auto");
    struct ScopeExit {
        std::string before;
        ~ScopeExit() { Platform::Settings::instance().setString("video/capture_method", before); }
    } restore{before};

    setCaptureMethod(CaptureMethod::Auto);
    REQUIRE(captureMethod() == CaptureMethod::Auto);

    setCaptureMethod(CaptureMethod::GraphicsCapture);
    REQUIRE(captureMethod() == CaptureMethod::GraphicsCapture);
    REQUIRE(Platform::Settings::instance().getString(key, "") == "graphics_capture");
    setCaptureMethod(CaptureMethod::DesktopDuplication);
    REQUIRE(captureMethod() == CaptureMethod::DesktopDuplication);
    REQUIRE(Platform::Settings::instance().getString(key, "") == "desktop_duplication");
    setCaptureMethod(CaptureMethod::Portal);
    REQUIRE(captureMethod() == CaptureMethod::Portal);
    REQUIRE(Platform::Settings::instance().getString(key, "") == "portal");
    setCaptureMethod(CaptureMethod::X11);
    REQUIRE(captureMethod() == CaptureMethod::X11);
    REQUIRE(Platform::Settings::instance().getString(key, "") == "x11");
    setCaptureMethod(CaptureMethod::Auto);
    REQUIRE(captureMethod() == CaptureMethod::Auto);
    REQUIRE(Platform::Settings::instance().getString(key, "") == "auto");

    // Anything else, and nothing at all, is the default.
    Platform::Settings::instance().setString(key, "something else");
    REQUIRE(captureMethod() == CaptureMethod::Auto);
    Platform::Settings::instance().setString(key, "");
    REQUIRE(captureMethod() == CaptureMethod::Auto);
}

#ifdef _WIN32
TEST_CASE("Windows are listed without this program's own", "[video][capture]")
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    using namespace Core::Video;
    std::unique_ptr<TestWindow> own;
    if (const std::optional<QuietScreen> quiet = quietScreen()) {
        own = std::make_unique<TestWindow>(TestWindow::Look{ FrameSize{320, 240}, quiet->cornerFor(FrameSize{400, 340}) });
        REQUIRE(own->isOpen());
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    const auto windows = captureWindows();
    if (!windowCaptureAvailable()) {
        REQUIRE(windows.empty());
        SKIP("this build or this Windows cannot capture a window");
    }

    std::vector<std::string> ids;
    for (const CaptureSource &window : windows) {
        REQUIRE(window.kind == CaptureSource::Kind::Window);
        REQUIRE(window.id.starts_with("window:"));
        REQUIRE(std::find(ids.begin(), ids.end(), window.id) == ids.end());
        ids.push_back(window.id);
        REQUIRE_FALSE(window.name.empty());
        REQUIRE(window.processId != 0);
        REQUIRE(window.processId != GetCurrentProcessId());

        bool numbered = false;
        try {
            const HWND handle = reinterpret_cast<HWND>(static_cast<uintptr_t>(std::stoull(window.id.substr(7))));
            numbered = true;
            if (IsWindow(handle) && !IsIconic(handle) && IsWindowVisible(handle))
                REQUIRE((window.geometry.width > 0 && window.geometry.height > 0));
        } catch (...) {}
        REQUIRE(numbered);
    }
    if (own)
        REQUIRE(std::find(ids.begin(), ids.end(), own->sourceId()) == ids.end());
}

TEST_CASE("A window has a thumbnail", "[video][capture]")
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    using namespace Core::Video;
    SKIP_WITHOUT_A_QUIET_SCREEN(quiet);
    for (bool scaledByWindows : {false, true}) {
        DYNAMIC_SECTION((scaledByWindows ? "a window Windows scales" : "a window that measures in the screen's pixels")) {
            const TestWindow window(TestWindow::Look{ FrameSize{480, 360}, quiet->cornerFor(FrameSize{560, 460}), SHARED_COLOUR, true, scaledByWindows });
            REQUIRE(window.isOpen());
            std::this_thread::sleep_for(std::chrono::milliseconds(300));

            CaptureSource source;
            source.id = window.sourceId();
            source.kind = CaptureSource::Kind::Window;
            const FrameSize bound{224, 126};
            const Platform::ImageBuffer thumbnail = captureThumbnail(source, bound);
            REQUIRE_FALSE(thumbnail.empty());
            REQUIRE(thumbnail.width <= bound.width);
            REQUIRE(thumbnail.height <= bound.height);
            REQUIRE((thumbnail.width == bound.width || thumbnail.height == bound.height));

            // The whole window as it shows, so its inside is in the middle and at the bottom, and its
            // title bar, which is another colour, at the top.
            const auto isTheWindowsColour = [&thumbnail](int x, int y) {
                const size_t index = (y * thumbnail.width + x) * 4;
                const int r = thumbnail.pixels[index];
                const int g = thumbnail.pixels[index + 1];
                const int b = thumbnail.pixels[index + 2];
                return std::abs(r - PATCH_RED) < 12 && std::abs(g - PATCH_GREEN) < 12 && std::abs(b - PATCH_BLUE) < 12;
            };
            REQUIRE(isTheWindowsColour(thumbnail.width / 2, thumbnail.height / 2));
            REQUIRE(isTheWindowsColour(thumbnail.width / 2, thumbnail.height - 4));
            REQUIRE(isTheWindowsColour(thumbnail.width - 4, thumbnail.height / 2));
            REQUIRE_FALSE(isTheWindowsColour(thumbnail.width / 2, 2));

            source.id = "window:1";
            REQUIRE(captureThumbnail(source, bound).empty());
        }
    }
}

TEST_CASE("The pipeline shares a window", "[video][encode]")
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
    using namespace Core::Video;
    const auto encodable = Core::Media::encodableVideoCodecs();
    if (encodable.empty())
        SKIP("this machine has no encoder");
    if (!windowCaptureAvailable())
        SKIP("this build or this Windows cannot capture a window");

    struct Config {
        CaptureMethod method;
        bool scaledByWindows;
        int maxHeight;
        const char *name;
    };
    const std::array configs = {
        Config{ CaptureMethod::GraphicsCapture, false, 0, "by graphics capture" },
        Config{ CaptureMethod::GraphicsCapture, true, 360, "by graphics capture, a window Windows scales, scaled down" },
        Config{ CaptureMethod::DesktopDuplication, false, 0, "by duplication" },
        Config{ CaptureMethod::DesktopDuplication, true, 360, "by duplication, a window Windows scales, scaled down" },
    };

    for (const auto &cfg : configs) {
        DYNAMIC_SECTION(cfg.name) {
            if (!captureMethodUsable(cfg.method))
                SKIP("this build or this Windows cannot capture a window this way");

            const VideoCodec codec = std::find(encodable.begin(), encodable.end(), VideoCodec::H264) != encodable.end() ? VideoCodec::H264 : encodable.front();
            const bool duplicated = cfg.method == CaptureMethod::DesktopDuplication;

            const auto countsOverASecond = [](PipelineRecord &record, PathCounts *earlier, PathCounts *later) {
                for (PathCounts *counts : { earlier, later }) {
                    size_t seen = 0;
                    {
                        std::lock_guard lock(record.mutex);
                        seen = record.stats.size();
                    }
                    if (!tryUntil([&] {
                        std::lock_guard lock(record.mutex);
                        return record.stats.size() > seen;
                    }, 2500))
                        return false;
                    std::lock_guard lock(record.mutex);
                    *counts = pathCounts(record.stats.back());
                }
                return true;
            };

            SKIP_WITHOUT_A_QUIET_SCREEN(quiet);
            const POINT home = quiet->cornerFor(FrameSize{ 860, 700 });
            TestWindow window(TestWindow::Look{ FrameSize{ 640, 480 }, home, SHARED_COLOUR, true, cfg.scaledByWindows });
            REQUIRE(window.isOpen());
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const FrameSize firstSize = encodeSizeFor(window.insideOnScreen().size(), cfg.maxHeight);
            REQUIRE((firstSize != window.insideOnScreen().size()) == (cfg.maxHeight != 0));

            setCaptureMethod(cfg.method);
            BroadcastPipeline pipeline;
            PipelineRecord record(pipeline);
            BroadcastSettings settings;
            settings.sourceId = window.sourceId();
            settings.maxHeight = cfg.maxHeight;
            settings.framerate = FRAMERATE;
            settings.codec = codec;
            settings.bitrate = 2'500'000;
            settings.maxBitrate = 4'000'000;
            pipeline.start(settings);

            REQUIRE(tryUntil([&] {
                std::lock_guard lock(record.mutex);
                return record.frames.size() >= 20 || !record.failures.empty() || record.closes > 0;
            }, 4000));
            {
                std::lock_guard lock(record.mutex);
                REQUIRE(record.failures.empty());
                REQUIRE(record.closes == 0);
                REQUIRE(record.starts.size() == 1);
                REQUIRE(record.starts.front() == firstSize);
                REQUIRE(record.frames[0]->keyframe);
                REQUIRE_FALSE(record.frames[1]->keyframe);
                for (size_t i = 1; i < 20; i++)
                    REQUIRE(static_cast<int32_t>(record.frames[i]->rtpTimestamp - record.frames[i - 1]->rtpTimestamp) > 0);
                const double ticksPerFrame = static_cast<double>(record.frames[19]->rtpTimestamp - record.frames[4]->rtpTimestamp) / 15.0;
                REQUIRE(ticksPerFrame > TICKS_PER_FRAME * 0.8);
            }

            REQUIRE(tryUntil([&] {
                std::lock_guard lock(record.mutex);
                return !record.stats.empty();
            }, 2500));
            {
                std::lock_guard lock(record.mutex);
                if (duplicated)
                    REQUIRE(pathCounts(record.stats.back()).cutOut > 0);
                else
                    REQUIRE(record.stats.back().encoderName.ends_with(", from gfxcapture"));
                const bool onCard = reachesTheEncoderOnTheCard(codec, cfg.maxHeight != 0, !duplicated);
                REQUIRE((record.stats.back().encoderName.find(onDevice(cardDevice())) != std::string::npos) == onCard);
            }

            // Window that has gone still
            window.setAnimated(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            size_t before = 0;
            {
                std::lock_guard lock(record.mutex);
                before = record.frames.size();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            size_t whileStill = 0;
            {
                std::lock_guard lock(record.mutex);
                whileStill = record.frames.size() - before;
            }
            REQUIRE(whileStill >= 3);
            REQUIRE(whileStill <= static_cast<size_t>(FRAMERATE * 2 * 1.25));

            // Off the screen
            window.moveTo(POINT{ -6000, -6000 });
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            {
                std::lock_guard lock(record.mutex);
                before = record.frames.size();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            size_t whileAway = 0;
            {
                std::lock_guard lock(record.mutex);
                whileAway = record.frames.size() - before;
            }
            REQUIRE(whileAway >= 3);
            REQUIRE(whileAway <= static_cast<size_t>(FRAMERATE * 2 * 1.25));
            window.moveTo(home);

            // Cloaked
            window.setCloaked(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
            {
                std::lock_guard lock(record.mutex);
                before = record.frames.size();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            size_t whileCloaked = 0;
            {
                std::lock_guard lock(record.mutex);
                whileCloaked = record.frames.size() - before;
            }
            REQUIRE(whileCloaked >= 3);
            REQUIRE(whileCloaked <= static_cast<size_t>(FRAMERATE * 2 * 1.25));

            size_t atRequest = 0;
            int64_t askedAt = 0;
            {
                std::lock_guard lock(record.mutex);
                atRequest = record.frames.size();
                askedAt = record.elapsedMs();
            }
            pipeline.requestKeyframe();
            REQUIRE(tryUntil([&] {
                std::lock_guard lock(record.mutex);
                return record.keyFrameFrom(atRequest) < record.frames.size();
            }, 1500));
            {
                std::lock_guard lock(record.mutex);
                const int64_t waited = record.arrivedAt[record.keyFrameFrom(atRequest)] - askedAt;
                REQUIRE(waited < 400);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            {
                std::lock_guard lock(record.mutex);
                REQUIRE(record.keyFrameFrom(record.keyFrameFrom(atRequest) + 1) == record.frames.size());
            }
            window.setCloaked(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            {
                std::lock_guard lock(record.mutex);
                REQUIRE(record.starts.size() == 1);
                REQUIRE(record.stalls.empty());
            }

            // Resized
            window.setAnimated(true);
            size_t atResize = 0;
            {
                std::lock_guard lock(record.mutex);
                atResize = record.frames.size();
            }
            window.resizeInside(cfg.maxHeight ? FrameSize{ 800, 400 } : FrameSize{ 800, 600 });
            const FrameSize secondSize = encodeSizeFor(window.insideOnScreen().size(), cfg.maxHeight);
            REQUIRE(secondSize != firstSize);
            REQUIRE(tryUntil([&] {
                std::lock_guard lock(record.mutex);
                return record.starts.size() == 2;
            }, 4000));
            {
                std::lock_guard lock(record.mutex);
                REQUIRE(record.starts.back() == secondSize);
            }
            REQUIRE(tryUntil([&] {
                std::lock_guard lock(record.mutex);
                return record.frames.size() >= atResize + 20;
            }, 3000));
            {
                std::lock_guard lock(record.mutex);
                REQUIRE(record.keyFrameFrom(atResize) < record.frames.size());
            }

            // Covered
            const CaptureRect inside = window.insideOnScreen();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            size_t atCover = 0;
            {
                std::lock_guard lock(record.mutex);
                atCover = record.frames.size();
            }
            size_t coveredUntil = 0;
            {
                const TestWindow cover(TestWindow::Look{ FrameSize{ inside.width / 3, inside.height / 3 }, POINT{ inside.center().x - inside.width / 6, inside.center().y - inside.height / 6 }, COVERING_COLOUR, false });
                REQUIRE(cover.isOpen());
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                PathCounts earlier;
                PathCounts later;
                REQUIRE(countsOverASecond(record, &earlier, &later));
                if (duplicated) {
                    REQUIRE(later.drawn > earlier.drawn);
                    REQUIRE(later.cutOut == earlier.cutOut);
                }
                std::lock_guard lock(record.mutex);
                coveredUntil = record.frames.size();
            }
            REQUIRE(coveredUntil >= atCover + 20);
            {
                std::lock_guard lock(record.mutex);
                REQUIRE(record.keyFrameFrom(atCover) >= coveredUntil);
                REQUIRE(record.starts.size() == 2);
            }
            const Decoded covered = decodeAll(codec, record.framesBefore(coveredUntil));
            REQUIRE(covered.errors == 0);
            REQUIRE(FrameSize{ covered.width, covered.height } == secondSize);
            REQUIRE(looksLike(covered.middle, SHARED_COLOUR));
            REQUIRE_FALSE(looksLike(covered.middle, COVERING_COLOUR));
            REQUIRE(looksLike(covered.top, SHARED_COLOUR));
            REQUIRE(offItsOwnCoefficients(covered, covered.middle, SHARED_COLOUR) < COLOUR_SURVIVES);

            if (duplicated) {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                PathCounts earlier;
                PathCounts later;
                REQUIRE(countsOverASecond(record, &earlier, &later));
                REQUIRE(later.cutOut - earlier.cutOut >= FRAMERATE / 2);
                REQUIRE(later.drawn - earlier.drawn <= FRAMERATE / 6);
            }

            if (duplicated && window.reachPastAnOuterEdge(quiet->whole, 10)) {
                PathCounts earlier;
                PathCounts later;
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                REQUIRE(countsOverASecond(record, &earlier, &later));
                REQUIRE(later.cutOut - earlier.cutOut >= FRAMERATE / 2);
                REQUIRE(later.drawn - earlier.drawn <= FRAMERATE / 6);
                REQUIRE(window.reachPastAnOuterEdge(quiet->whole, 40));
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                REQUIRE(countsOverASecond(record, &earlier, &later));
                REQUIRE(later.drawn > earlier.drawn);
                REQUIRE(later.cutOut == earlier.cutOut);
                window.moveTo(home);
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                std::lock_guard lock(record.mutex);
                REQUIRE(record.starts.size() == 2);
            }

            std::vector<bool> expectedStalls;
            for (int pause = 0; pause < (duplicated ? 2 : 1); pause++) {
                if (pause == 0)
                    window.hide();
                else if (!window.keepOutOfCaptures(true))
                    break;
                expectedStalls.push_back(true);
                REQUIRE(tryUntil([&] {
                    std::lock_guard lock(record.mutex);
                    return record.stalls == expectedStalls;
                }, 2000));
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                size_t whilePaused = 0;
                {
                    std::lock_guard lock(record.mutex);
                    whilePaused = record.frames.size();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(700));
                {
                    std::lock_guard lock(record.mutex);
                    REQUIRE(record.frames.size() == whilePaused);
                }

                if (pause == 0)
                    window.show();
                else
                    window.keepOutOfCaptures(false);
                expectedStalls.push_back(false);
                REQUIRE(tryUntil([&] {
                    std::lock_guard lock(record.mutex);
                    return record.stalls == expectedStalls;
                }, 3000));
                REQUIRE(tryUntil([&] {
                    std::lock_guard lock(record.mutex);
                    return record.frames.size() >= whilePaused + 10;
                }, 3000));
                std::lock_guard lock(record.mutex);
                REQUIRE(record.starts.size() == 2);
            }

            window.close();
            REQUIRE(tryUntil([&] {
                std::lock_guard lock(record.mutex);
                return record.closes == 1;
            }, 3000));
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            size_t afterClose = 0;
            {
                std::lock_guard lock(record.mutex);
                afterClose = record.frames.size();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            {
                std::lock_guard lock(record.mutex);
                REQUIRE(record.frames.size() == afterClose);
                REQUIRE(record.closes == 1);
                REQUIRE(record.failures.empty());
                REQUIRE(record.stalls == expectedStalls);
                for (const auto &f : record.frames)
                    REQUIRE(isWellFormed(codec, *f));
            }

            std::vector<EncodedFramePointer> allFrames;
            {
                std::lock_guard lock(record.mutex);
                allFrames = record.frames;
            }
            std::mutex blackMutex;
            std::vector<std::string> blackPictures;
            const Decoded decoded = decodeAll(codec, allFrames, [&](const AVFrame *pic, int idx) {
                if (pic->data[0][(pic->height / 2) * pic->linesize[0] + pic->width / 2] >= BLACK_AT_MOST)
                    return;
                std::lock_guard bLock(blackMutex);
                blackPictures.push_back("picture " + std::to_string(idx));
            });
            if (!duplicated)
                REQUIRE(blackPictures.empty());
            REQUIRE(decoded.errors == 0);
            REQUIRE(decoded.pictures == static_cast<int>(allFrames.size()));
            REQUIRE(FrameSize{ decoded.width, decoded.height } == secondSize);
            REQUIRE(looksLike(decoded.middle, SHARED_COLOUR));
            REQUIRE(looksLike(decoded.top, SHARED_COLOUR));
            REQUIRE(offItsOwnCoefficients(decoded, decoded.middle, SHARED_COLOUR) < COLOUR_SURVIVES);
            {
                std::lock_guard lock(record.mutex);
                const bool onCard = reachesTheEncoderOnTheCard(codec, cfg.maxHeight != 0, !duplicated);
                REQUIRE((record.stats.back().encoderName.find(onDevice(cardDevice())) != std::string::npos) == onCard);
            }
            pipeline.stop();
        }
    }
}

TEST_CASE("The pipeline holds back what its encoder would pad", "[video][encode]")
{
    SKIP_UNLESS_HARDWARE_IS_ASKED_FOR();
#ifdef _WIN32
    using namespace Core::Video;
    using Core::Media::codedSize;
    const std::string encoderName = Core::Media::videoEncoderFor(VideoCodec::AV1);
    const FrameSize padded{ 700, 500 };
    const FrameSize exact{ 640, 480 };
    if (encoderName.empty() || codedSize(encoderName, padded) == padded)
        SKIP("this machine's AV1 encoder, if it has one, codes every size as it is");
    if (!windowCaptureAvailable())
        SKIP("this build or this Windows cannot capture a window");
    REQUIRE_FALSE(Core::Media::paddingIsAcceptable(padded, codedSize(encoderName, padded)));
    REQUIRE(codedSize(encoderName, exact) == exact);

    SKIP_WITHOUT_A_QUIET_SCREEN(quiet);
    TestWindow window(TestWindow::Look{ padded, quiet->cornerFor(FrameSize{ 860, 700 }) });
    REQUIRE(window.isOpen());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    REQUIRE(window.insideOnScreen().size() == padded);

    CaptureSource source;
    source.id = window.sourceId();
    source.kind = CaptureSource::Kind::Window;
    REQUIRE(encodeSizeOf(source, 0) == padded);
    REQUIRE(encodeSizeOf(source, 360) == encodeSizeFor(padded, 360));

    BroadcastPipeline pipeline;
    PipelineRecord record(pipeline);
    BroadcastSettings settings;
    settings.sourceId = source.id;
    settings.maxHeight = 0;
    settings.framerate = FRAMERATE;
    settings.codec = VideoCodec::AV1;
    settings.bitrate = 2'500'000;
    settings.maxBitrate = 4'000'000;
    pipeline.start(settings);

    REQUIRE(tryUntil([&] {
        std::lock_guard lock(record.mutex);
        return !record.starts.empty() || !record.failures.empty();
    }, 4000));
    {
        std::lock_guard lock(record.mutex);
        REQUIRE(record.failures.empty());
        REQUIRE(record.starts.front() == padded);
    }
    REQUIRE(tryUntil([&] {
        std::lock_guard lock(record.mutex);
        return !record.stats.empty();
    }, 2500));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    {
        std::lock_guard lock(record.mutex);
        REQUIRE(record.frames.empty());
        REQUIRE(record.stats.back().encoderName.starts_with("none while AV1"));
        REQUIRE(record.stats.back().capturedFps > 0);
        REQUIRE(record.stats.back().framesEncoded == 0);
    }

    window.resizeInside(exact);
    REQUIRE(tryUntil([&] {
        std::lock_guard lock(record.mutex);
        return record.starts.size() == 2;
    }, 4000));
    {
        std::lock_guard lock(record.mutex);
        REQUIRE(record.starts.back() == exact);
    }
    REQUIRE(tryUntil([&] {
        std::lock_guard lock(record.mutex);
        return record.frames.size() >= 10;
    }, 3000));
    {
        std::lock_guard lock(record.mutex);
        REQUIRE(record.frames[0]->keyframe);
    }
    pipeline.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::vector<EncodedFramePointer> snapshot;
    {
        std::lock_guard lock(record.mutex);
        snapshot = record.frames;
    }
    const Decoded decoded = decodeAll(VideoCodec::AV1, snapshot);
    REQUIRE(decoded.errors == 0);
    REQUIRE(FrameSize{ decoded.width, decoded.height } == exact);
    {
        std::lock_guard lock(record.mutex);
        REQUIRE(record.failures.empty());
    }
#else
    SKIP("Linux only");
#endif
}
#endif
