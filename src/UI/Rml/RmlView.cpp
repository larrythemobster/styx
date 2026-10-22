#include "RmlView.hpp"

#include "RmlRender.hpp"
#include "RmlRuntime.hpp"

#include "Core/Logging.hpp"

#include <RmlUi/Core.h>

#include <QApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSurfaceFormat>
#include <QWheelEvent>

namespace Acheron {
namespace UI {
namespace Rml {

namespace KI = ::Rml::Input;

namespace {

KI::KeyIdentifier mapKey(int key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return static_cast<KI::KeyIdentifier>(KI::KI_A + (key - Qt::Key_A));
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return static_cast<KI::KeyIdentifier>(KI::KI_0 + (key - Qt::Key_0));
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12)
        return static_cast<KI::KeyIdentifier>(KI::KI_F1 + (key - Qt::Key_F1));
    switch (key) {
    case Qt::Key_Backspace: return KI::KI_BACK;
    case Qt::Key_Tab: return KI::KI_TAB;
    case Qt::Key_Backtab: return KI::KI_TAB;
    case Qt::Key_Return: return KI::KI_RETURN;
    case Qt::Key_Enter: return KI::KI_NUMPADENTER;
    case Qt::Key_Escape: return KI::KI_ESCAPE;
    case Qt::Key_Space: return KI::KI_SPACE;
    case Qt::Key_Delete: return KI::KI_DELETE;
    case Qt::Key_Insert: return KI::KI_INSERT;
    case Qt::Key_Home: return KI::KI_HOME;
    case Qt::Key_End: return KI::KI_END;
    case Qt::Key_PageUp: return KI::KI_PRIOR;
    case Qt::Key_PageDown: return KI::KI_NEXT;
    case Qt::Key_Left: return KI::KI_LEFT;
    case Qt::Key_Right: return KI::KI_RIGHT;
    case Qt::Key_Up: return KI::KI_UP;
    case Qt::Key_Down: return KI::KI_DOWN;
    case Qt::Key_Shift: return KI::KI_LSHIFT;
    case Qt::Key_Control: return KI::KI_LCONTROL;
    case Qt::Key_Alt: return KI::KI_LMENU;
    case Qt::Key_Meta: return KI::KI_LMETA;
    case Qt::Key_Minus: return KI::KI_OEM_MINUS;
    case Qt::Key_Equal: return KI::KI_OEM_PLUS;
    case Qt::Key_Comma: return KI::KI_OEM_COMMA;
    case Qt::Key_Period: return KI::KI_OEM_PERIOD;
    case Qt::Key_Slash: return KI::KI_OEM_2;
    case Qt::Key_Semicolon: return KI::KI_OEM_1;
    default: return KI::KI_UNKNOWN;
    }
}

int mapButton(Qt::MouseButton button)
{
    switch (button) {
    case Qt::LeftButton: return 0;
    case Qt::RightButton: return 1;
    case Qt::MiddleButton: return 2;
    default: return -1;
    }
}

} // namespace

View::View(const QString &contextName, QWidget *parent) : QOpenGLWidget(parent), name(contextName)
{
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setStencilBufferSize(8);
    format.setDepthBufferSize(0);
    setFormat(format);

    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled);
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);

    animationTimer.setInterval(16);
    animationTimer.setTimerType(Qt::PreciseTimer);
    connect(&animationTimer, &QTimer::timeout, this, [this] {
        update();
        if (animationClock.elapsed() >= animateUntilMs)
            animationTimer.stop();
    });
    animationClock.start();

    connect(&Runtime::instance(), &Runtime::texturesChanged, this, [this] { requestRender(); });
}

View::~View()
{
    if (!QOpenGLWidget::context())
        return;
    makeCurrent();
    if (rmlContext) {
        ::Rml::RemoveContext(name.toStdString());
        rmlContext = nullptr;
        rmlDocument = nullptr;
    }
    auto &runtime = Runtime::instance();
    if (runtime.renderer()) {
        if (!runtime.isInitialised() || ::Rml::GetNumContexts() == 0)
            runtime.shutdown();
        else
            runtime.renderer()->flushReleasedResources();
    }
    doneCurrent();
}

