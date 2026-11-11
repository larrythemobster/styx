#pragma once
#include "Controller.hpp"
#include "Platform/Timer.hpp"

namespace Styx::UI::Rml {
class InboxController;
class UpdatesController;
class TitleBarController final : public Controller {
public:
    TitleBarController(Shell &shell, ::Rml::Element *root, InboxController &inbox, UpdatesController &updates);
    ~TitleBarController() override;
private:
    void refresh();
    ::Rml::Element *hintButton() const;
    void updateHint();
    void showHint();
    void hideHint();
    void positionHint();
    InboxController &inbox;
    UpdatesController &updates;
    ::Rml::Element *hint = nullptr;
    ::Rml::Element *hintOwner = nullptr;
    Platform::Timer hintDelay;
    bool keyboardHints = false, hintsDismissed = false;
    float hintWidth = 0;
    float hintScale = 0;
    float hintLeft = -1, hintTop = -1;
};
}
