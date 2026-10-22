#include "RmlRuntime.hpp"

#include "RmlRender.hpp"

#include "Core/EmojiSegmenter.hpp"
#include "Core/ImageManager.hpp"
#include "Core/Logging.hpp"

#include <RmlUi/Core.h>

#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QGuiApplication>
#include <QPainter>
#include <QStandardPaths>
#include <QSvgRenderer>

#include <utility>

namespace Acheron {
namespace UI {
namespace Rml {

// ---------------------------------------------------------------------------------------
// System interface: time, logging, clipboard, cursor

class Runtime::System final : public ::Rml::SystemInterface
{
public:
    System() { clock.start(); }

    double GetElapsedTime() override { return clock.nsecsElapsed() / 1e9; }

    bool LogMessage(::Rml::Log::Type type, const ::Rml::String &message) override
    {
        const QString text = QString::fromStdString(message);
        switch (type) {
        case ::Rml::Log::LT_ERROR:
        case ::Rml::Log::LT_ASSERT:
            qCWarning(LogUI) << "RmlUi:" << text;
            break;
        case ::Rml::Log::LT_WARNING:
            qCWarning(LogUI) << "RmlUi:" << text;
            break;
        default:
            qCDebug(LogUI) << "RmlUi:" << text;
            break;
        }
        return true;
    }

    void SetMouseCursor(const ::Rml::String &name) override { cursorName = QString::fromStdString(name); }
    void SetClipboardText(const ::Rml::String &text) override { QGuiApplication::clipboard()->setText(QString::fromStdString(text)); }
    void GetClipboardText(::Rml::String &text) override { text = QGuiApplication::clipboard()->text().toStdString(); }

