#pragma once

#include "Controller.hpp"
#include "Core/Snowflake.hpp"

#include "Platform/Timer.hpp"
#include "Platform/Trackable.hpp"
#include <vector>
#include <memory>

namespace Styx {
namespace Core {
class ClientInstance;
}
namespace UI {
namespace Rml {
class CustomStatusController;
class ContextMenu;


class UserPanelController final : public Controller
{
public:
    UserPanelController(Shell &shell, ::Rml::Element *root);
    ~UserPanelController() override;

private:
    void rebuild();

    void renderVoicePanel();
    void renderUserPanel();
    void connectInstanceVoice(Core::ClientInstance *instance);
    void showConnectionInfo(float x, float y);
#ifdef STYX_HAVE_SCREENSHARE
    void onShareClicked();
#endif

    ::Rml::Element *voicePanel = nullptr;
    ::Rml::Element *userPanel = nullptr;
    void scheduleRebuild();
    Platform::Timer rebuildTimer;
    Platform::Guard<Core::ClientInstance> lastInstance;
    std::vector<Platform::Connection> instanceConnections;
    std::unique_ptr<CustomStatusController> customStatusController;
    std::unique_ptr<ContextMenu> connectionMenu;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
