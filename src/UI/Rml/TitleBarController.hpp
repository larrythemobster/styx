#pragma once
#include "Controller.hpp"
#include "Platform/Timer.hpp"
#include <RmlUi/Core/ObserverPtr.h>

namespace Rml { class ElementDocument; }

namespace Styx::UI::Rml {
class InboxController;
class UpdatesController;
class TitleBarController final : public Controller {
public:
    TitleBarController(Shell &shell, ::Rml::Element *root, InboxController &inbox, UpdatesController &updates);
    ~TitleBarController() override;
private:
    void refresh();
    // The hovered (or keyboard-focused) control whose title, or icon-only button label, is shown as a hint.
    ::Rml::Element *hintButton() const;
    ::Rml::Element *hintElementFor(::Rml::ElementDocument *document) const;
    void updateHint();
    void showHint();
    void hideHint();
    void positionHint();
    InboxController &inbox;
    UpdatesController &updates;
    // Popover documents own their hint element and may close while it is shown.
    ::Rml::ObserverPtr<::Rml::Element> hint;
    // Controls can be rebuilt while their hint is shown, so the owner is observed rather than held.
    ::Rml::ObserverPtr<::Rml::Element> hintOwner;
    Platform::Timer hintDelay;
    bool keyboardHints = false, hintsDismissed = false;
    float hintWidth = 0;
    float hintScale = 0;
    float hintLeft = -1, hintTop = -1;
};
}
