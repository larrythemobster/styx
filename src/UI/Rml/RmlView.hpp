#pragma once

#include <QElapsedTimer>
#include <QOpenGLWidget>
#include <QString>
#include <QTimer>

namespace Rml {
class Context;
class ElementDocument;
} // namespace Rml

namespace Acheron {
namespace UI {
namespace Rml {

// A QOpenGLWidget that hosts one RmlUi context: renders it with the GL3 backend and feeds
// it Qt's mouse, keyboard, wheel and input-method events. Repaints on demand (input, data
// changes, texture arrivals) plus a short burst afterwards so CSS transitions finish; an
// idle window draws nothing.
class View : public QOpenGLWidget
{
    Q_OBJECT
public:
    explicit View(const QString &contextName, QWidget *parent = nullptr);
    ~View() override;

    [[nodiscard]] ::Rml::Context *context() const { return rmlContext; }

    // Loads and shows a document from :/ui once the GL context exists (emits documentLoaded).
    void loadDocument(const QString &path);
    [[nodiscard]] ::Rml::ElementDocument *document() const { return rmlDocument; }

    // Ask for a frame; keeps drawing for `animateMs` so transitions/animations complete.
    void requestRender(int animateMs = 0);

signals:
    void documentLoaded(::Rml::ElementDocument *document);
    void initialisationFailed(const QString &error);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void focusOutEvent(QFocusEvent *event) override;
    bool focusNextPrevChild(bool next) override;
    bool event(QEvent *event) override;

private:
    int modifiers() const;
    void applyCursor();

    QString name;
    QString pendingDocument;
    ::Rml::Context *rmlContext = nullptr;
    ::Rml::ElementDocument *rmlDocument = nullptr;
    QTimer animationTimer;
    QElapsedTimer animationClock;
    qint64 animateUntilMs = 0;
    QString lastCursor;
};

} // namespace Rml
} // namespace UI
} // namespace Acheron
