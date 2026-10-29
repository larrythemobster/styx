#include "Core/Media/Player.hpp"

#include "Core/Logging.hpp"
#include "Discord/CdnUrls.hpp"

#include "Platform/Settings.hpp"
#include "Platform/Timer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include <initializer_list>
#include <utility>

#ifdef STYX_HAVE_FFMPEG

#include "Core/Media/AudioOutput.hpp"
#include "Core/Media/MediaDecoder.hpp"
#include "Core/Media/PacketQueue.hpp"


#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#endif // STYX_HAVE_FFMPEG

namespace Styx {
namespace Core {
namespace Media {

namespace {

constexpr auto VolumeKey = "media/volume";
constexpr auto MutedKey = "media/muted";

bool listContains(const std::string &value, std::initializer_list<const char *> list)
{
    return std::any_of(list.begin(), list.end(), [&](const char *item) { return value == item; });
}

std::string normalizedContentType(const std::string &contentType)
{
    std::string value = contentType.substr(0, contentType.find(';'));
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool hasSuffixIn(const std::string &url, std::initializer_list<const char *> suffixes)
{
    const std::string path = Platform::urlPath(url);
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos || path.find('/', dot) != std::string::npos)
        return false;
    std::string suffix = path.substr(dot + 1);
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return listContains(suffix, suffixes);
}

} // namespace

bool isSupported()
{
#ifdef STYX_HAVE_FFMPEG
    return true;
#else
    return false;
#endif
}

bool canPlay(const std::string &contentType, const std::string &url)
{
    if (!isSupported() || !Discord::Cdn::isDiscordMediaUrl(url))
        return false;

    if (listContains(normalizedContentType(contentType), { "video/mp4", "video/webm", "video/quicktime", "video/x-m4v" }))
        return true;

    return hasSuffixIn(url, { "mp4", "webm", "mov", "m4v" });
}

bool canPlayAudio(const std::string &contentType, const std::string &url)
{
    if (!isSupported() || !Discord::Cdn::isDiscordMediaUrl(url))
        return false;

    if (normalizedContentType(contentType).starts_with("audio/"))
        return true;

    return hasSuffixIn(url, { "mp3", "ogg", "oga", "opus", "wav", "flac", "m4a", "aac" });
}

float storedVolume()
{
    return std::clamp(static_cast<float>(Platform::Settings::instance().getDouble(VolumeKey, 1.0)), 0.0f, 1.0f);
}

void setStoredVolume(float volume)
{
    Platform::Settings::instance().setDouble(VolumeKey, std::clamp(volume, 0.0f, 1.0f));
}

bool storedMuted()
{
    return Platform::Settings::instance().getBool(MutedKey, false);
}

void setStoredMuted(bool muted)
{
    Platform::Settings::instance().setBool(MutedKey, muted);
}

struct OwnerThreadState
{
    Player *owner = nullptr;

    Player::State state = Player::State::Idle;
    int64_t durationMs = 0;
    int64_t positionMs = 0;
    Platform::Size native;
    bool audioAvailable = false;
    bool videoAvailable = false;
    bool seekable = false;
    Platform::ImageBuffer frame;

    float userVolume = storedVolume();
    bool userMuted = storedMuted();

    void setState(Player::State next)
    {
        if (state == next)
            return;
        state = next;
        owner->stateChanged.publish(next);
    }
};

#ifdef STYX_HAVE_FFMPEG

namespace {

constexpr int MaxQueuedFrames = 4;
constexpr int AudioChunkFrames = 2048;

constexpr int64_t MinPresentDelayMs = 1;
constexpr int64_t MaxPresentDelayMs = 40;

constexpr int AudioOnlyPresentDelayMs = 100;

Platform::Size effectiveTarget(const Platform::Size &native, const Platform::Size &want)
{
    if (native.empty() || want.empty())
        return native;
    const auto scaled = Platform::fitWithin(native, want);
    if (scaled.width >= native.width || scaled.height >= native.height || scaled.empty())
        return native;
    return scaled;
}

} // namespace

struct Player::Impl : OwnerThreadState
{
    explicit Impl(Player *player)
    {
        owner = player;
    }

    struct DecodedFrame
    {
        Platform::ImageBuffer image;
        int64_t ptsMs = 0;
    };

