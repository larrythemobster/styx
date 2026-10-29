#pragma once

#include <QObject>
#include <QString>
#include <QPoint>
#include <RmlUi/Core/EventListener.h>

#include <functional>
#include <vector>

namespace Rml {
class ElementDocument;
class Event;
} // namespace Rml

namespace Styx::UI::Rml {

class View;

struct MenuItem
{
    QString label;
    std::function<void()> action;
    bool enabled = true;
    bool danger = false;
    bool separator = false;
    std::vector<MenuItem> children;
    QString subtitle;
    QString imageSource;
    QString color;
};

class ContextMenu final : public QObject, public ::Rml::EventListener
{
public:
    explicit ContextMenu(View &view);
    ~ContextMenu() override;

    void show(std::vector<MenuItem> items, const QPoint &position);
    void hide();
    [[nodiscard]] bool isVisible() const;
    void ProcessEvent(::Rml::Event &event) override;

private:
    void render();
    void openSubmenu(int index);
    void focusItem(int index);
    void moveFocus(int direction);
    [[nodiscard]] const std::vector<MenuItem> &currentItems() const;
    [[nodiscard]] bool selectable(int index) const;

    View &view;
    ::Rml::ElementDocument *document = nullptr;
    std::vector<MenuItem> items;
    std::vector<int> path;
    int focused = -1;
    QPoint anchor;
};

} // namespace Styx::UI::Rml
