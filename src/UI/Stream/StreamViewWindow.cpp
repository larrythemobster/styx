#include "StreamViewWindow.hpp"

#include <QCloseEvent>
#include <QKeyEvent>
#include <QPainter>

namespace Styx {
namespace UI {

StreamViewWindow::StreamViewWindow(Core::Snowflake streamerId, const QString &streamerName, QWidget *parent)
    : QWidget(parent, Qt::Window), streamerId(streamerId)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setWindowTitle(tr("%1 — Live").arg(streamerName));
    resize(1280, 760);
    setMinimumSize(320, 180);
    status = tr("Connecting to %1's stream…").arg(streamerName);
    fpsClock.start();
}

void StreamViewWindow::setFrame(const QImage &image, const QSize &size)
{
    frame = image;
    sourceSize = size;
    status.clear();
    ++framesThisSecond;
    if (fpsClock.elapsed() >= 1000) {
        fps = framesThisSecond;
        framesThisSecond = 0;
        fpsClock.restart();
    }
    update();
}

void StreamViewWindow::setStatus(const QString &text)
{
    status = text;
    update();
}

void StreamViewWindow::streamEnded(const QString &reason)
{
    ended = true;
    status = reason.isEmpty() ? tr("The stream ended.") : reason;
    update();
}

void StreamViewWindow::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    if (!frame.isNull()) {
        const QSize fitted = sourceSize.isEmpty() ? frame.size().scaled(size(), Qt::KeepAspectRatio)
                                                  : sourceSize.scaled(size(), Qt::KeepAspectRatio);
        const QRect target(QPoint((width() - fitted.width()) / 2, (height() - fitted.height()) / 2), fitted);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(target, frame);
    }

    if (!status.isEmpty() || ended) {
        painter.setPen(Qt::white);
        QFont font = painter.font();
        font.setPointSizeF(font.pointSizeF() * 1.3);
        painter.setFont(font);
        QRect box = painter.fontMetrics().boundingRect(rect().adjusted(40, 0, -40, 0), Qt::AlignCenter | Qt::TextWordWrap, status);
        box.adjust(-16, -10, 16, 10);
        painter.fillRect(box, QColor(0, 0, 0, 170));
        painter.drawText(box, Qt::AlignCenter | Qt::TextWordWrap, status);
    } else if (!sourceSize.isEmpty()) {
        const QString info = QStringLiteral("%1×%2 · %3 fps").arg(sourceSize.width()).arg(sourceSize.height()).arg(fps);
        painter.setPen(QColor(255, 255, 255, 160));
        painter.drawText(rect().adjusted(8, 6, -8, -6), Qt::AlignTop | Qt::AlignRight, info);
    }
}

void StreamViewWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    emit viewSizeChanged(streamerId, size() * devicePixelRatioF());
}

void StreamViewWindow::closeEvent(QCloseEvent *event)
{
    emit closed(streamerId);
    QWidget::closeEvent(event);
}

void StreamViewWindow::mouseDoubleClickEvent(QMouseEvent *)
{
    toggleFullscreen();
}

void StreamViewWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_F11 || (event->key() == Qt::Key_Escape && isFullScreen())) {
        toggleFullscreen();
        return;
    }
    QWidget::keyPressEvent(event);
}

void StreamViewWindow::toggleFullscreen()
{
    if (isFullScreen())
        showNormal();
    else
        showFullScreen();
}

} // namespace UI
} // namespace Styx
