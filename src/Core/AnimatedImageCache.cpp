#include "AnimatedImageCache.hpp"

#include "AnimationTiming.hpp"
#include "ApngDecoder.hpp"
#include "Logging.hpp"
#include "LottieDecoder.hpp"
#include "Media/ClipDecoder.hpp"
#include "Platform/Dispatcher.hpp"
#include "Platform/Native.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace Styx::Core {
namespace {
constexpr int MaxFrames = 300;
constexpr size_t BytesPerMiB = 1024 * 1024;
constexpr double MinBudgetScale = 0.5;

std::optional<Platform::Size> frameSizeWithinBudget(Platform::Size wanted, int frameCount, size_t byteBudget)
{
    if (frameCount > MaxFrames || wanted.empty())
        return std::nullopt;
    const double wantedBytes = double(frameCount) * wanted.width * wanted.height * 4;
    if (wantedBytes <= double(byteBudget))
        return wanted;
    const double scale = std::sqrt(double(byteBudget) / wantedBytes);
    if (scale < MinBudgetScale)
        return std::nullopt;
    return Platform::Size{std::max(1, int(wanted.width * scale)), std::max(1, int(wanted.height * scale))};
}

Platform::ImageBuffer scaledFrame(Platform::ImageBuffer source, Platform::Size size)
{
    if (source.width == size.width && source.height == size.height)
        return source;
    return Platform::Image::scale(source, size.width, size.height);
}
} // namespace

int AnimatedFrames::frameAt(int64_t loopPhaseMs) const
{
    if (isStatic())
        return 0;
    auto it = std::upper_bound(frameEndMs.begin(), frameEndMs.end(), int(loopPhaseMs));
    return int(std::min<size_t>(size_t(it - frameEndMs.begin()), frames.size() - 1));
}

AnimatedImageCache::AnimatedImageCache(ImageManager *imageManager) : imageManager(imageManager)
{
    setCacheLimitMiB(CacheLimit.fallback);
    track(imageManager->rawDownloadReady.connect([this](const std::string &url, const Platform::Size &size) {
        onRawDownloadReady(url, size);
    }));
}

static_assert(AnimatedImageCache::AnimationLimit.max <= AnimatedImageCache::CacheLimit.min);

void AnimatedImageCache::setCacheLimitMiB(int mib)
{
    cacheLimitKiB = size_t(std::clamp(mib, CacheLimit.min, CacheLimit.max)) * 1024;
    trimCache();
}

void AnimatedImageCache::setAnimationLimitMiB(int mib)
{
    animationLimitMiB = std::clamp(mib, AnimationLimit.min, AnimationLimit.max);
}

AnimatedImageCache::~AnimatedImageCache()
{
    alive->store(false);
    decodePool.clear();
    decodePool.stopAndWait();
}

void AnimatedImageCache::removeCached(const ImageRequestKey &key)
{
    auto it = cache.find(key);
    if (it == cache.end()) return;
    cacheCostKiB -= it->second.cost;
    lru.erase(it->second.lru);
    cache.erase(it);
}

void AnimatedImageCache::trimCache()
{
    while (cacheCostKiB > cacheLimitKiB && !lru.empty())
        removeCached(lru.back());
}

AnimatedFramesPtr AnimatedImageCache::get(const std::string &url, const Platform::Size &logicalSize, Snowflake accountId)
{
    const ImageRequestKey key{url, logicalSize};
    auto cached = cache.find(key);
    if (cached != cache.end()) {
        lru.splice(lru.begin(), lru, cached->second.lru);
        return cached->second.frames;
    }
    wanted.insert(key);
    auto inFlight = decoding.find(key);
    if (inFlight != decoding.end()) {
        inFlight->second->store(false);
        return nullptr;
    }
    const std::string path = imageManager->rawDownloadPath(url, logicalSize);
    if (path.empty())
        imageManager->downloadToCache(url, logicalSize, accountId);
    else
        decode(key, path);
    return nullptr;
}

void AnimatedImageCache::discard(const std::string &url, const Platform::Size &logicalSize)
{
    const ImageRequestKey key{url, logicalSize};
    wanted.erase(key);
    removeCached(key);
    auto inFlight = decoding.find(key);
    if (inFlight != decoding.end())
        inFlight->second->store(true);
}

void AnimatedImageCache::onRawDownloadReady(const std::string &url, const Platform::Size &size)
{
    const ImageRequestKey key{url, size};
    if (!wanted.contains(key) || decoding.contains(key)) return;
    const std::string path = imageManager->rawDownloadPath(url, size);
    if (path.empty()) wanted.erase(key);
    else decode(key, path);
}

void AnimatedImageCache::decode(const ImageRequestKey &key, const std::string &path)
{
    auto discarded = std::make_shared<std::atomic_bool>(false);
    decoding.emplace(key, discarded);
    const DecodeTarget target{key.size, size_t(animationLimitMiB) * BytesPerMiB};
    decodePool.post([this, alive = alive, key, path, target, discarded]() {
        if (discarded->load()) {
            Platform::Dispatcher::main().post([this, alive, key]() {
                if (alive->load()) onDecodeSkipped(key);
            });
            return;
        }
        auto decoded = decodeFrames(path, target);
        Platform::Dispatcher::main().post([this, alive, key, decoded = std::move(decoded)]() mutable {
            if (alive->load()) publish(key, std::move(decoded));
        });
    });
}

