#include "VoiceStatusBar.hpp"
#include "VoiceWindow.hpp"

#include "Core/Audio/VoiceManager.hpp"
#ifdef ACHERON_HAVE_SCREENSHARE
#include "Core/Stream/StreamManager.hpp"
#include "UI/Stream/SourcePickerDialog.hpp"
#include "UI/Stream/StreamViewWindow.hpp"

#include <QMessageBox>
#endif

#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>

namespace Acheron {
namespace UI {

VoiceStatusBar::VoiceStatusBar(QWidget *parent) : QWidget(parent)
{
    setupUi();
    hide();

    closeTimer.setSingleShot(true);
    closeTimer.setInterval(500);
    connect(&closeTimer, &QTimer::timeout, this, [this]() {
        if (voiceWindow)
            voiceWindow->close();
    });
}

VoiceStatusBar::~VoiceStatusBar()
{
    delete voiceWindow;
}

void VoiceStatusBar::setupUi()
{
    setFixedHeight(32);
    setCursor(Qt::PointingHandCursor);

    setStyleSheet("QWidget { background-color: #1a1a2e; }"
                  "QLabel { background: transparent; }");

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 0, 6, 0);
    layout->setSpacing(6);

    statusDot = new QLabel(this);
    statusDot->setFixedSize(8, 8);
    statusDot->setStyleSheet("QLabel { background-color: #999; border-radius: 4px; }");

    statusLabel = new QLabel(this);
    statusLabel->setStyleSheet("QLabel { color: #999; font-size: 11px; font-weight: bold; }");

    channelLabel = new QLabel(this);
    channelLabel->setStyleSheet("QLabel { color: #72767d; font-size: 11px; }");

    // idk why i cant just set some text so this will have to do ig
    auto makeXIcon = [](QColor color) {
        QPixmap px(16, 16);
        px.fill(Qt::transparent);
        QPainter p(&px);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(color, 2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(4, 4, 12, 12);
        p.drawLine(12, 4, 4, 12);
        return QIcon(px);
    };

    disconnectBtn = new QPushButton(this);
    disconnectBtn->setFixedSize(24, 24);
    disconnectBtn->setIcon(makeXIcon(QColor(0xf0, 0x47, 0x47)));
    disconnectBtn->setIconSize(QSize(16, 16));
    disconnectBtn->setStyleSheet(
            "QPushButton { background: transparent; border: none; }"
            "QPushButton:hover { background: rgba(240, 71, 71, 30); border-radius: 4px; }");
    disconnectBtn->setCursor(Qt::PointingHandCursor);
    connect(disconnectBtn, &QPushButton::clicked, this, &VoiceStatusBar::disconnectRequested);

    layout->addWidget(statusDot);
    layout->addWidget(statusLabel);
    layout->addWidget(channelLabel);
    layout->addStretch();
#ifdef ACHERON_HAVE_SCREENSHARE
    shareBtn = new QPushButton(this);
    shareBtn->setFixedSize(24, 24);
    shareBtn->setIconSize(QSize(16, 16));
    shareBtn->setCursor(Qt::PointingHandCursor);
    shareBtn->setStyleSheet(
            "QPushButton { background: transparent; border: none; }"
            "QPushButton:hover { background: rgba(255, 255, 255, 20); border-radius: 4px; }");
    connect(shareBtn, &QPushButton::clicked, this, &VoiceStatusBar::onShareClicked);
    layout->addWidget(shareBtn);
    updateShareButton();
#endif
    layout->addWidget(disconnectBtn);
}

void VoiceStatusBar::setVoiceManager(Core::Audio::VoiceManager *manager)
{
    if (voiceManager == manager)
        return;

    disconnectManager();
    voiceManager = manager;

    if (!voiceManager) {
        if (voiceWindow)
            voiceWindow->setVoiceManager(nullptr);
        updateConnectionState();
        return;
    }

    connect(voiceManager, &Core::Audio::VoiceManager::voiceStateChanged,
            this, &VoiceStatusBar::updateConnectionState);

    configureVoiceWindow();
    updateConnectionState();
}

void VoiceStatusBar::setNameResolver(NameResolver resolver)
{
    nameResolver = std::move(resolver);
    if (voiceWindow)
        voiceWindow->setNameResolver(nameResolver);
}

void VoiceStatusBar::setAvatarResolver(AvatarResolver resolver)
{
    avatarResolver = std::move(resolver);
    if (voiceWindow)
        voiceWindow->setAvatarResolver(avatarResolver);
}

void VoiceStatusBar::setStatusResolver(StatusResolver resolver)
{
    statusResolver = std::move(resolver);
    if (voiceWindow)
        voiceWindow->setStatusResolver(statusResolver);
}

void VoiceStatusBar::refreshPresences(const QList<Core::Snowflake> &userIds)
{
    if (voiceWindow)
        voiceWindow->refreshPresences(userIds);
}

void VoiceStatusBar::setImageManager(Core::ImageManager *manager)
{
    imageManager = manager;
    if (voiceWindow)
        voiceWindow->setImageManager(manager);
}

void VoiceStatusBar::setAccount(Core::Snowflake id)
{
    accountId = id;
    if (voiceWindow)
        voiceWindow->setAccount(id);
}

void VoiceStatusBar::setChannelName(const QString &name)
{
    if (name.isEmpty())
        channelLabel->hide();
    else {
        channelLabel->setText(name);
        channelLabel->show();
    }
}

void VoiceStatusBar::updateConnectionState()
{
    using State = Discord::Voice::VoiceClient::State;

    State state = State::Disconnected;
    if (voiceManager)
        state = voiceManager->clientState();

    bool connected = (state == State::Connected);
    bool connecting = (state == State::Connecting || state == State::Identifying ||
                       state == State::WaitingForReady || state == State::DiscoveringIP ||
                       state == State::SelectingProtocol || state == State::WaitingForSession);
    bool active = connected || connecting;

    QString dotColor;
    QString textColor;
    QString stateText;

    if (connected) {
        stateText = tr("Voice Connected");
        dotColor = "#43b581";
        textColor = "#43b581";
    } else if (connecting) {
        if (state == State::Connecting)
            stateText = tr("Connecting...");
        else if (state == State::Identifying)
            stateText = tr("Identifying...");
        else
            stateText = tr("Securing...");
        dotColor = "#faa61a";
        textColor = "#faa61a";
    } else {
        stateText = tr("Disconnected");
        dotColor = "#999";
        textColor = "#999";
    }

    statusDot->setStyleSheet(QStringLiteral("QLabel { background-color: %1; border-radius: 4px; }").arg(dotColor));
    statusLabel->setText(stateText);
    statusLabel->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 11px; font-weight: bold; }").arg(textColor));

