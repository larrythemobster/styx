// Ported from ouwou/Acheron video-streams commit 71b4907 (GPL-3.0-or-later).
#include "Core/Video/CameraCapture.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <utility>

#if defined(STYX_HAVE_AVDEVICE) && (defined(_WIN32) || defined(__linux__))
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_desc.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
}
#ifdef __linux__
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif
#endif

namespace Styx::Core::Video {
namespace {
std::mutex factoryMutex;
CameraFrameSourceFactory replacementFactory;

#if defined(STYX_HAVE_AVDEVICE) && (defined(_WIN32) || defined(__linux__))
using Clock = std::chrono::steady_clock;
#ifdef _WIN32
constexpr char DEVICE[] = "dshow";
constexpr char URL_BEFORE_ID[] = "video=";
#else
constexpr char DEVICE[] = "v4l2";
constexpr char URL_BEFORE_ID[] = "";
#endif
struct Mode { const char *size; const char *framerate; AVCodecID codec; };
constexpr std::array modes{Mode{"1280x720", "30", AV_CODEC_ID_MJPEG}, Mode{"1280x720", "30", AV_CODEC_ID_NONE}, Mode{"640x480", "30", AV_CODEC_ID_NONE}, Mode{nullptr, nullptr, AV_CODEC_ID_NONE}};
constexpr int DEVICE_BUFFER_BYTES = 2 * 1280 * 720 * 2;
constexpr auto POLL_INTERVAL = std::chrono::milliseconds(1);
struct InputCloser { void operator()(AVFormatContext *input) const { avformat_close_input(&input); } };
struct DecoderDeleter { void operator()(AVCodecContext *decoder) const { avcodec_free_context(&decoder); } };
struct PacketDeleter { void operator()(AVPacket *packet) const { av_packet_free(&packet); } };
struct FrameDeleter { void operator()(AVFrame *frame) const { av_frame_free(&frame); } };
struct OpenCamera { std::unique_ptr<AVFormatContext, InputCloser> input; std::unique_ptr<AVCodecContext, DecoderDeleter> decoder; const char *sizeAskedFor = nullptr; };
const AVInputFormat *inputDevice() { static const AVInputFormat *device = [] { avdevice_register_all(); return av_find_input_format(DEVICE); }(); return device; }
AVFormatContext *openInput(const std::string &url, const Mode &mode) {
    AVFormatContext *input = avformat_alloc_context();
    if (!input) return nullptr;
    input->video_codec_id = mode.codec; input->flags |= AVFMT_FLAG_NONBLOCK;
    AVDictionary *options = nullptr;
    if (mode.size) { av_dict_set(&options, "video_size", mode.size, 0); av_dict_set(&options, "framerate", mode.framerate, 0); }
    av_dict_set_int(&options, "rtbufsize", DEVICE_BUFFER_BYTES, 0);
    avformat_open_input(&input, url.c_str(), inputDevice(), &options); av_dict_free(&options);
    return input;
}
AVCodecContext *openDecoder(const AVStream *stream) {
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext *decoder = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!decoder) return nullptr;
    decoder->thread_count = 1; decoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (avcodec_parameters_to_context(decoder, stream->codecpar) < 0 || avcodec_open2(decoder, codec, nullptr) < 0) avcodec_free_context(&decoder);
    return decoder;
}
OpenCamera openCamera(const std::string &url) {
    for (const auto &mode : modes) {
        OpenCamera camera; camera.input.reset(openInput(url, mode));
        if (!camera.input || camera.input->nb_streams == 0 || camera.input->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
#ifdef __linux__
        const auto *opened = camera.input->streams[0]->codecpar;
        if (mode.size && std::string(mode.size) != std::to_string(opened->width) + "x" + std::to_string(opened->height)) continue;
#endif
        camera.sizeAskedFor = mode.size; camera.decoder.reset(openDecoder(camera.input->streams[0])); if (camera.decoder) return camera;
    } return {};
}
std::string describe(const AVStream *stream) {
    const auto *sent = stream->codecpar;
    const char *format = sent->codec_id == AV_CODEC_ID_RAWVIDEO ? av_get_pix_fmt_name(static_cast<AVPixelFormat>(sent->format)) : avcodec_get_name(sent->codec_id);
    return std::string(DEVICE) + " " + (format ? format : "unknown") + " " + std::to_string(sent->width) + "x" + std::to_string(sent->height) + " at " + std::to_string(av_q2d(stream->avg_frame_rate)) + " fps";
}
#ifdef __linux__
bool copyOutOfDriver(AVPacket *packet) { AVBufferRef *copy = av_buffer_alloc(packet->size + AV_INPUT_BUFFER_PADDING_SIZE); if (!copy) return false; std::copy_n(packet->data, packet->size, copy->data); std::fill_n(copy->data + packet->size, AV_INPUT_BUFFER_PADDING_SIZE, 0); av_buffer_unref(&packet->buf); packet->buf = copy; packet->data = copy->data; return true; }
#endif
#endif
} // namespace

std::chrono::steady_clock::time_point CaptureClock::at(std::optional<std::chrono::microseconds> stamped, std::chrono::steady_clock::time_point readAt) {
    using Clock = std::chrono::steady_clock; Clock::time_point taken = readAt;
    if (stamped) { const auto ahead = readAt.time_since_epoch() - *stamped; const auto followed = ourClockAhead ? *ourClockAhead + (readAt - lastReadAt) / SLOWEST_CLOCK_FOLLOWED : ahead; ourClockAhead = ahead - followed > STARTED_OVER ? ahead : std::min(ahead, followed); taken = Clock::time_point(std::chrono::duration_cast<Clock::duration>(*stamped + *ourClockAhead)); }
    lastReadAt = readAt; last = last ? std::max(*last + std::chrono::microseconds(1), taken) : taken; return *last;
}
void setCameraFrameSourceFactory(CameraFrameSourceFactory factory) { std::lock_guard lock(factoryMutex); replacementFactory = std::move(factory); }
std::unique_ptr<CameraFrameSource> makeCameraFrameSource() { std::lock_guard lock(factoryMutex); if (replacementFactory) return replacementFactory();
#if defined(STYX_HAVE_AVDEVICE) && (defined(_WIN32) || defined(__linux__))
    return std::make_unique<CameraCapture>();
#else
    return nullptr;
#endif
}
#if defined(STYX_HAVE_AVDEVICE) && (defined(_WIN32) || defined(__linux__))
bool cameraCaptureAvailable() { return inputDevice() != nullptr; }
std::vector<CameraDevice> cameraDevices() {
    std::vector<CameraDevice> cameras; if (!inputDevice()) return cameras;
#ifdef _WIN32
    AVDeviceInfoList *found = nullptr; if (avdevice_list_input_sources(inputDevice(), nullptr, nullptr, &found) < 0) return cameras;
    for (int i = 0; i < found->nb_devices; ++i) { const auto *device = found->devices[i]; if (std::find(device->media_types, device->media_types + device->nb_media_types, AVMEDIA_TYPE_VIDEO) != device->media_types + device->nb_media_types) cameras.push_back({device->device_name, device->device_description}); } avdevice_free_list_devices(&found);
#else
    std::vector<std::pair<std::string, std::string>> persistent; std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator("/dev/v4l/by-id", std::filesystem::directory_options::skip_permission_denied, error)) { const auto canonical = std::filesystem::canonical(entry.path(), error); if (!error) persistent.emplace_back(canonical.string(), entry.path().string()); error.clear(); }
    for (const auto &entry : std::filesystem::directory_iterator("/dev", std::filesystem::directory_options::skip_permission_denied, error)) { const auto path = entry.path().string(); if (entry.path().filename().string().rfind("video", 0) != 0) continue; v4l2_capability device{}; const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC); if (fd < 0) continue; const bool answered = ioctl(fd, VIDIOC_QUERYCAP, &device) == 0; close(fd); const auto own = device.capabilities & V4L2_CAP_DEVICE_CAPS ? device.device_caps : device.capabilities; if (!answered || !(own & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE)) || !(own & V4L2_CAP_STREAMING)) continue; const auto it = std::find_if(persistent.begin(), persistent.end(), [&path](const auto &p) { return p.first == path; }); cameras.push_back({it == persistent.end() ? path : it->second, reinterpret_cast<const char *>(device.card)}); } 
