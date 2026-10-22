#pragma once

#include <QCoreApplication>
#include <QList>
#include <QMetaType>
#include <QSize>
#include <QString>

namespace Acheron {
namespace Core {
namespace Stream {

struct StreamQuality
{
    QString id;
    QSize size;
    int framerate = 30;
    int bitrate = 0;
    int maxBitrate = 0;
    bool needsNitro = false;

    [[nodiscard]] QString label() const
    {
        QString text = QStringLiteral("%1p %2 fps").arg(size.height()).arg(framerate);
        if (needsNitro)
            text += QCoreApplication::translate("StreamQuality", " (Nitro)");
        return text;
    }

    // Discord lets accounts without Nitro stream 720p30; the media server is the judge of
    // the rest, so higher presets are offered but labelled.
    static QList<StreamQuality> presets()
    {
        return {
            { QStringLiteral("720p30"), QSize(1280, 720), 30, 2'500'000, 4'000'000, false },
            { QStringLiteral("720p60"), QSize(1280, 720), 60, 4'000'000, 6'000'000, true },
            { QStringLiteral("1080p30"), QSize(1920, 1080), 30, 4'500'000, 6'500'000, true },
            { QStringLiteral("1080p60"), QSize(1920, 1080), 60, 7'000'000, 9'000'000, true },
        };
    }

    static StreamQuality byId(const QString &id)
    {
        const auto all = presets();
        for (const auto &preset : all) {
            if (preset.id == id)
                return preset;
        }
        return all.first();
    }
};

} // namespace Stream
} // namespace Core
} // namespace Acheron

Q_DECLARE_METATYPE(Acheron::Core::Stream::StreamQuality)