    QString cursorName;

private:
    QElapsedTimer clock;
};

// ---------------------------------------------------------------------------------------
// File interface: documents and style sheets live in Qt resources (:/ui/...)

class Runtime::Files final : public ::Rml::FileInterface
{
public:
    ::Rml::FileHandle Open(const ::Rml::String &path) override
    {
        QString name = QString::fromStdString(path);
        if (!name.startsWith(QLatin1String(":/")) && !QFile::exists(name))
            name = QStringLiteral(":/ui/") + name;
        auto *file = new QFile(name);
        if (!file->open(QIODevice::ReadOnly)) {
            qCWarning(LogUI) << "RmlUi: cannot open" << name;
            delete file;
            return 0;
        }
        return reinterpret_cast<::Rml::FileHandle>(file);
    }
    void Close(::Rml::FileHandle handle) override { delete reinterpret_cast<QFile *>(handle); }
    size_t Read(void *buffer, size_t size, ::Rml::FileHandle handle) override
    {
        const qint64 n = reinterpret_cast<QFile *>(handle)->read(static_cast<char *>(buffer), static_cast<qint64>(size));
        return n < 0 ? 0 : static_cast<size_t>(n);
    }
    bool Seek(::Rml::FileHandle handle, long offset, int origin) override
    {
        auto *file = reinterpret_cast<QFile *>(handle);
        qint64 base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? file->pos() : file->size();
        return file->seek(base + offset);
    }
    size_t Tell(::Rml::FileHandle handle) override { return static_cast<size_t>(reinterpret_cast<QFile *>(handle)->pos()); }
};

// ---------------------------------------------------------------------------------------

::Rml::TextureHandle RenderInterface::LoadTexture(::Rml::Vector2i &dimensions, const ::Rml::String &source)
{
    QImage image = Runtime::instance().loadImage(QString::fromStdString(source));
    if (image.isNull())
        return 0;
    image = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    dimensions = ::Rml::Vector2i(image.width(), image.height());
    // GenerateTexture wants tightly packed rows
    QByteArray packed;
    packed.resize(static_cast<qsizetype>(image.width()) * image.height() * 4);
    for (int y = 0; y < image.height(); ++y)
        memcpy(packed.data() + static_cast<qsizetype>(y) * image.width() * 4, image.constScanLine(y), static_cast<size_t>(image.width()) * 4);
    return GenerateTexture(::Rml::Span<const ::Rml::byte>(reinterpret_cast<const ::Rml::byte *>(packed.constData()), static_cast<size_t>(packed.size())),
                           dimensions);
}

void RenderInterface::ReleaseGeometry(::Rml::CompiledGeometryHandle handle)
{
    if (ownerContext && QOpenGLContext::currentContext() == ownerContext.data())
        RenderInterface_GL3::ReleaseGeometry(handle);
    else
        releasedGeometry.push_back(handle);
}

void RenderInterface::ReleaseTexture(::Rml::TextureHandle handle)
{
    if (ownerContext && QOpenGLContext::currentContext() == ownerContext.data())
        RenderInterface_GL3::ReleaseTexture(handle);
    else
        releasedTextures.push_back(handle);
}

void RenderInterface::flushReleasedResources()
{
    Q_ASSERT(ownerContext && QOpenGLContext::currentContext() == ownerContext.data());
    for (auto handle : releasedGeometry)
        RenderInterface_GL3::ReleaseGeometry(handle);
    releasedGeometry.clear();
    for (auto handle : releasedTextures)
        RenderInterface_GL3::ReleaseTexture(handle);
    releasedTextures.clear();
}

// ---------------------------------------------------------------------------------------

Runtime &Runtime::instance()
{
    static Runtime runtime;
    return runtime;
}

Runtime::~Runtime() = default;

QString Runtime::cursorName() const
{
    return system ? system->cursorName : QString();
}
bool Runtime::initialise(QString *error)
{
    if (initialised)
        return true;

    ::Rml::String message;
    if (!RmlGL3::Initialize(&message)) {
        if (error)
            *error = QString::fromStdString(message);
        return false;
    }
    qCInfo(LogUI) << "RmlUi renderer:" << QString::fromStdString(message);

    system = std::make_unique<System>();
    files = std::make_unique<Files>();
    render = std::make_unique<RenderInterface>();
    if (!*render) {
        if (error)
            *error = QStringLiteral("RmlUi GL3 shaders failed to compile");
        return false;
    }

    ::Rml::SetSystemInterface(system.get());
    ::Rml::SetFileInterface(files.get());
    ::Rml::SetRenderInterface(render.get());
    if (!::Rml::Initialise()) {
        if (error)
            *error = QStringLiteral("Rml::Initialise failed");
        return false;
    }
    loadFonts();
    initialised = true;
    return true;
}

void Runtime::shutdown()
{
    if (initialised) {
        ::Rml::Shutdown();
        initialised = false;
    }
    if (render) {
        render->flushReleasedResources();
        render.reset();
        RmlGL3::Shutdown();
    }
    fontData.clear();
    files.reset();
    system.reset();
}

void Runtime::loadFonts()
{
    // Discord's own face (gg sans) is not redistributable; the platform UI font is the
    // closest match and is what Discord falls back to.
    struct Face
    {
        const char *file;
        const char *family;
        ::Rml::Style::FontStyle style;
        ::Rml::Style::FontWeight weight;
        bool fallback;
    };
    using S = ::Rml::Style::FontStyle;
    using W = ::Rml::Style::FontWeight;
    static const Face faces[] = {
        { "segoeui.ttf", "ui", S::Normal, W::Normal, false },
        { "segoeuii.ttf", "ui", S::Italic, W::Normal, false },
        { "seguisb.ttf", "ui", S::Normal, W(600), false },
        { "segoeuib.ttf", "ui", S::Normal, W::Bold, false },
        { "segoeuiz.ttf", "ui", S::Italic, W::Bold, false },
        { "consola.ttf", "mono", S::Normal, W::Normal, false },
        { "consolab.ttf", "mono", S::Normal, W::Bold, false },
        { "seguisym.ttf", "symbols", S::Normal, W::Normal, true },
        { "msyh.ttc", "cjk", S::Normal, W::Normal, true },
    };

    const QString fontsDir = QDir(qEnvironmentVariable("WINDIR", QStringLiteral("C:/Windows"))).filePath(QStringLiteral("Fonts"));
    for (const Face &face : faces) {
        QFile file(QDir(fontsDir).filePath(QString::fromLatin1(face.file)));
        if (!file.open(QIODevice::ReadOnly)) {
            qCWarning(LogUI) << "RmlUi: font missing" << file.fileName();
            continue;
        }
        fontData.append(file.readAll());
        const QByteArray &data = fontData.last();
        if (!::Rml::LoadFontFace(::Rml::Span<const ::Rml::byte>(reinterpret_cast<const ::Rml::byte *>(data.constData()), static_cast<size_t>(data.size())),
                                 face.family, face.style, face.weight, face.fallback))
            qCWarning(LogUI) << "RmlUi: could not load" << face.file;
    }
}

void Runtime::setImageManager(Core::ImageManager *manager)
{
    if (images == manager)
        return;
    if (images)
        disconnect(images, nullptr, this, nullptr);
    images = manager;
    if (images)
        connect(images, &Core::ImageManager::imageFetched, this,
                [this](const QUrl &url, const QSize &size, const QPixmap &) { onImageFetched(url, size); });
}

QString Runtime::imageSource(const QUrl &url, const QSize &logicalSize, Core::Snowflake accountId)
{
    if (!url.isValid())
        return {};
    const QString key = url.toString() + QLatin1Char('|') + QString::number(logicalSize.width()) + QLatin1Char('x')
                        + QString::number(logicalSize.height());
    auto it = imageIds.constFind(key);
    int id;
    if (it == imageIds.constEnd()) {
        id = static_cast<int>(imageIds.size()) + 1;
        imageIds.insert(key, id);
        imageRequests.insert(id, ImageRequest{ url, logicalSize, accountId });
    } else {
        id = it.value();
    }
    return QStringLiteral("img:%1").arg(id);
}

void Runtime::onImageFetched(const QUrl &url, const QSize &size)
{
    const QString key = url.toString() + QLatin1Char('|') + QString::number(size.width()) + QLatin1Char('x') + QString::number(size.height());
    auto it = imageIds.constFind(key);
    if (it == imageIds.constEnd() || !initialised)
        return;
    changedTextures.insert(QStringLiteral("img:%1").arg(it.value()));
    emit texturesChanged();
}

void Runtime::setFrame(const QString &key, const QImage &frame)
{
    const auto previous = frames.constFind(key);
    if (previous == frames.constEnd() || previous->size() != frame.size())
        textureSizesChanged = true;
    frames.insert(key, frame);
    changedTextures.insert(QStringLiteral("frame:") + key);
    emit texturesChanged();
}

void Runtime::clearFrame(const QString &key)
{
    frames.remove(key);
    changedTextures.insert(QStringLiteral("frame:") + key);
    emit texturesChanged();
}

bool Runtime::releaseChangedTextures()
{
    for (const QString &source : std::as_const(changedTextures))
        ::Rml::ReleaseTexture(source.toStdString());
    changedTextures.clear();
    return std::exchange(textureSizesChanged, false);
}

QString Runtime::emojiSource(const QString &emoji)
{
    QStringList parts;
    for (char32_t cp : emoji.toUcs4())
        parts << QString::number(cp, 16);
    return QStringLiteral("emoji:") + parts.join(QLatin1Char('-'));
}

QImage Runtime::loadImage(const QString &source)
{
    if (source.startsWith(QLatin1String("img:"))) {
        const auto it = imageRequests.constFind(source.mid(4).toInt());
        if (it == imageRequests.constEnd() || !images)
            return {};
        if (!images->isCached(it->url, it->size)) {
            images->get(it->url, it->size, it->accountId);
            // RmlUi caches failed loads permanently. Keep a valid texture until fetch completes.
            QImage pending(it->size, QImage::Format_RGBA8888_Premultiplied);
            pending.fill(Qt::transparent);
            return pending;
        }
        const QPixmap cached = images->get(it->url, it->size, it->accountId);
        if (!cached.isNull())
            return cached.toImage();
        return {};
    }

    if (source.startsWith(QLatin1String("frame:"))) {
        const auto it = frames.constFind(source.mid(6));
        if (it != frames.constEnd() && !it->isNull())
            return it.value();
        QImage pending(1, 1, QImage::Format_RGBA8888_Premultiplied);
        pending.fill(Qt::transparent);
        return pending;
    }

    if (source.startsWith(QLatin1String("icon:"))) {
        QFile file(QStringLiteral(":/resources/icons/lucide/%1.svg").arg(source.mid(5)));
        if (!file.open(QIODevice::ReadOnly))
            return {};
        QByteArray svg = file.readAll();
        svg.replace("currentColor", "#ffffff");
        QSvgRenderer renderer(svg);
        QImage image(48, 48, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        renderer.render(&painter);
        return image;
    }

    if (source.startsWith(QLatin1String("emoji:"))) {
        QString text;
        for (const QString &hex : source.mid(6).split(QLatin1Char('-'))) {
            const char32_t cp = hex.toUInt(nullptr, 16);
            text += QString::fromUcs4(&cp, 1);
        }
        QImage image(64, 64, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::TextAntialiasing);
        QFont font(QStringLiteral("Twemoji"));
        font.setPixelSize(52);
        painter.setFont(font);
        painter.drawText(image.rect(), Qt::AlignCenter, text);
        return image;
    }

    // a Qt resource or file path
    QImage image(source.startsWith(QLatin1String(":/")) || QFile::exists(source) ? source : QStringLiteral(":/") + source);
    return image;
}

// ---------------------------------------------------------------------------------------

QString escape(const QString &plain)
{
    QString out = plain.toHtmlEscaped();
    out.replace(QLatin1Char('\n'), QLatin1String("<br/>"));
    return out;
}

QString richText(const QString &plain)
{
    const QStringList emoji = Core::extractEmojiSequences(plain);
    if (emoji.isEmpty())
        return escape(plain);

    QString out;
    qsizetype pos = 0;
    for (const QString &sequence : emoji) {
        const qsizetype at = plain.indexOf(sequence, pos);
        if (at < 0)
            continue;
        out += escape(plain.mid(pos, at - pos));
        out += QStringLiteral("<img class=\"emoji\" src=\"%1\"/>").arg(Runtime::emojiSource(sequence));
        pos = at + sequence.size();
    }
    out += escape(plain.mid(pos));
    return out;
}

} // namespace Rml
} // namespace UI
} // namespace Acheron
