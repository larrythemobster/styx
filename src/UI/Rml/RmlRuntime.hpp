#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QSize>
#include <QString>
#include <QUrl>

#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/SystemInterface.h>

#include <memory>

#include "Core/Snowflake.hpp"

class QWidget;

namespace Styx {
namespace Core {
class ImageManager;
}
namespace UI {
namespace Rml {

class RenderInterface;

// Process-wide RmlUi state: interfaces, fonts and the texture sources the documents use.
//
// Texture sources understood by <img src> / decorators:
//   :/path.png          Qt resource (any QImage format)
//   icon:<name>         a Lucide icon from :/resources/icons/lucide, rasterised white
//                       (tint it with RCSS image-color)
//   emoji:<hex-hex>     a Unicode emoji, rasterised from the bundled Twemoji font
//   img:<n>             a remote image registered with imageSource()
//   frame:<key>         a live video frame pushed with setFrame()
class Runtime : public QObject
{
    Q_OBJECT
public:
    static Runtime &instance();
    ~Runtime() override;

    // Needs a current OpenGL 3.3 context; called by the first View.
    bool initialise(QString *error);
    // Called by the last View before its OpenGL context is destroyed.
    void shutdown();
    [[nodiscard]] bool isInitialised() const { return initialised; }
    RenderInterface *renderer() const { return render.get(); }
    // Cursor the hovered element asked for (RCSS `cursor`), e.g. "pointer", "text".
    [[nodiscard]] QString cursorName() const;

    void setImageManager(Core::ImageManager *manager);

    // Source string for a remote image; the image is fetched on first use and the
    // element redraws when it arrives.
    QString imageSource(const QUrl &url, const QSize &logicalSize, Core::Snowflake accountId);
    static QString emojiSource(const QString &emoji);
    static QString iconSource(const QString &name) { return QStringLiteral("icon:") + name; }

    // Live video frames (Go Live). The element showing frame:<key> redraws on each call.
    void setFrame(const QString &key, const QImage &frame);
    void clearFrame(const QString &key);

    // Resolves a texture source to an image (called by the renderer on demand).
    QImage loadImage(const QString &source);

    // Needs a current OpenGL context. Returns whether frame dimensions changed.
    bool releaseChangedTextures();

signals:
    // A texture changed; views should repaint.
    void texturesChanged();

private:
    Runtime() = default;
    void loadFonts();
    void onImageFetched(const QUrl &url, const QSize &size);

    struct ImageRequest
    {
        QUrl url;
        QSize size;
        Core::Snowflake accountId;
    };

    class System;
    class Files;

    bool initialised = false;
    std::unique_ptr<System> system;
    std::unique_ptr<Files> files;
    std::unique_ptr<RenderInterface> render;
    QPointer<Core::ImageManager> images;

    QHash<QString, int> imageIds; // "url|w|h" -> id
    QHash<int, ImageRequest> imageRequests;
    QHash<QString, QImage> frames;
    QSet<QString> changedTextures;
    bool textureSizesChanged = false;
    QList<QByteArray> fontData; // FreeType reads faces straight from these buffers
};

// Escapes text for RML and turns Unicode emoji into inline <img class="emoji">.
QString richText(const QString &plain);
QString escape(const QString &plain);

} // namespace Rml
} // namespace UI
} // namespace Styx
