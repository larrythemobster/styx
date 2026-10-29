#pragma once

#include <atomic>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "ImageManager.hpp"
#include "MiBRange.hpp"
#include "Snowflake.hpp"
#include "Platform/Image.hpp"
#include "Platform/ThreadPool.hpp"
#include "Platform/Trackable.hpp"

namespace Styx::Core {

struct AnimatedFrames
{
    std::vector<Platform::ImageBuffer> frames;
    std::vector<int> frameEndMs;

    [[nodiscard]] bool isStatic() const { return frames.size() <= 1; }
    [[nodiscard]] int loopMs() const { return frameEndMs.empty() ? 0 : frameEndMs.back(); }
    [[nodiscard]] int frameAt(int64_t loopPhaseMs) const;
};

using AnimatedFramesPtr = std::shared_ptr<const AnimatedFrames>;

namespace Apng { class Reader; }

class AnimatedImageCache : public Platform::Trackable
{
public:
    static constexpr MiBRange CacheLimit{ 64, 96, 512 };
    static constexpr MiBRange AnimationLimit{ 8, 24, 64 };

    explicit AnimatedImageCache(ImageManager *imageManager);
    ~AnimatedImageCache() override;

    void setCacheLimitMiB(int mib);
    void setAnimationLimitMiB(int mib);
    AnimatedFramesPtr get(const std::string &url, const Platform::Size &logicalSize, Snowflake accountId);
    void discard(const std::string &url, const Platform::Size &logicalSize);

    Platform::Signal<const std::string &, const Platform::Size &, const AnimatedFramesPtr &> framesReady;

private:
    struct KeyLess {
        bool operator()(const ImageRequestKey &a, const ImageRequestKey &b) const {
            if (a.url != b.url) return a.url < b.url;
            return a.size < b.size;
        }
    };
    struct Decoded {
        std::vector<Platform::ImageBuffer> frames;
        std::vector<int> delaysMs;
    };
    struct DecodeTarget {
        Platform::Size physicalSize;
        size_t byteBudget;
    };
    struct CacheEntry {
        AnimatedFramesPtr frames;
        size_t cost;
        std::list<ImageRequestKey>::iterator lru;
    };
    using DiscardedFlag = std::shared_ptr<std::atomic_bool>;

    void onRawDownloadReady(const std::string &url, const Platform::Size &size);
    void decode(const ImageRequestKey &key, const std::string &path);
    void onDecodeSkipped(const ImageRequestKey &key);
    void publish(const ImageRequestKey &key, Decoded decoded);
    void removeCached(const ImageRequestKey &key);
    void trimCache();
    static Decoded decodeFrames(const std::string &path, const DecodeTarget &target);
    static Decoded decodeApng(Apng::Reader &apng, const DecodeTarget &target);
    static Decoded decodeLottie(const std::string &data, const DecodeTarget &target);
    static Decoded decodeAnimatedImage(const std::string &path, const DecodeTarget &target);

    ImageManager *imageManager;
    Platform::ThreadPool decodePool{4, "ImageDecode"};
    std::shared_ptr<std::atomic_bool> alive = std::make_shared<std::atomic_bool>(true);
    int animationLimitMiB = AnimationLimit.fallback;
    size_t cacheLimitKiB = 0;
    size_t cacheCostKiB = 0;
    std::set<ImageRequestKey, KeyLess> wanted;
    std::map<ImageRequestKey, DiscardedFlag, KeyLess> decoding;
    std::list<ImageRequestKey> lru;
    std::map<ImageRequestKey, CacheEntry, KeyLess> cache;
};

} // namespace Styx::Core
