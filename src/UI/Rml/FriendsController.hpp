#pragma once

#include "Controller.hpp"
#include "Core/Snowflake.hpp"

#include <QPointer>
#include <QString>
#include <QTimer>

namespace Styx {
namespace Core {
class ClientInstance;
}
namespace UI {
namespace Rml {

class FriendsController final : public Controller
{
    Q_OBJECT
public:
    enum class Tab {
        Online,
        All,
        AddFriend,
    };

    FriendsController(Shell &shell, ::Rml::Element *root, QObject *parent = nullptr);
    ~FriendsController() override;

private slots:
    void rebuild();

private:
    void render();
    void renderHeader(QString &rml, int onlineCount, int allCount);
    void renderFriendsList(QString &rml, int onlineCount, int allCount);
    void renderActiveNow(QString &rml);
    void renderAddFriend(QString &rml);

    void switchTab(Tab newTab);
    void setupEventListeners();
    void connectInstance(Core::ClientInstance *instance);
    void sendFriendRequest();
    void updateAddFriendControls();

    Tab currentTab = Tab::Online;
    QString searchText;
    QString addFriendInputText;
    QString addFriendFeedback;
    bool addFriendSuccess = false;
    bool addFriendPending = false;
    quint64 requestGeneration = 0;

    QTimer rebuildTimer;
    QPointer<Core::ClientInstance> lastInstance;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