    setVisible(active);

    if (active && wasDisconnected) {
        closeTimer.stop();
        showVoiceWindow();
        if (voiceWindow)
            voiceWindow->refreshDevices();
    } else if (active) {
        closeTimer.stop();
    }

    if (!active && voiceWindow)
        closeTimer.start();

    wasDisconnected = !active;
}

void VoiceStatusBar::mousePressEvent(QMouseEvent *event)
{
    if (disconnectBtn->geometry().contains(event->pos()) || (shareBtn && shareBtn->geometry().contains(event->pos()))) {
        QWidget::mousePressEvent(event);
        return;
    }

    toggleVoiceWindow();
}

void VoiceStatusBar::toggleVoiceWindow()
{
    if (voiceWindow && voiceWindow->isVisible() && !voiceWindow->isMinimized())
        voiceWindow->close();
    else
        showVoiceWindow();
}

void VoiceStatusBar::showVoiceWindow()
{
    if (!voiceWindow) {
        voiceWindow = new VoiceWindow();
        connect(voiceWindow, &VoiceWindow::userProfileRequested, this, &VoiceStatusBar::userProfileRequested);
        configureVoiceWindow();
    }

    voiceWindow->setWindowState(voiceWindow->windowState() & ~Qt::WindowMinimized);
    voiceWindow->show();
    voiceWindow->raise();
    voiceWindow->activateWindow();
}

void VoiceStatusBar::configureVoiceWindow()
{
    if (!voiceWindow)
        return;
    if (imageManager)
        voiceWindow->setImageManager(imageManager);
    voiceWindow->setAccount(accountId);
    if (nameResolver)
        voiceWindow->setNameResolver(nameResolver);
    if (avatarResolver)
        voiceWindow->setAvatarResolver(avatarResolver);
    if (statusResolver)
        voiceWindow->setStatusResolver(statusResolver);
    if (voiceManager)
        voiceWindow->setVoiceManager(voiceManager);
#ifdef ACHERON_HAVE_SCREENSHARE
    voiceWindow->setStreamManager(streamManager);
#endif
}

void VoiceStatusBar::disconnectManager()
{
    if (voiceManager)
        disconnect(voiceManager, nullptr, this, nullptr);
    voiceManager = nullptr;
}

#ifdef ACHERON_HAVE_SCREENSHARE
void VoiceStatusBar::setStreamManager(Core::Stream::StreamManager *manager)
{
    if (streamManager == manager)
        return;
    if (streamManager)
        disconnect(streamManager, nullptr, this, nullptr);
    streamManager = manager;
    streamStatus.clear();

    if (streamManager) {
        using SM = Core::Stream::StreamManager;
        connect(streamManager, &SM::streamingChanged, this, &VoiceStatusBar::updateShareButton);
        connect(streamManager, &SM::streamingStatus, this, [this](const QString &status) {
            streamStatus = status;
            updateShareButton();
        });
        connect(streamManager, &SM::streamingFailed, this, [this](const QString &reason) {
            streamStatus.clear();
            updateShareButton();
            QMessageBox::warning(window(), tr("Go Live"), reason);
        });
        connect(streamManager, &SM::watchStarted, this, &VoiceStatusBar::openViewer);
        connect(streamManager, &SM::watchConnected, this, [this](Core::Snowflake userId) {
            if (auto viewer = viewers.value(userId))
                viewer->setStatus(tr("Waiting for the first keyframe…"));
        });
        connect(streamManager, &SM::viewerFrame, this, [this](Core::Snowflake userId, const QImage &image, const QSize &sourceSize) {
            if (auto viewer = viewers.value(userId))
                viewer->setFrame(image, sourceSize);
        });
        connect(streamManager, &SM::watchEnded, this, [this](Core::Snowflake userId, const QString &reason) {
            if (auto viewer = viewers.value(userId))
                viewer->streamEnded(reason);
            else if (!reason.isEmpty())
                QMessageBox::information(window(), tr("Go Live"), reason);
        });
    }

    if (voiceWindow)
        voiceWindow->setStreamManager(streamManager);
    updateShareButton();
}

void VoiceStatusBar::updateShareButton()
{
    if (!shareBtn)
        return;
    const bool live = streamManager && streamManager->isStreaming();

    // a monitor glyph; red while live
    const QColor color = live ? QColor(0xf0, 0x47, 0x47) : QColor(0xb9, 0xbb, 0xbe);
    QPixmap px(16, 16);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, 1.6));
    p.drawRoundedRect(QRectF(1.5, 2.5, 13, 9), 1.5, 1.5);
    p.drawLine(QPointF(8, 11.5), QPointF(8, 13.5));
    p.drawLine(QPointF(5, 14), QPointF(11, 14));
    if (live) {
        p.setBrush(color);
        p.drawEllipse(QPointF(8, 7), 2, 2);
    }
    p.end();
    shareBtn->setIcon(QIcon(px));

    QString tip = live ? tr("Stop streaming") : tr("Share your screen");
    if (live && !streamStatus.isEmpty())
        tip += QStringLiteral("\n") + streamStatus;
    shareBtn->setToolTip(tip);
}

