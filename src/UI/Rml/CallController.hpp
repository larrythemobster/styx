#pragma once

#include "Controller.hpp"
#include "Core/Snowflake.hpp"

#include <QPointer>
#include <QSet>
#include <QTimer>

namespace Styx {
namespace Core {
class ClientInstance;
namespace Audio {
class VoiceManager;
}
namespace Stream {
class StreamManager;
}
} // namespace Core

namespace UI {
namespace Rml {

class CallController final : public Controller
{
    Q_OBJECT
public:
    CallController(Shell &shell, ::Rml::Element *root, QObject *parent = nullptr);
    ~CallController() override;

private:
    void setupConnections();
    void onLocationChanged();
    void onInstanceChanged();
    void onCallOpenChanged(bool open);

    void connectCurrentVoice();
    void disconnectCurrentVoice();

    void updateCallView();

    // Stream & Action Handlers
    void onMuteClicked();
    void onDeafClicked();
    void onShareClicked();
    void onDisconnectClicked();
    void onWatchClicked(Core::Snowflake userId);
    void onStopWatchClicked(Core::Snowflake userId);
    void onTileClicked(Core::Snowflake userId);
    void onCloseClicked();
    void onFullscreenClicked();

    QTimer updateDebounceTimer;
    QPointer<Core::ClientInstance> currentInstance;
    QPointer<Core::Audio::VoiceManager> currentVoiceManager;
#ifdef STYX_HAVE_SCREENSHARE
    QPointer<Core::Stream::StreamManager> currentStreamManager;
#endif

    Core::Snowflake focusedUserId;
    bool isFullscreen = false;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
