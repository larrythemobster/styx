#include "ShortcutController.hpp"

#include "Shell.hpp"

#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

#include <utility>

namespace Styx {
namespace UI {
namespace Rml {

ShortcutController::ShortcutController(Shell &shell)
    : Controller(shell, nullptr)
{
    if (auto *doc = shell.document()) {
        listen("keydown", [this](::Rml::Event &event) {
            const auto action = actionFor(event);
            if (action) {
                const auto it = handlers.find(*action);
                if (it != handlers.end() && it->second())
                    event.StopPropagation();
            }
        }, doc, true);
    }
}

void ShortcutController::setHandler(ShortcutAction action, std::function<bool()> handler)
{
    handlers.insert_or_assign(action, std::move(handler));
}

bool ShortcutController::hasHandler(ShortcutAction action) const
{
    return handlers.contains(action);
}

std::optional<ShortcutAction> ShortcutController::actionFor(::Rml::Event &event)
{
    int modifiers = 0;
    if (event.GetParameter<int>("ctrl_key", 0) != 0)
        modifiers |= ::Rml::Input::KM_CTRL;
    if (event.GetParameter<int>("shift_key", 0) != 0)
        modifiers |= ::Rml::Input::KM_SHIFT;
    if (event.GetParameter<int>("alt_key", 0) != 0)
        modifiers |= ::Rml::Input::KM_ALT;
    if (event.GetParameter<int>("meta_key", 0) != 0)
        modifiers |= ::Rml::Input::KM_META;

    const int key = event.GetParameter<int>("key_identifier", 0);
    return matchShortcut(key, modifiers);
}

} // namespace Rml
} // namespace UI
} // namespace Styx