#endif
    return cameras;
}
CameraCapture::CameraCapture() : newest(av_frame_alloc()) {}
CameraCapture::~CameraCapture() { stopping = true; if (thread.joinable()) thread.join(); av_frame_free(&newest); }
bool CameraCapture::open(const std::string &deviceId) { thread = std::thread([this, deviceId] { run(deviceId); }); std::unique_lock lock(mutex); changed.wait(lock, [this] { return state != State::Opening; }); return state == State::Open; }
CameraFrameSource::Status CameraCapture::read(AVFrame *into, std::chrono::steady_clock::time_point &capturedAt) { std::unique_lock lock(mutex); changed.wait_for(lock, std::chrono::milliseconds(READ_WAIT_MS), [this] { return newest->buf[0] || state == State::Ended; }); if (newest->buf[0]) { av_frame_move_ref(into, newest); capturedAt = newestCapturedAt; return Status::Frame; } return state == State::Ended ? Status::Ended : Status::NoFrame; }
std::string CameraCapture::failure() const { std::lock_guard lock(mutex); return failureText; }
std::string CameraCapture::description() const { std::lock_guard lock(mutex); return mode; }
void CameraCapture::opened(std::string openedMode) { { std::lock_guard lock(mutex); mode = std::move(openedMode); state = State::Open; } changed.notify_all(); }
void CameraCapture::end(std::string reason) { { std::lock_guard lock(mutex); failureText = std::move(reason); state = State::Ended; } changed.notify_all(); }
void CameraCapture::publish(AVFrame *picture, std::chrono::steady_clock::time_point capturedAt) { if (picture->colorspace == AVCOL_SPC_UNSPECIFIED) picture->colorspace = AVCOL_SPC_SMPTE170M; { std::lock_guard lock(mutex); av_frame_unref(newest); av_frame_move_ref(newest, picture); newestCapturedAt = capturedAt; } changed.notify_all(); }
void CameraCapture::run(std::string deviceId) { const auto cameras = cameraDevices(); if (deviceId.empty() && cameras.empty()) { end("No camera is connected."); return; } const auto id = deviceId.empty() ? cameras.front().id : deviceId; const auto listed = std::find_if(cameras.begin(), cameras.end(), [&id](const auto &each) { return each.id == id; }); const auto camera = openCamera(std::string(URL_BEFORE_ID) + id); if (!camera.input) { end(listed == cameras.end() ? "The camera is not connected." : "The camera could not be opened. Another program may be using it."); return; } const AVStream *stream = camera.input->streams[0]; opened(describe(stream)); const std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc()), newer(av_packet_alloc()); const std::unique_ptr<AVFrame, FrameDeleter> decoded(av_frame_alloc()); const auto *codec = avcodec_descriptor_get(camera.decoder->codec_id); const bool intra = codec && (codec->props & AV_CODEC_PROP_INTRA_ONLY); CaptureClock clock; auto sincePicture = Clock::now(); while (!stopping) { const int got = av_read_frame(camera.input.get(), packet.get()); if (got == AVERROR(EAGAIN)) { if (Clock::now() - sincePicture >= std::chrono::milliseconds(NO_PICTURE_TIMEOUT_MS)) { end("The camera is not sending pictures. Another program may be using it."); return; } std::this_thread::sleep_for(POLL_INTERVAL); continue; } if (got < 0) { end("The camera was disconnected."); return; } sincePicture = Clock::now(); const auto readAt = Clock::now(); while (intra && av_read_frame(camera.input.get(), newer.get()) >= 0) { av_packet_unref(packet.get()); av_packet_move_ref(packet.get(), newer.get()); }
#ifdef __linux__
if (!copyOutOfDriver(packet.get())) { av_packet_unref(packet.get()); continue; }
#endif
const int sent = avcodec_send_packet(camera.decoder.get(), packet.get()); av_packet_unref(packet.get()); while (sent >= 0 && avcodec_receive_frame(camera.decoder.get(), decoded.get()) >= 0) { std::optional<std::chrono::microseconds> stamped; if (decoded->pts != AV_NOPTS_VALUE) stamped = std::chrono::microseconds(av_rescale_q(decoded->pts, stream->time_base, AVRational{1, 1'000'000})); publish(decoded.get(), clock.at(stamped, readAt)); } } }
#else
bool cameraCaptureAvailable() { return false; }
std::vector<CameraDevice> cameraDevices() { return {}; }
#endif
} // namespace Styx::Core::Video
