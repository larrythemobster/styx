#include "Controller.hpp"

#include "Shell.hpp"
#include "RmlView.hpp"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>

namespace Styx {
namespace UI {
namespace Rml {

Controller::Controller(Shell &shell, ::Rml::Element *root)
    : shell(shell), regionRoot(root)
{
}

Controller::~Controller()
{
    removeListeners();
}

void Controller::removeListeners()
{
    for (const Registration &r : registrations) {
        r.element->RemoveEventListener(r.type, r.listener, r.capture);
        delete r.listener;
    }
    registrations.clear();
}

void Controller::requestRender(int animateMs)
{
    if (auto *view = shell.view())
        view->requestRender(animateMs);
}

void Controller::listen(const std::string &eventType, std::function<void(::Rml::Event &)> handler,
                        ::Rml::Element *element, bool capture)
{
    if (!element)
        element = regionRoot;
    if (!element)
        return;
    auto *listener = new Listener;
    listener->handler = std::move(handler);
    element->AddEventListener(eventType, listener, capture);
    registrations.push_back({ element, eventType, listener, capture });
}

::Rml::Element *Controller::closest(::Rml::Element *element, const char *attributeName)
{
    for (; element; element = element->GetParentNode()) {
        if (element->HasAttribute(attributeName))
            return element;
    }
    return nullptr;
}

std::string Controller::attribute(::Rml::Element *element, const char *name)
{
    if (!element)
        return {};
    return element->GetAttribute<::Rml::String>(name, "");
}

} // namespace Rml
} // namespace UI
} // namespace Styx