    // owner thread
    Platform::Timer presentTimer;
    std::chrono::steady_clock::time_point wallTimer = std::chrono::steady_clock::now();
    int64_t wallBaseMs = 0;
    bool seeking = false;
    bool audioClockStalled = false;
    bool autoPlayOnReady = false;
    std::shared_ptr<void> sessionContext;
    std::thread worker;
    std::thread demuxer;

    // decode thread
    MediaDecoder decoder;
    ProxyConfig proxy;
    std::vector<float> audioScratch;
    std::vector<float> audioPending;
    size_t audioPendingOffset = 0;

    // shared, guarded by mutex, waited on through cv
    std::mutex mutex;
    std::condition_variable cv;
    PacketQueue packets;
    std::deque<DecodedFrame> frames;
    Platform::Size targetSize;

    // shared, lock free
    std::atomic<bool> running{ false };
    std::atomic<bool> paused{ true };
    std::atomic<bool> reachedEnd{ false };
    std::atomic<bool> audioSinkBroken{ false };
    std::atomic<bool> primed{ false };
    std::atomic<int64_t> pendingSeek{ -1 };
    std::atomic<int64_t> seekTargetMs{ -1 };
    std::atomic<bool> awaitingSeekFrame{ false };
    std::atomic<int64_t> demuxedMs{ 0 };
    std::atomic<bool> demuxEof{ false };
    std::atomic<bool> flushPending{ false };

    AudioOutput audio;

    void open(const std::string &url, const ProxyConfig &proxy);
    void close();
    void play();
    void pause();
    void seek(int64_t positionMs);
    void setVolume(float volume);
    void setMuted(bool muted);
    void setTargetSize(const Platform::Size &size);
    [[nodiscard]] int64_t bufferedPosition() const { return demuxedMs.load(std::memory_order_relaxed); }

    void startWorker(const std::string &input);
    void stopWorker();
    void present();
    void scheduleNextPresent(int64_t clock);
    [[nodiscard]] int64_t clockMs() const;
    void fail(const std::string &message);

    // decode/demux threads
    static int interruptThunk(void *opaque);
    void decodeLoop(std::string input);
    void demuxLoop();
    bool openSource(const std::string &input);
    void demuxSeek(int64_t targetMs);
    void applyFlush(int64_t targetMs);
    void drainAtEnd(AVFrame *avFrame);
    void decodePacket(AVPacket *pkt, AVFrame *avFrame);
    bool pushVideoFrame(AVFrame *avFrame);
    void pushAudioFrame(AVFrame *avFrame);
    void drainPendingAudio();

    [[nodiscard]] bool beforeSeekTarget(int64_t ptsMs) const
    {
        const int64_t target = seekTargetMs.load(std::memory_order_relaxed);
        return target >= 0 && ptsMs < target;
    }

    [[nodiscard]] bool demuxInterrupted() const
    {
        return !running.load(std::memory_order_relaxed) || pendingSeek.load(std::memory_order_relaxed) >= 0;
    }

