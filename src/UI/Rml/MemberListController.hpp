#pragma once

#include "Controller.hpp"
#include "Core/Snowflake.hpp"
#include "Platform/Timer.hpp"
#include "Platform/Trackable.hpp"

#include <vector>

namespace Styx::Core {
class ClientInstance;
class MemberListManager;
class PresenceManager;
}

namespace Styx::UI::Rml {

class MemberListController final : public Controller
{
public:
    MemberListController(Shell &shell, ::Rml::Element *root);
    ~MemberListController() override;

private:
    void setupConnections();
    void onLocationChanged();
    void onInstanceChanged();
    void onScroll();
    void scheduleUpdate();
    void updateMemberList();
    void renderGuildMemberList();
    void renderGroupDmList(const std::vector<Core::Snowflake> &recipients);
    void renderSingleDmProfile(Core::Snowflake targetUserId);
    void connectCurrentManager();
    void disconnectCurrentManager();

    Platform::Timer updateDebounceTimer;
    Platform::Guard<Core::ClientInstance> currentInstance;
    Platform::Guard<Core::MemberListManager> currentMemberListManager;
    Platform::Guard<Core::PresenceManager> currentPresenceManager;
    std::vector<Platform::Connection> managerConnections;
    Core::Snowflake activeGuildId;
    Core::Snowflake activeChannelId;
    int lastSubStart = -1;
    int lastSubEnd = -1;
    static constexpr int RowHeight = 42;
};

} // namespace Styx::UI::Rml
