#pragma once

#include "ProxyConfig.hpp"
#include "Snowflake.hpp"
#include "Platform/Image.hpp"
#include "Platform/Native.hpp"
#include "Platform/Trackable.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <list>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Styx::Core {

enum class PinGroup { None, ChannelList, ChatView };

struct ImageRequestKey {
    std::string url;
    Platform::Size size;
    bool operator==(const ImageRequestKey &) const = default;
};

struct ImageRequestKeyHash {
    size_t operator()(const ImageRequestKey &key) const noexcept {
        size_t h = std::hash<std::string>{}(key.url);
        h ^= std::hash<int>{}(key.size.width) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(key.size.height) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

class ImageManager : public Platform::Trackable {
public:
    ImageManager();
    ~ImageManager() override;

    static constexpr int MaxDisplayWidth = 400;
    static constexpr int MaxDisplayHeight = 300;
    static constexpr int64_t FailedFetchRetryMs = 60 * 1000;

    void setAccountProxy(Snowflake accountId, const ProxyConfig &proxy);
    [[nodiscard]] bool hasRouteFor(Snowflake accountId) const;
    [[nodiscard]] ProxyConfig accountProxy(Snowflake accountId) const;
    [[nodiscard]] std::string curlProxyFor(Snowflake accountId) const;

    [[nodiscard]] bool isCached(const std::string &url, const Platform::Size &size);
    [[nodiscard]] bool isUnavailable(const ImageRequestKey &key) const;
    [[nodiscard]] bool willNeverLoad(const ImageRequestKey &key) const;
    [[nodiscard]] std::string rawDownloadPath(const std::string &url, const Platform::Size &size) const;
    Platform::ImageBuffer get(const std::string &url, const Platform::Size &size, Snowflake accountId, PinGroup pin = PinGroup::None);
    Platform::ImageBuffer getIfCached(const std::string &url, const Platform::Size &size);
    void downloadToCache(const std::string &url, const Platform::Size &size, Snowflake accountId);
    [[nodiscard]] Platform::ImageBuffer placeholder(const Platform::Size &size);
    void unpinGroup(PinGroup group);

    [[nodiscard]] static Platform::Size calculateDisplaySize(const Platform::Size &original);
    [[nodiscard]] static std::string fullQualityUrl(const std::string &proxyUrl);
    void fetch(const std::string &url, Snowflake accountId, Platform::Trackable *context, std::function<void(const std::string &)> done);
    void download(const std::string &url, Snowflake accountId, const std::string &path);

    Platform::Signal<const std::string &, const Platform::Size &, const Platform::ImageBuffer &> imageFetched;
    Platform::Signal<const std::string &, const Platform::Size &> rawDownloadReady;
    Platform::Signal<const std::string &, const Platform::Size &> imageUnavailable;

private:
    enum class FetchPurpose { Image, DownloadOnly };
    using ImageMap = std::unordered_map<ImageRequestKey, Platform::ImageBuffer, ImageRequestKeyHash>;
    Platform::ImageBuffer getImpl(const std::string &url, const Platform::Size &size, PinGroup pin, bool fetchIfNeeded, Snowflake accountId);
    void request(const std::string &url, const Platform::Size &size, PinGroup pin, Snowflake accountId, FetchPurpose purpose = FetchPurpose::Image);
    [[nodiscard]] std::filesystem::path getCachePath(const std::string &url, const Platform::Size &size) const;
    [[nodiscard]] bool recentlyFailed(const ImageRequestKey &key) const;
    void fetchFromNetwork(const std::string &url, const Platform::Size &size, const std::string &proxy, FetchPurpose purpose, int attempt = 1);
    void giveUpOn(const ImageRequestKey &key, int httpStatus);
    void cacheImage(const ImageRequestKey &key, Platform::ImageBuffer image);
    void pinImage(const ImageRequestKey &key, PinGroup group);
    static bool isDiscordProxyUrl(const std::string &url);
    static bool scalesToDevicePixels(const std::string &url);
    static std::string buildOptimizedUrl(const std::string &url, const Platform::Size &size, float dpr);

    std::unordered_map<Snowflake, ProxyConfig> accountProxies;
    std::unordered_map<ImageRequestKey, uint64_t, ImageRequestKeyHash> activeHttpRequests;
    std::filesystem::path tempDir;
    mutable std::unordered_map<ImageRequestKey, std::filesystem::path, ImageRequestKeyHash> cachePaths;
    mutable size_t nextCachePath = 0;
    std::unordered_set<ImageRequestKey, ImageRequestKeyHash> requests, downloadOnlyRequests, undecodable, refusedByServer;
    std::unordered_map<ImageRequestKey, std::chrono::steady_clock::time_point, ImageRequestKeyHash> failedAt;
    std::unordered_map<ImageRequestKey, PinGroup, ImageRequestKeyHash> pendingPins;
    ImageMap placeholders, cache, pinnedImages;
    std::list<ImageRequestKey> lru;
    std::unordered_map<ImageRequestKey, std::list<ImageRequestKey>::iterator, ImageRequestKeyHash> lruPositions;
    std::unordered_map<PinGroup, std::unordered_set<ImageRequestKey, ImageRequestKeyHash>> pinGroupKeys;
};

} // namespace Styx::Core