    [[nodiscard]] bool seekInFlight() const
    {
        return pendingSeek.load(std::memory_order_relaxed) >= 0 || flushPending.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool interrupted() const
    {
        return !running.load(std::memory_order_relaxed) || seekInFlight();
    }

    [[nodiscard]] bool audioPendingEmpty() const { return audioPendingOffset >= audioPending.size(); }

    [[nodiscard]] int64_t audioDrainWaitMs() const
    {
        const int64_t buffered = int64_t(audio.bufferedFrames()) * 1000 / AudioOutput::SampleRate;
        return std::clamp(buffered / 2, int64_t(5), int64_t(250));
    }

    [[nodiscard]] bool audioSinkDrained() const
    {
        if (!audioSinkUsable())
            return true;
        if (!audioPendingEmpty())
            return false;
        return audio.bufferedFrames() == 0 ||
               (!audio.isRunning() && !paused.load(std::memory_order_relaxed));
    }

    [[nodiscard]] bool audioSinkUsable() const
    {
        return decoder.hasAudio() && !audioSinkBroken.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool decodable() const
    {
        return (!paused.load(std::memory_order_relaxed) && !reachedEnd.load(std::memory_order_relaxed)) ||
               !primed.load(std::memory_order_relaxed);
    }

    void clearPendingAudio()
    {
        audioPending.clear();
        audioPendingOffset = 0;
    }

    template <typename F>
    void postToOwner(F &&fn)
    {
        std::weak_ptr<void> lifetime = sessionContext;
        Platform::postToMain([lifetime, fn = std::forward<F>(fn)]() mutable {
            if (lifetime.lock()) fn();
        });
    }
};

#else // STYX_HAVE_FFMPEG

struct Player::Impl : OwnerThreadState
{
    explicit Impl(Player *player) { owner = player; }

    void open(const std::string &, const ProxyConfig &) {}
    void close() {}
    void play() {}
    void pause() {}
    void seek(int64_t) {}
    void setVolume(float value) { userVolume = std::clamp(value, 0.0f, 1.0f); }
    void setMuted(bool value) { userMuted = value; }
    void setTargetSize(const Platform::Size &) {}
    [[nodiscard]] int64_t bufferedPosition() const { return 0; }
};

#endif // STYX_HAVE_FFMPEG

Player::Player() : d(std::make_unique<Impl>(this)) {}

Player::~Player()
{
    d->state = State::Idle; // no stateChanged() out of a destructor
    d->close();
}

void Player::open(const std::string &url, const ProxyConfig &proxy)
{
    d->open(url, proxy);
}

void Player::close()
{
    d->close();
}

void Player::play()
{
    d->play();
}

void Player::pause()
{
    d->pause();
}

void Player::togglePlayPause()
{
    if (state() == State::Playing)
        pause();
    else
        play();
}

void Player::seek(int64_t positionMs)
{
    d->seek(positionMs);
}

void Player::setVolume(float volume)
{
    d->setVolume(volume);
}

float Player::volume() const
{
    return d->userVolume;
}

void Player::setMuted(bool muted)
{
    d->setMuted(muted);
}

bool Player::isMuted() const
{
    return d->userMuted;
}

void Player::setTargetSize(const Platform::Size &size)
{
    d->setTargetSize(size);
}

Platform::ImageBuffer Player::currentFrame() const
{
    return d->frame;
}

int64_t Player::position() const
{
    return d->positionMs;
}

int64_t Player::duration() const
{
    return d->durationMs;
}

int64_t Player::bufferedPosition() const
{
    return d->bufferedPosition();
}

Platform::Size Player::nativeSize() const
{
    return d->native;
}

bool Player::hasVideo() const
{
    return d->videoAvailable;
}

Player::State Player::state() const
{
    return d->state;
}

bool Player::isSeekable() const
{
    return d->seekable;
}

#ifdef STYX_HAVE_FFMPEG

void Player::Impl::open(const std::string &url, const ProxyConfig &proxyConfig)
{
    close();

    setState(Player::State::Opening);

    proxy = proxyConfig;
    startWorker(url.starts_with("file:") ? Platform::localFilePath(url).string() : url);
}

void Player::Impl::close()
{
    stopWorker();

    sessionContext.reset();

    presentTimer.stop();
    audio.stop();

    {
        std::lock_guard<std::mutex> lock(mutex);
        frames.clear();
    }

    positionMs = 0;
    durationMs = 0;
    demuxedMs.store(0, std::memory_order_relaxed);
    frame = Platform::ImageBuffer();
    audioAvailable = false;
    videoAvailable = false;
    seeking = false;
    audioClockStalled = false;
    autoPlayOnReady = false;

    setState(Player::State::Idle);
}

void Player::Impl::play()
{
    if (state == Player::State::Opening) {
        autoPlayOnReady = true;
        return;
    }

    if (state != Player::State::Paused && state != Player::State::Ended)
        return;

    if (state == Player::State::Ended)
        seek(0);

    if (audioAvailable) {
        const bool started = audio.start();
        audioSinkBroken.store(!started, std::memory_order_relaxed);
        if (!started)
            audioClockStalled = true;
    }

    wallBaseMs = positionMs;
    wallTimer = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(mutex);
        paused.store(false, std::memory_order_relaxed);
    }
    cv.notify_all();

    setState(Player::State::Playing);
    scheduleNextPresent(positionMs);
}

void Player::Impl::pause()
{
    if (state != Player::State::Playing)
        return;

    wallBaseMs = clockMs();
    {
        std::lock_guard<std::mutex> lock(mutex);
        paused.store(true, std::memory_order_relaxed);
    }
    cv.notify_all();

    audio.stop();
    presentTimer.stop();

    setState(Player::State::Paused);
}

void Player::Impl::seek(int64_t requestedMs)
{
    if (!running.load(std::memory_order_relaxed))
        return;

    if (state == Player::State::Opening)
        return;

    const int64_t upper = durationMs > 0 ? durationMs : requestedMs;
    const int64_t target = std::clamp(requestedMs, int64_t(0), upper);

    seeking = true;
    positionMs = target;
    wallBaseMs = target;
    wallTimer = std::chrono::steady_clock::now();
    audioClockStalled = false;
    reachedEnd.store(false, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(mutex);
        pendingSeek.store(target, std::memory_order_relaxed);
    }
    cv.notify_all();

    owner->positionChanged.publish(target);

    if (state == Player::State::Ended)
        setState(Player::State::Paused);
}

void Player::Impl::setVolume(float value)
{
    const float bounded = std::clamp(value, 0.0f, 1.0f);
    if (std::abs(bounded - userVolume) <= 0.00001f)
        return;

    userVolume = bounded;
    setStoredVolume(bounded);
    audio.setVolume(bounded);
}

void Player::Impl::setMuted(bool value)
{
    if (userMuted == value)
        return;

    userMuted = value;
    setStoredMuted(value);
    audio.setMuted(value);
}

void Player::Impl::setTargetSize(const Platform::Size &size)
{
    std::lock_guard<std::mutex> lock(mutex);
    targetSize = size;
}

void Player::Impl::fail(const std::string &message)
{
    Core::log(Core::LogLevel::Warning, LogVideo) << "playback failed:" << message;
    setState(Player::State::Error);
    owner->errorOccurred.publish(message);
}

void Player::Impl::startWorker(const std::string &input)
{
    stopWorker();

    audio.setVolume(userVolume);
    audio.setMuted(userMuted);

    sessionContext = std::make_shared<int>(0);
    running.store(true, std::memory_order_relaxed);
    paused.store(true, std::memory_order_relaxed);
    reachedEnd.store(false, std::memory_order_relaxed);
    pendingSeek.store(-1, std::memory_order_relaxed);
    seekTargetMs.store(-1, std::memory_order_relaxed);
    awaitingSeekFrame.store(false, std::memory_order_relaxed);
    demuxedMs.store(0, std::memory_order_relaxed);
    demuxEof.store(false, std::memory_order_relaxed);
    flushPending.store(false, std::memory_order_relaxed);
    audioSinkBroken.store(false, std::memory_order_relaxed);
    primed.store(false, std::memory_order_relaxed);

    worker = std::thread(&Player::Impl::decodeLoop, this, input);
}

void Player::Impl::stopWorker()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        running.store(false, std::memory_order_relaxed);
    }

