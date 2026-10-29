#pragma once

#include "Controller.hpp"
#include "Core/Snowflake.hpp"

#include "Platform/Timer.hpp"
#include "Platform/Trackable.hpp"
#include <vector>

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
public:
    CallController(Shell &shell, ::Rml::Element *root);
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

    void scheduleUpdate();
    Platform::Timer updateDebounceTimer;
    Platform::Guard<Core::ClientInstance> currentInstance;
    Platform::Guard<Core::Audio::VoiceManager> currentVoiceManager;
    std::vector<Platform::Connection> voiceConnections;
#ifdef STYX_HAVE_SCREENSHARE
    Platform::Guard<Core::Stream::StreamManager> currentStreamManager;
#endif

    Core::Snowflake focusedUserId;
    bool isFullscreen = false;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
