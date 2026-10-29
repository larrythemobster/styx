#include "Core/Media/ClipPlayer.hpp"
#include "Core/Media/ClipDecoder.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Styx::Core::Media {
namespace {
constexpr int64_t MinFrameGapMs = 10;
constexpr int64_t MaxFrameGapMs = 60000;
constexpr int FramesInAnAnimation = 2;

Platform::Size frameSizeWithoutUpscaling(Platform::Size native, Platform::Size bounds, ClipPlayer::Fit fit)
{
    if (native.empty() || bounds.empty()) return native;
    Platform::Size scaled;
    if (fit == ClipPlayer::Fit::Stretch) {
        scaled = bounds;
    } else {
        const double x = double(bounds.width) / native.width;
        const double y = double(bounds.height) / native.height;
        const double ratio = fit == ClipPlayer::Fit::Cover ? std::max(x, y) : std::min(x, y);
        scaled = {std::max(1, int(native.width * ratio)), std::max(1, int(native.height * ratio))};
    }
    if (scaled.empty() || scaled.width >= native.width || scaled.height >= native.height) return native;
    return scaled;
}
}

struct ClipPlayer::Clip {
    std::string path;
    Platform::Size boundPixels;
    Fit fit = Fit::Contain;
    int64_t memoryLimitBytes = 0;
    std::unique_ptr<ClipDecoder> decoder;
    Platform::Size frameSize;
    int64_t previousStartMs = 0;
    int framesThisLoop = 0;
    bool playing = false;
    bool failed = false;
    bool hasReady = false;
    Platform::ImageBuffer ready;
    int64_t readyGapMs = 0;
    Platform::ImageBuffer spare;
    Platform::ImageBuffer shown;
    std::optional<int64_t> shownAtMs;
    bool failureReported = false;
};

ClipPlayer::ClipPlayer() = default;
ClipPlayer::~ClipPlayer()
{
    callbackLifetime.reset();
    presentTimer.stop();
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    frameWanted.notify_all();
    if (decodeThread.joinable()) decodeThread.join();
}

int64_t ClipPlayer::elapsedMs() const
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - clock).count();
}

ClipPlayer::ClipId ClipPlayer::open(const std::string &path, Platform::Size bounds, Fit fit)
{
    auto clip = std::make_shared<Clip>();
    clip->path = path;
    clip->boundPixels = bounds;
    clip->fit = fit;
    clip->memoryLimitBytes = newClipMemoryLimitBytes;
    const ClipId id = ++lastId;
    clips.emplace(id, clip);
    {
        std::lock_guard lock(mutex);
        decodeOrder.push_back(std::move(clip));
    }
    if (!decodeThread.joinable()) decodeThread = std::thread(&ClipPlayer::decodeLoop, this);
    return id;
}

int64_t ClipPlayer::memoryToPlay(Platform::Size native, Platform::Size frame)
{
    if (native.empty() || frame.empty()) return 0;
    const uint64_t nativePixels = uint64_t(native.width) * native.height;
    const uint64_t framePixels = uint64_t(frame.width) * frame.height;
    constexpr uint64_t limit = uint64_t(std::numeric_limits<int64_t>::max());
    if (nativePixels > limit / 8 || framePixels > limit / 8 || nativePixels + framePixels > limit / 8)
        return std::numeric_limits<int64_t>::max();
    return int64_t((nativePixels + framePixels) * 8);
}
void ClipPlayer::setMemoryLimitForNewClips(int64_t bytes) { newClipMemoryLimitBytes = bytes; }