    if (worker.joinable()) {
        cv.notify_all();
        worker.join();
    }
}

int Player::Impl::interruptThunk(void *opaque)
{
    auto *impl = static_cast<Player::Impl *>(opaque);
    return impl && impl->demuxInterrupted() ? 1 : 0;
}

bool Player::Impl::openSource(const std::string &input)
{
    decoder.setInterrupt(&Player::Impl::interruptThunk, this);

    std::string openError;
    if (!decoder.open(input, proxy, &openError)) {
        postToOwner([this, openError] { fail(openError); });
        return false;
    }

    const MediaDecoder::Info info = decoder.info();
    if (!info.hasVideo)
        primed.store(true, std::memory_order_relaxed);

    postToOwner([this, info] {
        native = info.native;
        durationMs = info.durationMs;
        seekable = info.seekable;
        audioAvailable = info.hasAudio;
        videoAvailable = info.hasVideo;
        owner->durationChanged.publish(info.durationMs);
    });

    return true;
}

void Player::Impl::decodePacket(AVPacket *pkt, AVFrame *avFrame)
{
    const MediaDecoder::Stream stream = decoder.streamOf(pkt);
    if (stream == MediaDecoder::Stream::None || !decoder.sendPacket(stream, pkt))
        return;

    while (decoder.receiveFrame(stream, avFrame) >= 0) {
        if (stream == MediaDecoder::Stream::Video)
            pushVideoFrame(avFrame);
        else
            pushAudioFrame(avFrame);
        av_frame_unref(avFrame);
    }
}

void Player::Impl::demuxSeek(int64_t targetMs)
{
    std::string seekError;
    if (!decoder.seek(targetMs, &seekError))
        Core::log(Core::LogLevel::Warning, LogVideo) << "seek failed:" << seekError;

    demuxEof.store(false, std::memory_order_relaxed);
    demuxedMs.store(targetMs, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(mutex);
        packets.clearLocked();
        packets.pushFlushLocked(targetMs);
        flushPending.store(true, std::memory_order_relaxed);
    }
    cv.notify_all();
}

void Player::Impl::applyFlush(int64_t targetMs)
{
    decoder.flush();

    {
        std::lock_guard<std::mutex> lock(mutex);
        frames.clear();
    }
    clearPendingAudio();

    audio.reset(targetMs);

    reachedEnd.store(false, std::memory_order_relaxed);
    seekTargetMs.store(targetMs, std::memory_order_relaxed);

    if (decoder.hasVideo()) {
        awaitingSeekFrame.store(true, std::memory_order_relaxed);
        primed.store(false, std::memory_order_relaxed);
        return;
    }

    postToOwner([this, targetMs] {
        seeking = false;
        audioClockStalled = false;
        wallBaseMs = targetMs;
        wallTimer = std::chrono::steady_clock::now();
        if (state == Player::State::Playing)
            scheduleNextPresent(targetMs);
    });
}

void Player::Impl::demuxLoop()
{
    AVPacket *packet = av_packet_alloc();

    while (running.load(std::memory_order_relaxed)) {
        const int64_t seekTarget = pendingSeek.exchange(-1, std::memory_order_relaxed);
        if (seekTarget >= 0) {
            demuxSeek(seekTarget);
            continue;
        }

        {
            std::unique_lock<std::mutex> lock(mutex);
            if (packets.fullLocked() || demuxEof.load(std::memory_order_relaxed)) {
                cv.wait(lock, [this] {
                    return demuxInterrupted() ||
                           (!demuxEof.load(std::memory_order_relaxed) && !packets.fullLocked());
                });
                continue;
            }
        }

        const int ret = decoder.readPacket(packet);
        if (ret < 0) {
            if (demuxInterrupted())
                continue;

            if (ret == AVERROR(EAGAIN)) {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait_for(lock, std::chrono::milliseconds(5));
                continue;
            }

            if (ret != AVERROR_EOF)
                Core::log(Core::LogLevel::Warning, LogVideo) << "read failed:" << MediaDecoder::errorString(ret);

            const int64_t total = decoder.containerDurationMs();
            if (total > 0)
                demuxedMs.store(total, std::memory_order_relaxed);
            demuxEof.store(true, std::memory_order_relaxed);
            cv.notify_all();
            continue;
        }

        if (decoder.streamOf(packet) == MediaDecoder::Stream::None) {
            av_packet_unref(packet);
            continue;
        }

        const int64_t tsMs = decoder.packetTimestampMs(packet);
        if (tsMs >= 0 && tsMs > demuxedMs.load(std::memory_order_relaxed))
            demuxedMs.store(tsMs, std::memory_order_relaxed);

        AVPacket *stored = av_packet_alloc();
        av_packet_move_ref(stored, packet);

        bool wasEmpty;
        {
            std::lock_guard<std::mutex> lock(mutex);
            wasEmpty = packets.emptyLocked();
            packets.pushLocked(stored);
        }

        if (wasEmpty)
            cv.notify_all();
    }

    av_packet_free(&packet);
}

bool Player::Impl::pushVideoFrame(AVFrame *avFrame)
{
    const int64_t ptsMs = decoder.frameTimestampMs(avFrame, MediaDecoder::Stream::Video);

    if (beforeSeekTarget(ptsMs))
        return false;

    Platform::Size want;
    {
        std::lock_guard<std::mutex> lock(mutex);
        want = targetSize;
    }

    const Platform::ImageBuffer image = decoder.scaleToImage(avFrame, effectiveTarget(Platform::Size{avFrame->width, avFrame->height}, want));
    if (image.empty())
        return false;

    bool wasEmpty;
    {
        std::lock_guard<std::mutex> lock(mutex);
        wasEmpty = frames.empty();
        frames.push_back({ image, ptsMs });
    }
    primed.store(true, std::memory_order_relaxed);

    const bool resolvedSeek = awaitingSeekFrame.exchange(false, std::memory_order_relaxed);

    if (!resolvedSeek && !wasEmpty)
        return true;

    postToOwner([this, ptsMs, resolvedSeek, image] {
        if (resolvedSeek) {
            seeking = false;
            audioClockStalled = false;
            wallBaseMs = ptsMs;
            wallTimer = std::chrono::steady_clock::now();
        }

        if (state == Player::State::Playing) {
            if (!presentTimer.active())
                present();
            return;
        }

        if (!frame.empty() && !resolvedSeek)
            return;

        frame = image;
        if (resolvedSeek && positionMs != ptsMs) {
            positionMs = ptsMs;
            owner->positionChanged.publish(positionMs);
        }
        owner->frameReady.publish();
    });

    return true;
}

void Player::Impl::pushAudioFrame(AVFrame *avFrame)
{
    if (!audioSinkUsable())
        return;

    if (beforeSeekTarget(decoder.frameTimestampMs(avFrame, MediaDecoder::Stream::Audio)))
        return;

    if (decoder.appendResampledFrames(avFrame, audioPending, audioScratch) <= 0)
        return;

    drainPendingAudio();
}

void Player::Impl::drainPendingAudio()
{
    if (audioPendingEmpty())
        return;

    if (audioSinkBroken.load(std::memory_order_relaxed)) {
        clearPendingAudio();
        return;
    }

    const size_t available = audioPending.size() - audioPendingOffset;
    const int pendingFrames = static_cast<int>(available / AudioOutput::Channels);
    const int written = audio.write(audioPending.data() + audioPendingOffset, pendingFrames);
    if (written <= 0)
        return;

    audioPendingOffset += static_cast<size_t>(written) * AudioOutput::Channels;
    if (audioPendingEmpty())
        clearPendingAudio();
}

void Player::Impl::decodeLoop(std::string input)
{
    if (!openSource(input)) {
        decoder.close();
        return;
    }

    postToOwner([this] {
        if (state != Player::State::Opening)
            return;
        setState(Player::State::Paused);
        if (autoPlayOnReady) {
            autoPlayOnReady = false;
            play();
        }
    });

    demuxer = std::thread(&Player::Impl::demuxLoop, this);

    AVFrame *avFrame = av_frame_alloc();

    while (running.load(std::memory_order_relaxed)) {
        drainPendingAudio();

        AVPacket *next = nullptr;
        int64_t flushTarget = -1;
        bool starved = false;
        bool queueWasFull = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (packets.emptyLocked()) {
                starved = true;
            } else if (packets.frontIsFlushLocked()) {
                flushTarget = packets.takeFlushLocked();
                flushPending.store(false, std::memory_order_relaxed);
            } else if (decodable()) {
                const bool videoRoom = decoder.hasVideo() && frames.size() < MaxQueuedFrames;
                const bool audioRoom = audioSinkUsable() &&
                                       audio.writableFrames() >= AudioChunkFrames &&
                                       audioPendingEmpty();
                if (videoRoom || audioRoom) {
                    queueWasFull = packets.fullLocked();
                    next = packets.takePacketLocked();
                }
            }
        }

        if (flushTarget >= 0) {
            applyFlush(flushTarget);
            continue;
        }

        if (next) {
            if (queueWasFull)
                cv.notify_all();

            decodePacket(next, avFrame);
            av_packet_free(&next);
            continue;
        }

        if (starved &&
            demuxEof.load(std::memory_order_relaxed) &&
            !reachedEnd.load(std::memory_order_relaxed) &&
            !seekInFlight()) {
            drainAtEnd(avFrame);
            continue;
        }

        std::unique_lock<std::mutex> lock(mutex);
        if (decodable() && !packets.emptyLocked() && audioSinkUsable()) {
            cv.wait_for(lock, std::chrono::milliseconds(audioDrainWaitMs()));
        } else {
            cv.wait(lock, [this] {
                if (!running.load(std::memory_order_relaxed))
                    return true;

                if (packets.emptyLocked())
                    return demuxEof.load(std::memory_order_relaxed) &&
                           !reachedEnd.load(std::memory_order_relaxed) &&
                           !seekInFlight();

                if (packets.frontIsFlushLocked())
                    return true;

                return decodable() && (!decoder.hasVideo() || frames.size() < MaxQueuedFrames);
            });
        }
    }

