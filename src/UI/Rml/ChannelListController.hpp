#pragma once

#include "Controller.hpp"
#include "Core/Snowflake.hpp"

#include <QModelIndex>
#include <QPointer>
#include <QString>
#include <QTimer>

namespace Styx {
namespace Core {
class ClientInstance;
}
namespace UI {
namespace Rml {

class ChannelListController final : public Controller
{
    Q_OBJECT
public:
    ChannelListController(Shell &shell, ::Rml::Element *root, QObject *parent = nullptr);

private slots:
    void rebuild();

private:
    void renderSidebar();
    void renderGuildSidebar();
    void renderHomeSidebar();
    void renderChannelNode(const QModelIndex &proxyIndex, QString &rml, int depth = 0);
    void connectInstanceVoice(Core::ClientInstance *instance);

    ::Rml::Element *sidebarHeader = nullptr;
    ::Rml::Element *channelList = nullptr;
    QTimer rebuildTimer;
    QPointer<Core::ClientInstance> lastInstance;
    QString dmFilterText;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
