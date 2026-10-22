#pragma once

#include <QHash>
#include <QWidget>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QUrl>

#include <functional>

#include "Core/Snowflake.hpp"
#include "Discord/Voice/VoiceClient.hpp"
#include "Core/Presence/PresenceBadge.hpp"

namespace Styx {
namespace Core {
class ImageManager;
namespace Audio {
class VoiceManager;
}
namespace Stream {
class StreamManager;
}
} // namespace Core
namespace UI {

class VoiceWindow;
class StreamViewWindow;

class VoiceStatusBar : public QWidget
{
    Q_OBJECT
public:
    using NameResolver = std::function<QString(Core::Snowflake)>;
    using AvatarResolver = std::function<QUrl(Core::Snowflake)>;
    using StatusResolver = std::function<Core::PresenceBadge(Core::Snowflake)>;

    explicit VoiceStatusBar(QWidget *parent = nullptr);
    ~VoiceStatusBar() override;

    void setVoiceManager(Core::Audio::VoiceManager *manager);
#ifdef STYX_HAVE_SCREENSHARE
    void setStreamManager(Core::Stream::StreamManager *manager);
#endif
    void setNameResolver(NameResolver resolver);
    void setAvatarResolver(AvatarResolver resolver);
    void setStatusResolver(StatusResolver resolver);
    void refreshPresences(const QList<Core::Snowflake> &userIds);
    void setImageManager(Core::ImageManager *manager);
    void setAccount(Core::Snowflake accountId);
    void setChannelName(const QString &name);
    void updateConnectionState();

signals:
    void disconnectRequested();
    void userProfileRequested(Core::Snowflake accountId, Core::Snowflake userId);

protected:
    void mousePressEvent(QMouseEvent *event) override;

private:
    void setupUi();
    void disconnectManager();
    void toggleVoiceWindow();
    void showVoiceWindow();
    void configureVoiceWindow();
#ifdef STYX_HAVE_SCREENSHARE
    void onShareClicked();
    void updateShareButton();
    void openViewer(Core::Snowflake userId);
#endif

    Core::Audio::VoiceManager *voiceManager = nullptr;
    Core::ImageManager *imageManager = nullptr;
    Core::Snowflake accountId;
    NameResolver nameResolver;
    AvatarResolver avatarResolver;
    StatusResolver statusResolver;

    QLabel *statusDot;
    QLabel *statusLabel;
    QLabel *channelLabel;
    QPushButton *disconnectBtn;
    QPushButton *shareBtn = nullptr;
#ifdef STYX_HAVE_SCREENSHARE
    QPointer<Core::Stream::StreamManager> streamManager;
    QHash<Core::Snowflake, QPointer<StreamViewWindow>> viewers;
    QString streamStatus;
#endif

    QPointer<VoiceWindow> voiceWindow;
    QTimer closeTimer;
    bool wasDisconnected = true;
};

} // namespace UI
} // namespace Styx
