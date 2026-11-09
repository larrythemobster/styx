#pragma once

#include "Controller.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Enums.hpp"

#include "Platform/Timer.hpp"
#include "Platform/Trackable.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Styx {
namespace Core {
class ClientInstance;
}
namespace Discord {
struct Relationship;
}
namespace UI {
namespace Rml {

class FriendsController final : public Controller
{
public:
    enum class Tab {
        Online,
        All,
        Pending,
        AddFriend,
    };

    FriendsController(Shell &shell, ::Rml::Element *root);
    ~FriendsController() override;

private:
    struct FriendItem {
        Core::Snowflake userId;
        std::string displayName;
        std::string username;
        std::string avatarSrc;
        std::string statusClass;
        std::string subtext;
        bool isOnline = false;
        Discord::RelationshipType relationshipType = Discord::RelationshipType::NONE;
    };

    void rebuild();

    void render();
    void renderHeader(std::string &rml, int onlineCount, int allCount);
    void renderFriendsList(std::string &rml, int onlineCount, int allCount);
    void renderFriendRow(std::string &rml, const FriendItem &item, std::string_view actionsRml);
    FriendItem buildFriendItem(Core::ClientInstance *inst, Core::Snowflake userId, const Discord::Relationship &rel);
    void renderActiveNow(std::string &rml);
    void renderAddFriend(std::string &rml);

    void switchTab(Tab newTab);
    void setupEventListeners();
    void connectInstance(Core::ClientInstance *instance);
    void sendFriendRequest();
    void respondToRequest(Core::Snowflake userId, bool accept);
    void updateAddFriendControls();

    Tab currentTab = Tab::Online;
    std::string searchText;
    std::string pendingError;
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
