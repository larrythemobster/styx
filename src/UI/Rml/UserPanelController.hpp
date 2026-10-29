#pragma once

#include "Controller.hpp"
#include "Core/Snowflake.hpp"

#include "Platform/Timer.hpp"
#include "Platform/Trackable.hpp"
#include <vector>

namespace Styx {
namespace Core {
class ClientInstance;
}
namespace UI {
namespace Rml {

class UserPanelController final : public Controller
{
public:
    UserPanelController(Shell &shell, ::Rml::Element *root);

private:
    void rebuild();

    void renderVoicePanel();
    void renderUserPanel();
    void connectInstanceVoice(Core::ClientInstance *instance);
#ifdef STYX_HAVE_SCREENSHARE
    void onShareClicked();
#endif

    ::Rml::Element *voicePanel = nullptr;
    ::Rml::Element *userPanel = nullptr;
    void scheduleRebuild();
    Platform::Timer rebuildTimer;
    Platform::Guard<Core::ClientInstance> lastInstance;
    std::vector<Platform::Connection> instanceConnections;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
