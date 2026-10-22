#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QString>
#include <QWidget>

#include "Core/Snowflake.hpp"

namespace Acheron {
namespace UI {

// A watched Go Live stream. Paints the newest decoded frame, letterboxed; double-click or
// F11 toggles fullscreen.
class StreamViewWindow : public QWidget
{
    Q_OBJECT
public:
    StreamViewWindow(Core::Snowflake streamerId, const QString &streamerName, QWidget *parent = nullptr);

    [[nodiscard]] Core::Snowflake streamer() const { return streamerId; }

    void setFrame(const QImage &image, const QSize &sourceSize);
    void setStatus(const QString &text);
    void streamEnded(const QString &reason);

signals:
    void viewSizeChanged(Core::Snowflake streamerId, const QSize &devicePixels);
    void closed(Core::Snowflake streamerId);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void toggleFullscreen();

    Core::Snowflake streamerId;
    QImage frame;
    QSize sourceSize;
    QString status;
    bool ended = false;
    QElapsedTimer fpsClock;
    int framesThisSecond = 0;
    int fps = 0;
};

} // namespace UI
} // namespace Acheron
