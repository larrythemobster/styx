#pragma once

#include "Controller.hpp"
#include "Shortcuts.hpp"

#include <functional>
#include <optional>
#include <unordered_map>

namespace Rml {
class Event;
}

namespace Styx {
namespace UI {
namespace Rml {

// Runs the handler registered for a shortcut when its keys are pressed anywhere in the main
// window. It listens in the capture phase so a focused text box can't swallow the keys first.
class ShortcutController final : public Controller
{
public:
    explicit ShortcutController(Shell &shell);

    // One handler per action; setting it again replaces the old one. Actions without a handler
    // let the key press through.
    void setHandler(ShortcutAction action, std::function<void()> handler);
    [[nodiscard]] bool hasHandler(ShortcutAction action) const;

    // The action a keydown event asks for, for popovers in their own documents that the main
    // listener doesn't see.
    [[nodiscard]] static std::optional<ShortcutAction> actionFor(::Rml::Event &event);

private:
    std::unordered_map<ShortcutAction, std::function<void()>> handlers;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
