#include "ImageManager.hpp"

#include "Discord/CdnUrls.hpp"
#include "Logging.hpp"
#include "Platform/Dispatcher.hpp"
#include "Platform/Http.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <utility>

namespace Styx::Core {
namespace {
constexpr int MaxMemoizedPlaceholderPx = 64;
constexpr int MaxFetchAttempts = 2;
constexpr int RetryBrokenTransferAfterMs = 500;
constexpr size_t MaxCachedImages = 300;

bool isErrorAnswer(int status) { return status >= 400; }
bool refusesForGood(int status)
{
    return isErrorAnswer(status) && status < 500 && status != 408 && status != 429;
}

std::string pathString(const std::filesystem::path &path)
{
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

Platform::ImageBuffer scaleImage(Platform::ImageBuffer image, Platform::Size bounds)
{
    if (bounds.empty()) return image;
    const auto size = Platform::fitWithin({image.width, image.height}, bounds);
    if (size.empty() || (size.width == image.width && size.height == image.height)) return image;
    return Platform::Image::scale(image, size.width, size.height);
}
}

ImageManager::ImageManager()
{
    std::error_code error;
    auto base = std::filesystem::temp_directory_path(error);
    if (!error) {
        tempDir = base / ("styx-images-" + Platform::randomUuid());
        std::filesystem::create_directories(tempDir, error);
    }
    if (error) {
        tempDir.clear();
        Core::log(Core::LogLevel::Warning, LogCore) << "Failed to create temp directory for image cache";
    }
}

ImageManager::~ImageManager()
{
    for (const auto &[key, id] : activeHttpRequests)
        Platform::HttpClient::instance().cancel(id);
    if (!tempDir.empty()) {
        std::error_code error;
        std::filesystem::remove_all(tempDir, error);
    }
}

void ImageManager::setAccountProxy(Snowflake accountId, const ProxyConfig &proxy) { accountProxies[accountId] = proxy; }
bool ImageManager::hasRouteFor(Snowflake accountId) const { return accountProxies.contains(accountId); }
ProxyConfig ImageManager::accountProxy(Snowflake accountId) const
{
    auto it = accountProxies.find(accountId);
    return it == accountProxies.end() ? ProxyConfig{} : it->second;
}
std::string ImageManager::curlProxyFor(Snowflake accountId) const { return accountProxy(accountId).toCurlProxyString(); }

bool ImageManager::isCached(const std::string &url, const Platform::Size &size)
{
    ImageRequestKey key{url, size};
    return pinnedImages.contains(key) || cache.contains(key) || (!undecodable.contains(key) && !rawDownloadPath(url, size).empty());
}
bool ImageManager::isUnavailable(const ImageRequestKey &key) const { return willNeverLoad(key) || recentlyFailed(key); }
bool ImageManager::willNeverLoad(const ImageRequestKey &key) const { return undecodable.contains(key) || refusedByServer.contains(key); }
std::string ImageManager::rawDownloadPath(const std::string &url, const Platform::Size &size) const
{
    auto path = getCachePath(url, size);
    std::error_code error;
    return !path.empty() && std::filesystem::exists(path, error) ? pathString(path) : std::string{};
}

Platform::ImageBuffer ImageManager::get(const std::string &url, const Platform::Size &size, Snowflake accountId, PinGroup pin)
{
    return getImpl(url, size, pin, true, accountId);
}
Platform::ImageBuffer ImageManager::getIfCached(const std::string &url, const Platform::Size &size)
{
    return getImpl(url, size, PinGroup::None, false, Snowflake{});
}

void ImageManager::cacheImage(const ImageRequestKey &key, Platform::ImageBuffer image)
{
    if (auto it = lruPositions.find(key); it != lruPositions.end()) {
        lru.erase(it->second);
        lruPositions.erase(it);
    }
    cache.insert_or_assign(key, std::move(image));
    lru.push_front(key);
    lruPositions[key] = lru.begin();
    while (cache.size() > MaxCachedImages) {
        auto oldest = lru.back();
        lru.pop_back();
        lruPositions.erase(oldest);
        cache.erase(oldest);
    }
}

void ImageManager::pinImage(const ImageRequestKey &key, PinGroup group)
{
    if (group == PinGroup::None) return;
    pinGroupKeys[group].insert(key);
    auto it = cache.find(key);
    if (it != cache.end()) {
        pinnedImages.insert_or_assign(key, std::move(it->second));
        cache.erase(it);
        auto pos = lruPositions.find(key);
        if (pos != lruPositions.end()) {
            lru.erase(pos->second);
            lruPositions.erase(pos);
        }
    }
}

Platform::ImageBuffer ImageManager::getImpl(const std::string &url, const Platform::Size &size, PinGroup pin, bool fetchIfNeeded, Snowflake accountId)
{
    ImageRequestKey key{url, size};
    if (auto it = pinnedImages.find(key); it != pinnedImages.end()) {
        if (pin != PinGroup::None) pinGroupKeys[pin].insert(key);
        return it->second;
    }
    if (auto it = cache.find(key); it != cache.end()) {
        if (pin != PinGroup::None) {
            pinImage(key, pin);
            return pinnedImages.at(key);
        }
        lru.splice(lru.begin(), lru, lruPositions.at(key));
        return it->second;
    }
    if (willNeverLoad(key)) return placeholder(size);
    if (requests.contains(key) || recentlyFailed(key)) {
        if (fetchIfNeeded) request(url, size, pin, accountId);
        return placeholder(size);
    }
    const auto path = getCachePath(url, size);
    std::error_code error;
    if (!path.empty() && std::filesystem::exists(path, error)) {
        auto image = Platform::Image::decodeFile(path);
        if (image) {
            const float dpr = scalesToDevicePixels(url) ? Platform::primaryDevicePixelRatio() : 1.0f;
            *image = scaleImage(std::move(*image), {int(std::lround(size.width * dpr)), int(std::lround(size.height * dpr))});
            cacheImage(key, std::move(*image));
            if (pin != PinGroup::None) { pinImage(key, pin); return pinnedImages.at(key); }
            return cache.at(key);
        }
        Core::log(Core::LogLevel::Warning, LogCore) << "Cached download is not a decodable image: " << url;
        undecodable.insert(key);
        return placeholder(size);
    }
    if (fetchIfNeeded) request(url, size, pin, accountId);
    return placeholder(size);
}

Platform::ImageBuffer ImageManager::placeholder(const Platform::Size &size)
{
    ImageRequestKey key{"", size};
    const bool memoize = size.width <= MaxMemoizedPlaceholderPx && size.height <= MaxMemoizedPlaceholderPx;
    if (memoize) {
        auto it = placeholders.find(key);
        if (it != placeholders.end()) return it->second;
    }
    const float dpr = Platform::primaryDevicePixelRatio();
    Platform::ImageBuffer image;
    image.width = std::max(0, int(std::lround(size.width * dpr)));
    image.height = std::max(0, int(std::lround(size.height * dpr)));
    image.pixels.resize(size_t(image.width) * image.height * 4);
    for (size_t i = 0; i < image.pixels.size(); i += 4) {
        image.pixels[i] = image.pixels[i + 1] = image.pixels[i + 2] = 60;
        image.pixels[i + 3] = 255;
    }
    if (memoize) placeholders.emplace(key, image);
    return image;
}

bool ImageManager::recentlyFailed(const ImageRequestKey &key) const
{
    auto it = failedAt.find(key);
    return it != failedAt.end() && std::chrono::steady_clock::now() - it->second < std::chrono::milliseconds(FailedFetchRetryMs);
}
void ImageManager::downloadToCache(const std::string &url, const Platform::Size &size, Snowflake accountId)
{
    request(url, size, PinGroup::None, accountId, FetchPurpose::DownloadOnly);
}
void ImageManager::request(const std::string &url, const Platform::Size &size, PinGroup pin, Snowflake accountId, FetchPurpose purpose)
{
    if (!hasRouteFor(accountId)) {
        Core::log(Core::LogLevel::Warning, LogCore) << "Refusing to fetch image with no proxied route for the account: " << url;
        return;
    }
    ImageRequestKey key{url, size};
    if (isUnavailable(key)) return;
    if (requests.contains(key)) {
        if (purpose == FetchPurpose::Image) downloadOnlyRequests.erase(key);
        if (pin != PinGroup::None && !pendingPins.contains(key)) pendingPins[key] = pin;
        return;
    }
    requests.insert(key);
    if (purpose == FetchPurpose::DownloadOnly) downloadOnlyRequests.insert(key);
    if (pin != PinGroup::None) pendingPins[key] = pin;
    fetchFromNetwork(url, size, curlProxyFor(accountId), purpose);
}
void ImageManager::giveUpOn(const ImageRequestKey &key, int httpStatus)
{
    pendingPins.erase(key);
    requests.erase(key);
    downloadOnlyRequests.erase(key);
    if (refusesForGood(httpStatus)) refusedByServer.insert(key);
    else failedAt[key] = std::chrono::steady_clock::now();
    imageUnavailable.publish(key.url, key.size);
}

void ImageManager::fetchFromNetwork(const std::string &url, const Platform::Size &size, const std::string &proxy, FetchPurpose purpose, int attempt)
{
    const float dpr = Platform::primaryDevicePixelRatio();
    const bool deviceScaled = scalesToDevicePixels(url);
    const bool optimize = purpose == FetchPurpose::Image && isDiscordProxyUrl(url) && !Discord::Cdn::isStickerUrl(url);
    Platform::HttpRequest req;
    req.url = optimize ? buildOptimizedUrl(url, size, dpr) : url;
    req.proxy = proxy;
    const auto path = getCachePath(url, size);
    const ImageRequestKey key{url, size};
    auto guard = Platform::Guard<ImageManager>(this);
    auto id = Platform::HttpClient::instance().request(std::move(req), [guard, key, proxy, purpose, attempt, path, dpr, deviceScaled](const Platform::HttpResponse &resp) {
        if (!guard) return;
        auto &self = *guard;
        self.activeHttpRequests.erase(key);
        bool fetched = resp.ok();
        auto part = path;
        part += ".part";
        if (fetched && purpose == FetchPurpose::DownloadOnly) {
            fetched = !path.empty() && Platform::Resources::writeFile(part, resp.body);
            if (fetched) {
                std::error_code error;
                std::filesystem::remove(path, error);
                error.clear();
                std::filesystem::rename(part, path, error);
                fetched = !error;
            }
        }
        if (!fetched) {
            Core::log(Core::LogLevel::Warning, LogCore) << "Failed to fetch image: " << key.url << " " << resp.error;
            if (purpose == FetchPurpose::DownloadOnly) { std::error_code error; std::filesystem::remove(part, error); }
            if (resp.ok() || isErrorAnswer(resp.statusCode) || attempt >= MaxFetchAttempts) {
                self.giveUpOn(key, resp.statusCode);
            } else {
                Platform::Dispatcher::main().setTimer(RetryBrokenTransferAfterMs, false, [guard, key, proxy, purpose, attempt] {
                    if (guard) guard->fetchFromNetwork(key.url, key.size, proxy, purpose, attempt + 1);
                });
            }
            return;
        }
        PinGroup pin = PinGroup::None;
        if (auto it = self.pendingPins.find(key); it != self.pendingPins.end()) { pin = it->second; self.pendingPins.erase(it); }
        self.failedAt.erase(key);
        const bool imageWanted = self.downloadOnlyRequests.erase(key) == 0;
        std::string downloaded;
        const std::string *data = &resp.body;
        if (purpose == FetchPurpose::DownloadOnly) {
            if (imageWanted) {
                try { downloaded = Platform::Resources::readFile(path); }
                catch (const std::exception &) {}
            }
            data = &downloaded;
        } else if (!path.empty()) {
            Platform::Resources::writeFile(path, resp.body);
        }
        auto image = imageWanted ? Platform::Image::decode(reinterpret_cast<const uint8_t *>(data->data()), data->size()) : std::nullopt;
        self.requests.erase(key);
        if (image) {
            const float scale = deviceScaled ? dpr : 1.0f;
            *image = scaleImage(std::move(*image), {int(std::lround(key.size.width * scale)), int(std::lround(key.size.height * scale))});
            self.cacheImage(key, std::move(*image));
            if (pin != PinGroup::None) self.pinImage(key, pin);
            const auto &stored = pin != PinGroup::None ? self.pinnedImages.at(key) : self.cache.at(key);
            self.imageFetched.publish(key.url, key.size, stored);
        } else if (imageWanted) {
            self.undecodable.insert(key);
            self.imageUnavailable.publish(key.url, key.size);
        }
        if (guard) guard->rawDownloadReady.publish(key.url, key.size);
    });
    activeHttpRequests[key] = id;
}

void ImageManager::unpinGroup(PinGroup group)
{
    if (group == PinGroup::None) return;
    auto groupIt = pinGroupKeys.find(group);
    if (groupIt == pinGroupKeys.end()) return;
    auto keys = std::move(groupIt->second);
    pinGroupKeys.erase(groupIt);
    for (const auto &key : keys) {
        bool stillPinned = false;
        for (const auto &[other, members] : pinGroupKeys) {
            if (members.contains(key)) { stillPinned = true; break; }
        }
        if (!stillPinned) {
            auto it = pinnedImages.find(key);
            if (it != pinnedImages.end()) {
                auto image = std::move(it->second);
                pinnedImages.erase(it);
                cacheImage(key, std::move(image));
            }
        }
    }
}

Platform::Size ImageManager::calculateDisplaySize(const Platform::Size &original)
{
    if (original.empty()) return {MaxDisplayWidth, MaxDisplayHeight};
    if (original.width <= MaxDisplayWidth && original.height <= MaxDisplayHeight) return original;
    return Platform::fitWithin(original, {MaxDisplayWidth, MaxDisplayHeight});
}
std::filesystem::path ImageManager::getCachePath(const std::string &url, const Platform::Size &size) const
{
    if (tempDir.empty()) return {};
    ImageRequestKey key{url, size};
    auto it = cachePaths.find(key);
    if (it != cachePaths.end()) return it->second;
    return cachePaths.emplace(std::move(key), tempDir / std::to_string(nextCachePath++)).first->second;
}
bool ImageManager::scalesToDevicePixels(const std::string &url) { return isDiscordProxyUrl(url) || Discord::Cdn::isEmojiUrl(url); }
bool ImageManager::isDiscordProxyUrl(const std::string &url)
{
    const auto host = Platform::urlHost(url);
    return host == "media.discordapp.net" || host.starts_with("images-ext-");
}
std::string ImageManager::buildOptimizedUrl(const std::string &url, const Platform::Size &size, float dpr)
{
    auto fragment = url.find('#');
    std::string result = url.substr(0, fragment);
    result += result.find('?') == std::string::npos ? '?' : '&';
    result += "format=webp&quality=lossless";
    if (!size.empty()) result += "&width=" + std::to_string(int(std::lround(size.width * dpr))) + "&height=" + std::to_string(int(std::lround(size.height * dpr)));
    if (fragment != std::string::npos) result += url.substr(fragment);
    return result;
}
std::string ImageManager::fullQualityUrl(const std::string &url)
{
    return isDiscordProxyUrl(url) ? buildOptimizedUrl(url, {}, 1.0f) : url;
}
void ImageManager::fetch(const std::string &url, Snowflake accountId, Platform::Trackable *context, std::function<void(const std::string &)> done)
{
    if (!hasRouteFor(accountId)) { done({}); return; }
    Platform::HttpRequest req;
    req.url = url;
    req.proxy = curlProxyFor(accountId);
    const bool hasContext = context != nullptr;
    auto lifetime = context ? context->lifetime() : std::weak_ptr<void>{};
    Platform::HttpClient::instance().request(std::move(req), [hasContext, lifetime, done = std::move(done)](const Platform::HttpResponse &resp) {
        if (hasContext && lifetime.expired()) return;
        if (done) done(resp.ok() ? resp.body : std::string{});
    });
}
void ImageManager::download(const std::string &url, Snowflake accountId, const std::string &path)
{
    if (!hasRouteFor(accountId)) return;
    Platform::HttpRequest req;
    req.url = url;
    req.proxy = curlProxyFor(accountId);
    Platform::HttpClient::instance().request(std::move(req), [url, path](const Platform::HttpResponse &resp) {
        if (!resp.ok() || !Platform::Resources::writeFile(std::filesystem::u8path(path), resp.body))
            Core::log(Core::LogLevel::Warning, LogCore) << "Download failed: " << url << " " << resp.error;
    });
}
} // namespace Styx::Core
