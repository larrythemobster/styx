#pragma once

#include "Controller.hpp"
#include "Core/Snowflake.hpp"

#include "Platform/Timer.hpp"
#include "Platform/Trackable.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace Styx {
namespace Core {
class ClientInstance;
}
namespace UI {
namespace Rml {

class FriendsController final : public Controller
{
public:
    enum class Tab {
        Online,
        All,
        AddFriend,
    };

    FriendsController(Shell &shell, ::Rml::Element *root);
    ~FriendsController() override;

private:
    void rebuild();

    void render();
    void renderHeader(std::string &rml, int onlineCount, int allCount);
    void renderFriendsList(std::string &rml, int onlineCount, int allCount);
    void renderActiveNow(std::string &rml);
    void renderAddFriend(std::string &rml);

    void switchTab(Tab newTab);
    void setupEventListeners();
    void connectInstance(Core::ClientInstance *instance);
    void sendFriendRequest();
    void updateAddFriendControls();

    Tab currentTab = Tab::Online;
    std::string searchText;
    std::string addFriendInputText;
    std::string addFriendFeedback;
    bool addFriendSuccess = false;
    bool addFriendPending = false;
    std::uint64_t requestGeneration = 0;

    void scheduleRebuild();
    Platform::Timer rebuildTimer;
    Platform::Guard<Core::ClientInstance> lastInstance;
    std::vector<Platform::Connection> instanceConnections;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