void View::requestRender(int animateMs)
{
    update();
    if (animateMs > 0) {
        animateUntilMs = std::max(animateUntilMs, animationClock.elapsed() + animateMs);
        if (!animationTimer.isActive())
            animationTimer.start();
    }
}

void View::loadDocument(const QString &path)
{
    if (!rmlContext) {
        pendingDocument = path;
        return;
    }
    if (rmlDocument)
        rmlDocument->Close();
    rmlDocument = rmlContext->LoadDocument(path.toStdString());
    if (!rmlDocument) {
        qCWarning(LogUI) << "RmlUi: could not load" << path;
        return;
    }
    rmlDocument->Show();
    emit documentLoaded(rmlDocument);
    requestRender();
}

void View::initializeGL()
{
    QString error;
    if (!Runtime::instance().initialise(&error)) {
        qCCritical(LogUI) << "RmlUi initialisation failed:" << error;
        emit initialisationFailed(error);
        return;
    }
    const qreal dpr = devicePixelRatioF();
    rmlContext = ::Rml::CreateContext(name.toStdString(), ::Rml::Vector2i(int(width() * dpr), int(height() * dpr)));
    if (!rmlContext) {
        emit initialisationFailed(QStringLiteral("could not create the RmlUi context"));
        return;
    }
    rmlContext->SetDensityIndependentPixelRatio(static_cast<float>(dpr));
    if (!pendingDocument.isEmpty()) {
        const QString path = pendingDocument;
        pendingDocument.clear();
        loadDocument(path);
    }
}

void View::resizeGL(int w, int h)
{
    if (!rmlContext)
        return;
    const qreal dpr = devicePixelRatioF();
    const ::Rml::Vector2i size(int(w * dpr), int(h * dpr));
    rmlContext->SetDimensions(size);
    rmlContext->SetDensityIndependentPixelRatio(static_cast<float>(dpr));
    Runtime::instance().renderer()->SetViewport(size.x, size.y);
}

void View::paintGL()
{
    if (!rmlContext)
        return;
    auto *renderer = Runtime::instance().renderer();
    renderer->flushReleasedResources();
    const qreal dpr = devicePixelRatioF();
    renderer->SetViewport(int(width() * dpr), int(height() * dpr));
    if (Runtime::instance().releaseChangedTextures() && rmlDocument) {
        ::Rml::ElementList images;
        rmlDocument->GetElementsByTagName(images, "img");
        for (auto *image : images) {
            const auto source = image->GetAttribute<::Rml::String>("src", "");
            if (source.starts_with("frame:"))
                image->SetAttribute("src", source);
        }
    }
    rmlContext->Update();
    renderer->BeginFrame();
    renderer->Clear();
    rmlContext->Render();
    renderer->EndFrame();
    applyCursor();
}

void View::applyCursor()
{
    const QString cursor = Runtime::instance().cursorName();
    if (cursor == lastCursor)
        return;
    lastCursor = cursor;
    if (cursor == QLatin1String("pointer"))
        setCursor(Qt::PointingHandCursor);
    else if (cursor == QLatin1String("text"))
        setCursor(Qt::IBeamCursor);
    else if (cursor == QLatin1String("move"))
        setCursor(Qt::SizeAllCursor);
    else if (cursor == QLatin1String("resize") || cursor == QLatin1String("ew-resize"))
        setCursor(Qt::SizeHorCursor);
    else if (cursor == QLatin1String("not-allowed"))
        setCursor(Qt::ForbiddenCursor);
    else
        unsetCursor();
}