    av_frame_free(&avFrame);

    if (demuxer.joinable()) {
        cv.notify_all();
        demuxer.join();
    }

    decoder.close();
}

void Player::Impl::drainAtEnd(AVFrame *avFrame)
{
    if (decoder.drain(MediaDecoder::Stream::Video)) {
        while (!interrupted()) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [this] {
                    return interrupted() || frames.size() < MaxQueuedFrames;
                });
            }
            if (interrupted() || decoder.receiveFrame(MediaDecoder::Stream::Video, avFrame) < 0)
                break;

            pushVideoFrame(avFrame);
            av_frame_unref(avFrame);
        }
    }

    if (decoder.drain(MediaDecoder::Stream::Audio)) {
        while (!interrupted() && decoder.receiveFrame(MediaDecoder::Stream::Audio, avFrame) >= 0) {
            pushAudioFrame(avFrame);
            av_frame_unref(avFrame);
        }
    }

    bool clockHandedOver = false;
    while (!interrupted()) {
        drainPendingAudio();

        bool videoDrained;
        {
            std::lock_guard<std::mutex> lock(mutex);
            videoDrained = frames.empty();
        }

        const bool audioDrained = audioSinkDrained();

        if (audioDrained && !clockHandedOver && audioSinkUsable()) {
            clockHandedOver = true;
            postToOwner([this] {
                audioClockStalled = true;
                if (state == Player::State::Playing) {
                    wallBaseMs = audio.clockMs();
                    wallTimer = std::chrono::steady_clock::now();
                }
            });
        }

        if (videoDrained && audioDrained)
            break;

        std::unique_lock<std::mutex> lock(mutex);
        if (paused.load(std::memory_order_relaxed)) {
            cv.wait(lock, [this] {
                return interrupted() || !paused.load(std::memory_order_relaxed);
            });
        } else {
            cv.wait_for(lock, std::chrono::milliseconds(10));
        }
    }

    if (interrupted())
        return;

    awaitingSeekFrame.store(false, std::memory_order_relaxed);
    reachedEnd.store(true, std::memory_order_relaxed);
    postToOwner([this] {
        seeking = false;
        if (state == Player::State::Playing) {
            positionMs = durationMs;
            owner->positionChanged.publish(positionMs);
            setState(Player::State::Ended);
            presentTimer.stop();
            audio.stop();
        }
    });
}

