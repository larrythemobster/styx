#pragma once

#include "Controller.hpp"
#include "Platform/Timer.hpp"
#include "Platform/Trackable.hpp"

#include <string>
#include <vector>

namespace Styx {
namespace Core { class ClientInstance; }
namespace UI {
struct ChannelNode;
namespace Rml {

class ChannelListController final : public Controller
{
public:
    ChannelListController(Shell &shell, ::Rml::Element *root);

private:
    void scheduleRebuild();
    void renderSidebar();
    void renderGuildSidebar();
    void renderHomeSidebar();
    void renderChannelNode(ChannelNode *node, std::string &rml);
    void connectInstanceVoice(Core::ClientInstance *instance);

    ::Rml::Element *sidebarHeader = nullptr;
    ::Rml::Element *channelList = nullptr;
    Platform::Timer rebuildTimer;
    Platform::Guard<Core::ClientInstance> lastInstance;
    std::vector<Platform::Connection> instanceConnections;
    std::string dmFilterText;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