void AnimatedImageCache::onDecodeSkipped(const ImageRequestKey &key)
{
    decoding.erase(key);
    if (wanted.contains(key)) onRawDownloadReady(key.url, key.size);
}

AnimatedImageCache::Decoded AnimatedImageCache::decodeFrames(const std::string &path, const DecodeTarget &target)
{
    const std::string data = Platform::Resources::readFile(std::filesystem::u8path(path));
    if (data.empty()) return {};
    if (Lottie::looksLikeLottie(data)) return decodeLottie(data, target);
    if (auto apng = Apng::Reader::open(data)) return decodeApng(*apng, target);
    const bool gif = data.size() >= 4 && data.compare(0, 4, "GIF8") == 0;
    const bool webp = data.size() >= 12 && data.compare(0, 4, "RIFF") == 0 && data.compare(8, 4, "WEBP") == 0;
    if (gif || webp) return decodeAnimatedImage(path, target);
    return {};
}

AnimatedImageCache::Decoded AnimatedImageCache::decodeApng(Apng::Reader &apng, const DecodeTarget &target)
{
    auto frameSize = frameSizeWithinBudget(Platform::fitWithin(apng.canvasSize(), target.physicalSize), apng.frameCount(), target.byteBudget);
    if (!frameSize) {
        Core::log(Core::LogLevel::Debug, LogCore) << "APNG exceeds frame budget, rendering static";
        return {};
    }
    Decoded out;
    Platform::ImageBuffer canvas;
    int delayMs = 0;
    while (apng.next(canvas, delayMs)) {
        out.frames.push_back(scaledFrame(canvas, *frameSize));
        out.delaysMs.push_back(normalizedFrameDelayMs(delayMs));
    }
    return out;
}

AnimatedImageCache::Decoded AnimatedImageCache::decodeLottie(const std::string &data, const DecodeTarget &target)
{
    const auto animation = Lottie::Animation::fromJson(data);
    if (!animation) return {};
    const auto budgetedSize = frameSizeWithinBudget(target.physicalSize, animation->frameCount(), target.byteBudget);
    if (!budgetedSize) Core::log(Core::LogLevel::Debug, LogCore) << "Lottie animation exceeds frame budget, rendering static";
    const int frameCount = budgetedSize ? animation->frameCount() : 1;
    const auto frameSize = budgetedSize.value_or(target.physicalSize);
    Decoded out;
    for (int frame = 0; frame < frameCount; ++frame) {
        out.frames.push_back(animation->render(frame, frameSize));
        out.delaysMs.push_back(animation->frameDelayMs());
    }
    return out;
}

AnimatedImageCache::Decoded AnimatedImageCache::decodeAnimatedImage(const std::string &path, const DecodeTarget &target)
{
    auto decoder = Media::ClipDecoder::open(path);
    if (!decoder) return {};
    const auto wantedSize = Platform::fitWithin(decoder->nativeSize(), target.physicalSize);
    std::vector<int64_t> starts;
    Platform::ImageBuffer frame;
    int64_t start = 0;
    while (decoder->nextFrameOfLoop(frame, start, {1, 1})) {
        starts.push_back(start);
        if (starts.size() > MaxFrames) return {};
    }
    if (starts.empty()) return {};
    const int64_t loopEnd = decoder->finishedLoopMs();
    const auto frameSize = frameSizeWithinBudget(wantedSize, int(starts.size()), target.byteBudget);
    if (!frameSize || !decoder->rewind()) return {};
    Decoded out;
    out.frames.reserve(starts.size());
    out.delaysMs.reserve(starts.size());
    for (size_t i = 0; i < starts.size(); ++i) {
        Platform::ImageBuffer image;
        if (!decoder->nextFrameOfLoop(image, start, *frameSize)) return {};
        const int64_t end = i + 1 < starts.size() ? starts[i + 1] : loopEnd;
        out.frames.push_back(std::move(image));
        out.delaysMs.push_back(normalizedFrameDelayMs(int(end - starts[i])));
    }
    return out;
}

void AnimatedImageCache::publish(const ImageRequestKey &key, Decoded decoded)
{
    decoding.erase(key);
    if (!wanted.erase(key)) return;
    auto frames = std::make_shared<AnimatedFrames>();
    size_t bytes = 0;
    int elapsed = 0;
    for (size_t i = 0; i < decoded.frames.size(); ++i) {
        bytes += decoded.frames[i].byteSize();
        elapsed += decoded.delaysMs[i];
        frames->frameEndMs.push_back(elapsed);
    }
    frames->frames = std::move(decoded.frames);
    const AnimatedFramesPtr published = std::move(frames);
    const size_t cost = std::max<size_t>(1, bytes / 1024);
    removeCached(key);
    if (cost <= cacheLimitKiB) {
        lru.push_front(key);
        cache.emplace(key, CacheEntry{published, cost, lru.begin()});
        cacheCostKiB += cost;
        trimCache();
    }
    framesReady.publish(key.url, key.size, published);
}

} // namespace Styx::Core
