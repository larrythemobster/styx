#include "Controller.hpp"

#include "Shell.hpp"
#include "RmlView.hpp"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>

namespace Acheron {
namespace UI {
namespace Rml {

Controller::Controller(Shell &shell, ::Rml::Element *root, QObject *parent)
    : QObject(parent), shell(shell), regionRoot(root)
{
}

Controller::~Controller()
{
    for (const Registration &r : registrations) {
        r.element->RemoveEventListener(r.type.toStdString(), r.listener, r.capture);
        delete r.listener;
    }
}

void Controller::requestRender(int animateMs)
{
    if (auto *view = shell.view())
        view->requestRender(animateMs);
}

void Controller::listen(const QString &eventType, std::function<void(::Rml::Event &)> handler,
                        ::Rml::Element *element, bool capture)
{
    if (!element)
        element = regionRoot;
    if (!element)
        return;
    auto *listener = new Listener;
    listener->handler = std::move(handler);
    element->AddEventListener(eventType.toStdString(), listener, capture);
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

QString Controller::attribute(::Rml::Element *element, const char *name)
{
    if (!element)
        return {};
    return QString::fromStdString(element->GetAttribute<::Rml::String>(name, ""));
}

} // namespace Rml
} // namespace UI
} // namespace Acheron
