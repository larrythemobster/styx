#include "ImageManager.hpp"

#include "Platform/Http.hpp"
#include <QCryptographicHash>
#include <QFile>
#include <QPointer>
#include <QTimer>
#include <QUrlQuery>
#include <QApplication>

#include <memory>

#include "Logging.hpp"

#include "Discord/CdnUrls.hpp"

namespace Styx {
namespace Core {

namespace {
constexpr int MaxMemoizedPlaceholderPx = 64;
constexpr auto PartialDownloadSuffix = ".part";
constexpr int MaxFetchAttempts = 2;
constexpr int RetryBrokenTransferAfterMs = 500;
constexpr int HttpRequestTimeout = 408;
constexpr int HttpTooManyRequests = 429;

bool isErrorAnswer(int httpStatus)
{
    return httpStatus >= 400;
}

bool refusesForGood(int httpStatus)
{
    return isErrorAnswer(httpStatus) && httpStatus < 500 && httpStatus != HttpRequestTimeout && httpStatus != HttpTooManyRequests;
}

QByteArray fileContents(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

ImageManager::ImageManager(QObject *parent) : QObject(parent)
{
    cache.setMaxCost(300);
    uptime.start();

    if (!tempDir.isValid())
        qCWarning(LogCore) << "Failed to create temp directory for image cache";
}

ImageManager::~ImageManager()
{
    for (uint64_t reqId : activeHttpRequests) {
        Platform::HttpClient::instance().cancel(reqId);
    }
}

bool ImageManager::isCached(const QUrl &url, const QSize &size)
{
    ImageRequestKey k{ url, size };
    if (pinnedImages.contains(k) || cache.contains(k))
        return true;

    return !undecodable.contains(k) && !rawDownloadPath(url, size).isEmpty();
}

bool ImageManager::isUnavailable(const ImageRequestKey &key) const
{
    return willNeverLoad(key) || recentlyFailed(key);
}

bool ImageManager::willNeverLoad(const ImageRequestKey &key) const
{
    return undecodable.contains(key) || refusedByServer.contains(key);
}

QString ImageManager::rawDownloadPath(const QUrl &url, const QSize &size) const
{
    const QString path = getCachePath(url, size);
    return QFile::exists(path) ? path : QString();
}

void ImageManager::setAccountProxy(Snowflake accountId, const ProxyConfig &proxy)
{
    accountProxies.insert(accountId, proxy);
}

bool ImageManager::hasRouteFor(Snowflake accountId) const
{
    return accountProxies.contains(accountId);
}

ProxyConfig ImageManager::accountProxy(Snowflake accountId) const
{
    return accountProxies.value(accountId);
}

QString ImageManager::curlProxyFor(Snowflake accountId) const
{
    auto it = accountProxies.constFind(accountId);
    if (it == accountProxies.constEnd())
        return {};
    return it.value().toCurlProxyString();
}

void ImageManager::assign(QLabel *label, const QUrl &url, const QSize &size, Snowflake accountId)
{
    if (!label)
        return;

    // just in case
    disconnect(this, &ImageManager::imageFetched, label, nullptr);

    QPixmap pixmap = get(url, size, accountId);
    label->setPixmap(pixmap);

    if (!isCached(url, size)) {
        connect(this, &ImageManager::imageFetched, label,
                [=](const QUrl &u, const QSize &s, const QPixmap &p) {
                    if (u == url && s == size)
                        label->setPixmap(p);
                });
    }
}

QPixmap ImageManager::get(const QUrl &url, const QSize &size, Snowflake accountId, PinGroup pin)
{
    return getImpl(url, size, pin, true, accountId);
}

QPixmap ImageManager::getIfCached(const QUrl &url, const QSize &size)
{
    return getImpl(url, size, PinGroup::None, false, Snowflake());
}

QPixmap ImageManager::getImpl(const QUrl &url, const QSize &size, PinGroup pin, bool fetchIfNeeded, Snowflake accountId)
{
    ImageRequestKey k{ url, size };

    auto pinnedIt = pinnedImages.constFind(k);
    if (pinnedIt != pinnedImages.constEnd()) {
        if (pin != PinGroup::None && !pinGroupKeys.contains(pin, k))
            pinGroupKeys.insert(pin, k);
        return pinnedIt.value();
    }

    if (const QPixmap *cached = cache.object(k)) {
        QPixmap pixmap = *cached;
        if (pin != PinGroup::None) {
            pinnedImages.insert(k, pixmap);
            pinGroupKeys.insert(pin, k);
            cache.remove(k);
        }
        return pixmap;
    }

    if (willNeverLoad(k))
        return placeholder(size);

    if (requests.contains(k) || recentlyFailed(k)) {
        if (fetchIfNeeded)
            request(url, size, pin, accountId);
        return placeholder(size);
    }

    // check disk cache
    QString path = getCachePath(url, size);
    if (QFile::exists(path)) {
        QPixmap pixmap = QPixmap::fromImage(QImage(path));
        if (!pixmap.isNull()) {
            qreal dpr = qApp->devicePixelRatio();

            if (scalesToDevicePixels(url)) {
                QSize physicalSize(qRound(size.width() * dpr), qRound(size.height() * dpr));
                if (pixmap.size() != physicalSize)
                    pixmap = pixmap.scaled(physicalSize, Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation);
                pixmap.setDevicePixelRatio(dpr);
            } else {
                if (pixmap.size() != size)
                    pixmap = pixmap.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }

            if (pin != PinGroup::None) {
                pinnedImages.insert(k, pixmap);
                pinGroupKeys.insert(pin, k);
            } else {
                cache.insert(k, new QPixmap(pixmap));
            }
            return pixmap;
        }

        qCWarning(LogCore) << "Cached download is not a decodable image:" << url;
        undecodable.insert(k);
        return placeholder(size);
    }

    if (fetchIfNeeded) {
        request(url, size, pin, accountId);
    }

    return placeholder(size);
}

QPixmap ImageManager::placeholder(const QSize &size)
{
    const bool memoize = size.width() <= MaxMemoizedPlaceholderPx && size.height() <= MaxMemoizedPlaceholderPx;
    if (memoize) {
        auto it = placeholders.constFind(size);
        if (it != placeholders.constEnd())
            return it.value();
    }

    qreal dpr = qApp->devicePixelRatio();
    QSize physicalSize(qRound(size.width() * dpr), qRound(size.height() * dpr));
    QPixmap pixmap(physicalSize);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(QColor(60, 60, 60));
    if (memoize)
        placeholders.insert(size, pixmap);
    return pixmap;
}

bool ImageManager::recentlyFailed(const ImageRequestKey &key) const
{
    const auto it = failedAtMs.constFind(key);
    return it != failedAtMs.constEnd() && uptime.elapsed() - it.value() < FailedFetchRetryMs;
}

void ImageManager::downloadToCache(const QUrl &url, const QSize &size, Snowflake accountId)
{
    request(url, size, PinGroup::None, accountId, FetchPurpose::DownloadOnly);
}

void ImageManager::request(const QUrl &url, const QSize &size, PinGroup pin, Snowflake accountId, FetchPurpose purpose)
{
    if (!hasRouteFor(accountId)) {
        qCWarning(LogCore) << "Refusing to fetch image with no proxied route for the account:" << url;
        return;
    }

    ImageRequestKey k{ url, size };
    if (isUnavailable(k))
        return;

    if (requests.contains(k)) {
        if (purpose == FetchPurpose::Pixmap)
            downloadOnlyRequests.remove(k);

        // promote
        if (pin != PinGroup::None) {
            auto it = pendingPins.find(k);
            if (it == pendingPins.end() || it.value() == PinGroup::None)
                pendingPins.insert(k, pin);
        }
        return;
    }

    requests.insert(k);
    if (purpose == FetchPurpose::DownloadOnly)
        downloadOnlyRequests.insert(k);
    if (pin != PinGroup::None)
        pendingPins.insert(k, pin);

    const QString proxyStr = curlProxyFor(accountId);
    fetchFromNetwork(url, size, proxyStr, purpose);
}

void ImageManager::giveUpOn(const ImageRequestKey &key, int httpStatus)
{
    pendingPins.remove(key);
    requests.remove(key);
    downloadOnlyRequests.remove(key);
    if (refusesForGood(httpStatus))
        refusedByServer.insert(key);
    else
        failedAtMs.insert(key, uptime.elapsed());
    emit imageUnavailable(key.url, key.size);
}

void ImageManager::fetchFromNetwork(const QUrl &url, const QSize &size, const QString &proxyStr, FetchPurpose purpose, int attempt)
{
    qreal dpr = qApp->devicePixelRatio();
    const bool deviceScaled = scalesToDevicePixels(url);

    // the format override in the optimized query would break an animated sticker
    const bool optimizable = purpose == FetchPurpose::Pixmap && isDiscordProxyUrl(url) && !Discord::Cdn::isStickerUrl(url);
    QUrl fetchUrl = optimizable ? buildOptimizedUrl(url, size, dpr) : url;

    Platform::HttpRequest req;
    req.url = fetchUrl.toString().toStdString();
    if (!proxyStr.isEmpty()) {
        req.proxy = proxyStr.toStdString();
    }
    req.timeout = std::chrono::milliseconds(30000);

    const QString path = getCachePath(url, size);
    ImageRequestKey k{ url, size };

    uint64_t reqId = Platform::HttpClient::instance().request(std::move(req), [this, url, size, deviceScaled, dpr, path, purpose, attempt, proxyStr](const Platform::HttpResponse &resp) {
        ImageRequestKey k{ url, size };
        activeHttpRequests.remove(k);

        bool fetched = resp.ok();
        if (fetched && purpose == FetchPurpose::DownloadOnly) {
            const QString partPath = path + PartialDownloadSuffix;
            QFile partFile(partPath);
            if (partFile.open(QIODevice::WriteOnly)) {
                partFile.write(resp.body.data(), static_cast<qint64>(resp.body.size()));
                partFile.close();
                QFile::remove(path);
                fetched = partFile.rename(path);
            } else {
                fetched = false;
            }
        }

        if (!fetched) {
            const int httpStatus = resp.statusCode;
            qCWarning(LogCore) << "Failed to fetch image:" << url << QString::fromStdString(resp.error);
            if (purpose == FetchPurpose::DownloadOnly) {
                QFile::remove(path + PartialDownloadSuffix);
            }

            const bool transferBroke = !resp.ok() && !isErrorAnswer(httpStatus);
            if (!transferBroke || attempt >= MaxFetchAttempts) {
                giveUpOn(k, httpStatus);
                return;
            }

            QTimer::singleShot(RetryBrokenTransferAfterMs, this, [this, k, proxyStr, purpose, attempt]() {
                fetchFromNetwork(k.url, k.size, proxyStr, purpose, attempt + 1);
            });
            return;
        }

        PinGroup pin = pendingPins.value(k, PinGroup::None);
        pendingPins.remove(k);
        failedAtMs.remove(k);

        const bool pixmapWanted = !downloadOnlyRequests.remove(k);

        QByteArray data;
        if (purpose == FetchPurpose::DownloadOnly) {
            if (pixmapWanted)
                data = fileContents(path);
        } else {
            data = QByteArray(resp.body.data(), static_cast<int>(resp.body.size()));
            QFile file(path);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(data);
                file.close();
            }
        }

        QPixmap pixmap;
        if (pixmapWanted && pixmap.loadFromData(data)) {
            if (deviceScaled) {
                QSize physicalSize(qRound(size.width() * dpr), qRound(size.height() * dpr));
                if (pixmap.size() != physicalSize)
                    pixmap = pixmap.scaled(physicalSize, Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation);
                pixmap.setDevicePixelRatio(dpr);
            } else {
                pixmap = pixmap.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }

            if (pin != PinGroup::None) {
                pinnedImages.insert(k, pixmap);
                pinGroupKeys.insert(pin, k);
            } else {
                cache.insert(k, new QPixmap(pixmap));
            }

            requests.remove(k);
            emit imageFetched(url, size, pixmap);
        } else {
            requests.remove(k);
            if (pixmapWanted) {
                qCWarning(LogCore) << "Fetched image is not decodable:" << url;
                undecodable.insert(k);
                emit imageUnavailable(url, size);
            }
        }
        emit rawDownloadReady(url, size);
    });

    activeHttpRequests.insert(k, reqId);
}

void ImageManager::unpinGroup(PinGroup group)
{
    if (group == PinGroup::None)
        return;

    QList<ImageRequestKey> keys = pinGroupKeys.values(group);
    pinGroupKeys.remove(group);

    for (const auto &k : keys) {
        // the same url could be pinned by multiple groups
        bool stillPinned = false;
        for (auto it = pinGroupKeys.cbegin(); it != pinGroupKeys.cend(); ++it) {
            if (it.value() == k) {
                stillPinned = true;
                break;
            }
        }

        if (!stillPinned) {
            auto it = pinnedImages.find(k);
            if (it != pinnedImages.end()) {
                // send it back to the lru
                cache.insert(k, new QPixmap(it.value()));
                pinnedImages.erase(it);
            }
        }
    }
}

QSize ImageManager::calculateDisplaySize(const QSize &original)
{
    if (!original.isValid() || original.isEmpty())
        return QSize(MaxDisplayWidth, MaxDisplayHeight);

    if (original.width() <= MaxDisplayWidth && original.height() <= MaxDisplayHeight)
        return original;

    return original.scaled(MaxDisplayWidth, MaxDisplayHeight, Qt::KeepAspectRatio);
}

QString ImageManager::getCachePath(const QUrl &url, const QSize &size) const
{
    QString compound = url.toString() + QStringLiteral(":%1x%2").arg(size.width()).arg(size.height());
    QByteArray hash =
            QCryptographicHash::hash(compound.toUtf8(), QCryptographicHash::Sha1);
    QString filename = QString::fromLatin1(hash.toHex());
    return tempDir.filePath(filename);
}

bool ImageManager::scalesToDevicePixels(const QUrl &url)
{
    return isDiscordProxyUrl(url) || Discord::Cdn::isEmojiUrl(url);
}

bool ImageManager::isDiscordProxyUrl(const QUrl &url)
{
    QString host = url.host();
    return host == u"media.discordapp.net" || host.startsWith(u"images-ext-");
}

QUrl ImageManager::buildOptimizedUrl(const QUrl &proxyUrl, const QSize &displaySize, qreal dpr)
{
    QUrl optimized = proxyUrl;
    QUrlQuery query(optimized);

    query.addQueryItem("format", "webp");
    query.addQueryItem("quality", "lossless");

    if (displaySize.isValid() && !displaySize.isEmpty()) {
        int physicalWidth = qRound(displaySize.width() * dpr);
        int physicalHeight = qRound(displaySize.height() * dpr);
        query.addQueryItem("width", QString::number(physicalWidth));
        query.addQueryItem("height", QString::number(physicalHeight));
    }

    optimized.setQuery(query);
    return optimized;
}

QUrl ImageManager::fullQualityUrl(const QUrl &proxyUrl)
{
    return isDiscordProxyUrl(proxyUrl) ? buildOptimizedUrl(proxyUrl, QSize(), 1.0) : proxyUrl;
}

void ImageManager::fetch(const QUrl &url, Snowflake accountId, QObject *context, std::function<void(const QByteArray &)> done)
{
    if (!hasRouteFor(accountId)) {
        qCWarning(LogCore) << "Refusing to fetch with no proxied route for the account:" << url;
        done({});
        return;
    }

    Platform::HttpRequest req;
    req.url = url.toString().toStdString();
    QString proxyStr = curlProxyFor(accountId);
    if (!proxyStr.isEmpty()) {
        req.proxy = proxyStr.toStdString();
    }
    req.timeout = std::chrono::milliseconds(30000);

    QPointer<QObject> ctx(context);
    Platform::HttpClient::instance().request(std::move(req), [ctx, done = std::move(done)](const Platform::HttpResponse &resp) {
        if (!ctx && !done)
            return;
        done(resp.ok() ? QByteArray(resp.body.data(), static_cast<int>(resp.body.size())) : QByteArray());
    });
}

void ImageManager::download(const QUrl &url, Snowflake accountId, const QString &path)
{
    if (!hasRouteFor(accountId)) {
        qCWarning(LogCore) << "Refusing to download with no proxied route for the account:" << url;
        return;
    }

    Platform::HttpRequest req;
    req.url = url.toString().toStdString();
    QString proxyStr = curlProxyFor(accountId);
    if (!proxyStr.isEmpty()) {
        req.proxy = proxyStr.toStdString();
    }
    req.timeout = std::chrono::milliseconds(30000);

    Platform::HttpClient::instance().request(std::move(req), [path, url](const Platform::HttpResponse &resp) {
        if (!resp.ok()) {
            qCWarning(LogCore) << "Download failed:" << url << QString::fromStdString(resp.error);
            return;
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qCWarning(LogCore) << "Failed to open" << path << "for writing:" << file.errorString();
            return;
        }
        file.write(resp.body.data(), static_cast<qint64>(resp.body.size()));
        file.close();
    });
}

} // namespace Core
} // namespace Styx