void ClipPlayer::close(ClipId id)
{
    auto it = clips.find(id);
    if (it == clips.end()) return;
    auto clip = std::move(it->second);
    clips.erase(it);
    std::lock_guard lock(mutex);
    decodeOrder.erase(std::remove(decodeOrder.begin(), decodeOrder.end(), clip), decodeOrder.end());
}
void ClipPlayer::closeAll()
{
    clips.clear();
    presentTimer.stop();
    std::lock_guard lock(mutex);
    decodeOrder.clear();
}
void ClipPlayer::setPlaying(ClipId id, bool playing)
{
    auto it = clips.find(id);
    if (it == clips.end()) return;
    Clip &clip = *it->second;
    {
        std::lock_guard lock(mutex);
        if (clip.playing == playing) return;
        clip.playing = playing;
    }
    if (!playing) return;
    if (clip.shownAtMs) clip.shownAtMs = elapsedMs();
    frameWanted.notify_one();
    presentDueFrames();
}
const Platform::ImageBuffer *ClipPlayer::frame(ClipId id) const
{
    auto it = clips.find(id);
    return it == clips.end() || it->second->shown.empty() ? nullptr : &it->second->shown;
}
ClipPlayer::ClipPtr ClipPlayer::nextClipAwaitingFrame()
{
    for (size_t offset = 0; offset < decodeOrder.size(); ++offset) {
        const size_t slot = (nextToServe + offset) % decodeOrder.size();
        const auto &clip = decodeOrder[slot];
        if (clip->playing && !clip->failed && !clip->hasReady) {
            nextToServe = slot + 1;
            return clip;
        }
    }
    return nullptr;
}
std::optional<int64_t> ClipPlayer::decodeNextFrame(Clip &clip, Platform::ImageBuffer &buffer)
{
    if (!clip.decoder) {
        clip.decoder = ClipDecoder::open(clip.path);
        if (!clip.decoder) return std::nullopt;
        const auto native = clip.decoder->nativeSize();
        clip.frameSize = frameSizeWithoutUpscaling(native, clip.boundPixels, clip.fit);
        if (clip.frameSize.empty() || memoryToPlay(native, clip.frameSize) > clip.memoryLimitBytes) {
            clip.decoder.reset();
            return std::nullopt;
        }
    }
    int64_t startMs = 0;
    int64_t gapMs = 0;
    if (clip.decoder->nextFrameOfLoop(buffer, startMs, clip.frameSize)) {
        gapMs = clip.framesThisLoop == 0 ? 0 : startMs - clip.previousStartMs;
    } else {
        if (clip.framesThisLoop < FramesInAnAnimation) return std::nullopt;
        const int64_t lastFrameMs = std::max<int64_t>(0, clip.decoder->finishedLoopMs() - clip.previousStartMs);
        clip.framesThisLoop = 0;
        if (!clip.decoder->rewind() || !clip.decoder->nextFrameOfLoop(buffer, startMs, clip.frameSize)) return std::nullopt;
        gapMs = lastFrameMs + startMs;
    }
    clip.previousStartMs = startMs;
    ++clip.framesThisLoop;
    return std::clamp(gapMs, MinFrameGapMs, MaxFrameGapMs);
}
void ClipPlayer::decodeLoop()
{
    std::unique_lock lock(mutex);
    for (;;) {
        ClipPtr clip;
        frameWanted.wait(lock, [this, &clip] {
            if (!stopping) clip = nextClipAwaitingFrame();
            return stopping || bool(clip);
        });
        if (stopping) return;
        auto buffer = std::exchange(clip->spare, {});
        lock.unlock();
        const auto gap = decodeNextFrame(*clip, buffer);
        lock.lock();
        if (gap) {
            clip->ready = std::move(buffer);
            clip->readyGapMs = *gap;
            clip->hasReady = true;
        } else clip->failed = true;
        lock.unlock();
        clip.reset();
        requestPresent();
        lock.lock();
    }
}
void ClipPlayer::requestPresent()
{
    if (presentRequested.exchange(true)) return;
    std::weak_ptr<void> alive = callbackLifetime;
    Platform::Dispatcher::main().post([this, alive] {
        if (alive.expired()) return;
        presentRequested.store(false);
        presentDueFrames();
    });
}
void ClipPlayer::presentDueFrames()
{
    const int64_t now = elapsedMs();
    int64_t nextDue = std::numeric_limits<int64_t>::max();
    std::vector<ClipId> changed, failed;
    {
        std::lock_guard lock(mutex);
        for (auto &[id, ptr] : clips) {
            Clip &clip = *ptr;
            if (clip.failed) {
                if (!std::exchange(clip.failureReported, true)) failed.push_back(id);
                continue;
            }
            if (!clip.playing || !clip.hasReady) continue;
            const int64_t due = clip.shownAtMs ? *clip.shownAtMs + clip.readyGapMs : now;
            if (due > now) { nextDue = std::min(nextDue, due); continue; }
            clip.spare = std::exchange(clip.shown, std::exchange(clip.ready, {}));
            clip.hasReady = false;
            clip.shownAtMs = now - due > clip.readyGapMs ? now : due;
            changed.push_back(id);
        }
    }
    if (!changed.empty()) frameWanted.notify_one();
    if (nextDue != std::numeric_limits<int64_t>::max())
        presentTimer.start(std::chrono::milliseconds(nextDue - now), false, [this] { presentDueFrames(); });
    for (ClipId id : failed) clipFailed.publish(id);
    for (ClipId id : changed) frameChanged.publish(id);
}

} // namespace Styx::Core::Media
