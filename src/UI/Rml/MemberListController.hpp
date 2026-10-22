#pragma once

#include "Controller.hpp"
#include "Core/Snowflake.hpp"

#include <QPointer>
#include <QSet>
#include <QTimer>

namespace Acheron {
namespace Core {
class ClientInstance;
class MemberListManager;
class PresenceManager;
} // namespace Core

namespace UI {
namespace Rml {

class MemberListController final : public Controller
{
    Q_OBJECT
public:
    MemberListController(Shell &shell, ::Rml::Element *root, QObject *parent = nullptr);
    ~MemberListController() override;

private:
    void setupConnections();
    void onLocationChanged();
    void onInstanceChanged();
    void onScroll();

    void updateMemberList();
    void renderGuildMemberList();
    void renderGroupDmList(const QList<Core::Snowflake> &recipients);
    void renderSingleDmProfile(Core::Snowflake targetUserId);

    void connectCurrentManager();
    void disconnectCurrentManager();

    QTimer updateDebounceTimer;
    QPointer<Core::ClientInstance> currentInstance;
    QPointer<Core::MemberListManager> currentMemberListManager;
    QPointer<Core::PresenceManager> currentPresenceManager;

    Core::Snowflake activeGuildId;
    Core::Snowflake activeChannelId;
    int lastSubStart = -1;
    int lastSubEnd = -1;

    static constexpr int RowHeight = 42;
    static constexpr int GroupHeaderHeight = 40;
};

} // namespace Rml
} // namespace UI
} // namespace Acheron