int View::modifiers() const
{
    const Qt::KeyboardModifiers m = QApplication::keyboardModifiers();
    int out = 0;
    if (m & Qt::ControlModifier)
        out |= KI::KM_CTRL;
    if (m & Qt::ShiftModifier)
        out |= KI::KM_SHIFT;
    if (m & Qt::AltModifier)
        out |= KI::KM_ALT;
    if (m & Qt::MetaModifier)
        out |= KI::KM_META;
    return out;
}

void View::mouseMoveEvent(QMouseEvent *event)
{
    if (!rmlContext)
        return;
    const QPointF p = event->position() * devicePixelRatioF();
    rmlContext->ProcessMouseMove(int(p.x()), int(p.y()), modifiers());
    requestRender(250);
}

void View::mousePressEvent(QMouseEvent *event)
{
    if (!rmlContext)
        return;
    const int button = mapButton(event->button());
    if (button >= 0)
        rmlContext->ProcessMouseButtonDown(button, modifiers());
    requestRender(300);
}

void View::mouseReleaseEvent(QMouseEvent *event)
{
    if (!rmlContext)
        return;
    const int button = mapButton(event->button());
    if (button >= 0)
        rmlContext->ProcessMouseButtonUp(button, modifiers());
    requestRender(300);
}

void View::mouseDoubleClickEvent(QMouseEvent *event)
{
    // RmlUi detects double clicks itself from down/up timing
    mousePressEvent(event);
}

void View::wheelEvent(QWheelEvent *event)
{
    if (!rmlContext)
        return;
    // one notch (120) = one RmlUi wheel unit; pixel deltas from touchpads scale down
    QPointF delta = event->angleDelta() / 120.0;
    if (delta.isNull() && !event->pixelDelta().isNull())
        delta = QPointF(event->pixelDelta()) / 40.0;
    rmlContext->ProcessMouseWheel(::Rml::Vector2f(float(-delta.x()), float(-delta.y())), modifiers());
    requestRender(300);
}

void View::leaveEvent(QEvent *event)
{
    if (rmlContext)
        rmlContext->ProcessMouseLeave();
    requestRender(250);
    QOpenGLWidget::leaveEvent(event);
}

void View::keyPressEvent(QKeyEvent *event)
{
    if (!rmlContext)
        return;
    const bool propagate = rmlContext->ProcessKeyDown(mapKey(event->key()), modifiers());
    // printable text; control characters are handled as keys
    const QString text = event->text();
    if (!text.isEmpty() && !(event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier))) {
        bool printable = true;
        for (QChar c : text)
            printable &= c.isPrint() || c.isSurrogate();
        if (printable)
            rmlContext->ProcessTextInput(text.toStdString());
    }
    requestRender(200);
    if (propagate)
        event->ignore();
}

void View::keyReleaseEvent(QKeyEvent *event)
{
    if (!rmlContext)
        return;
    rmlContext->ProcessKeyUp(mapKey(event->key()), modifiers());
    requestRender();
}

void View::inputMethodEvent(QInputMethodEvent *event)
{
    if (rmlContext && !event->commitString().isEmpty())
        rmlContext->ProcessTextInput(event->commitString().toStdString());
    requestRender();
    event->accept();
}

QVariant View::inputMethodQuery(Qt::InputMethodQuery query) const
{
    if (query == Qt::ImEnabled)
        return true;
    return QOpenGLWidget::inputMethodQuery(query);
}

void View::focusOutEvent(QFocusEvent *event)
{
    QOpenGLWidget::focusOutEvent(event);
    requestRender();
}

bool View::focusNextPrevChild(bool)
{
    // Tab belongs to the document (focus moves between RmlUi elements)
    return false;
}

bool View::event(QEvent *event)
{
    // Tab and Shift+Tab reach keyPressEvent instead of moving Qt widget focus
    if (event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
            keyPressEvent(key);
            return true;
        }
    }
    return QOpenGLWidget::event(event);
}

} // namespace Rml
} // namespace UI
} // namespace Acheron