int64_t Player::Impl::clockMs() const
{
    if (audioAvailable && !audioClockStalled)
        return audio.clockMs();
    if (paused.load(std::memory_order_relaxed))
        return wallBaseMs;
    return wallBaseMs + std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - wallTimer).count();
}

void Player::Impl::scheduleNextPresent(int64_t clock)
{
    if (state != Player::State::Playing)
        return;

    if (!videoAvailable) {
        presentTimer.start(std::chrono::milliseconds(AudioOnlyPresentDelayMs), false, [this] { present(); });
        return;
    }

    int64_t delay = 16;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!frames.empty())
            delay = frames.front().ptsMs - clock;
    }

    presentTimer.start(std::chrono::milliseconds(std::clamp(delay, MinPresentDelayMs, MaxPresentDelayMs)), false, [this] { present(); });
}

void Player::Impl::present()
{
    if (seeking) {
        scheduleNextPresent(positionMs);
        return;
    }

    const int64_t clock = clockMs();

    Platform::ImageBuffer latest;
    bool popped = false;
    {
        std::lock_guard<std::mutex> lock(mutex);

        while (!frames.empty() && frames.front().ptsMs <= clock) {
            latest = frames.front().image;
            popped = true;
            frames.pop_front();
        }
    }
    cv.notify_all();

    if (popped) {
        frame = latest;
        owner->frameReady.publish();
    }

    const int64_t bounded = durationMs > 0 ? std::clamp(clock, int64_t(0), durationMs)
                                          : std::max(int64_t(0), clock);
    if (positionMs != bounded) {
        positionMs = bounded;
        owner->positionChanged.publish(positionMs);
    }

    scheduleNextPresent(clock);
}

#endif // STYX_HAVE_FFMPEG

} // namespace Media
} // namespace Core
} // namespace Styx