void VoiceStatusBar::onShareClicked()
{
    if (!streamManager)
        return;
    if (streamManager->isStreaming()) {
        streamManager->stopStreaming();
        return;
    }

    QString reason;
    if (!streamManager->canStream(&reason)) {
        QMessageBox::information(window(), tr("Go Live"), reason);
        return;
    }

    SourcePickerDialog picker(window());
    if (picker.exec() != QDialog::Accepted)
        return;
    // the voice call may have ended while the picker was open
    if (streamManager && streamManager->canStream(&reason))
        streamManager->startStreaming(picker.selectedSource(), picker.selectedQuality(), picker.includeAudio());
}

void VoiceStatusBar::openViewer(Core::Snowflake userId)
{
    if (auto existing = viewers.value(userId)) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }

    const QString name = nameResolver ? nameResolver(userId) : QString::number(userId);
    auto *viewer = new StreamViewWindow(userId, name);
    viewers.insert(userId, viewer);
    connect(viewer, &StreamViewWindow::closed, this, [this](Core::Snowflake streamer) {
        viewers.remove(streamer);
        if (streamManager && streamManager->isWatching(streamer))
            streamManager->stopWatching(streamer);
    });
    connect(viewer, &StreamViewWindow::viewSizeChanged, this, [this](Core::Snowflake streamer, const QSize &size) {
        if (streamManager)
            streamManager->setViewerSize(streamer, size);
    });
    viewer->show();
}
#endif

} // namespace UI
} // namespace Acheron
