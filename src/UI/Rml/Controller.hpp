#pragma once

#include <QObject>
#include <QString>

#include <RmlUi/Core/EventListener.h>

#include <functional>
#include <vector>

namespace Rml {
class Element;
class Event;
} // namespace Rml

namespace Styx {
namespace UI {

class Shell;

namespace Rml {

// Base for the objects that own one region of main.rml (rail, channel list, chat, ...).
// A controller builds its region's DOM, keeps it in sync with Core signals, and turns
// RmlUi events into Shell navigation calls. Controllers live on the GUI thread.
class Controller : public QObject
{
    Q_OBJECT
public:
    Controller(Shell &shell, ::Rml::Element *root, QObject *parent = nullptr);
    ~Controller() override;

    [[nodiscard]] ::Rml::Element *root() const { return regionRoot; }

protected:
    // Repaint the view after a DOM change (animateMs keeps drawing for CSS transitions).
    void requestRender(int animateMs = 0);

    // Listen for an RmlUi event type ("click", "mouseover", "keydown", "scroll", ...) on
    // `element` (default: the region root). Handlers see the event's target and can walk up
    // with closest() to find the item that was hit. Removed when the controller dies.
    void listen(const QString &eventType, std::function<void(::Rml::Event &)> handler,
                ::Rml::Element *element = nullptr, bool capture = false);

    // Nearest ancestor-or-self of `element` that has `attribute` (e.g. "data-id").
    static ::Rml::Element *closest(::Rml::Element *element, const char *attribute);
    static QString attribute(::Rml::Element *element, const char *name);

    Shell &shell;
    ::Rml::Element *regionRoot;

private:
    struct Listener final : ::Rml::EventListener
    {
        std::function<void(::Rml::Event &)> handler;
        void ProcessEvent(::Rml::Event &event) override { handler(event); }
    };
    struct Registration
    {
        ::Rml::Element *element;
        QString type;
        Listener *listener;
        bool capture;
    };
    std::vector<Registration> registrations;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
