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

class UserPanelController final : public Controller
{
    Q_OBJECT
public:
    UserPanelController(Shell &shell, ::Rml::Element *root, QObject *parent = nullptr);

private slots:
    void rebuild();

private:
    void renderVoicePanel();
    void renderUserPanel();
    void connectInstanceVoice(Core::ClientInstance *instance);
#ifdef STYX_HAVE_SCREENSHARE
    void onShareClicked();
#endif

    ::Rml::Element *voicePanel = nullptr;
    ::Rml::Element *userPanel = nullptr;
    QTimer rebuildTimer;
    QPointer<Core::ClientInstance> lastInstance;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
