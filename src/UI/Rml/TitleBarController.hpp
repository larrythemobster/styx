#pragma once
#include "Controller.hpp"

namespace Styx::UI::Rml {
class InboxController;
class UpdatesController;
class TitleBarController final : public Controller {
public:
    TitleBarController(Shell &shell, ::Rml::Element *root, InboxController &inbox, UpdatesController &updates);
    ~TitleBarController() override;
private:
    void refresh();
    InboxController &inbox;
    UpdatesController &updates;
};
}
