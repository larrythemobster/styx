#pragma once

#include "Platform/Native.hpp"
#include "Platform/Trackable.hpp"
#include <string>
#include <cstdint>
#include <RmlUi/Core/EventListener.h>

#include <functional>
#include <vector>
#include <optional>

namespace Rml {
class ElementDocument;
class Event;
} // namespace Rml

namespace Styx::UI::Rml {

class View;

struct MenuItem
{
    using ToggleComplete = std::function<void(bool accepted)>;
    using ToggleAction = std::function<void(bool checked, ToggleComplete complete)>;
    std::string label;
    std::function<void()> action;
    bool enabled = true;
    bool danger = false;
    bool separator = false;
    std::vector<MenuItem> children;
    std::string subtitle;
    std::string imageSource;
    std::string color;
    std::optional<bool> checked;
    bool radio = false;
    std::string iconText;
    ToggleAction toggle;
    bool pending = false;
};

class ContextMenu final : public Platform::Trackable, public ::Rml::EventListener
{
public:
    explicit ContextMenu(View &view);
    ~ContextMenu() override;

    void show(std::vector<MenuItem> items, Platform::Point position);
    void hide();
    [[nodiscard]] bool isVisible() const;
    void ProcessEvent(::Rml::Event &event) override;

private:
    void renderPanel(std::size_t depth);
    void positionPanels();
    void refreshStates();
    void activate(std::size_t depth, int index);
    void closeSubmenus(std::size_t depth);
    void openSubmenu(std::size_t depth, int index, bool enter);
    void focusItem(int index, bool scroll = true);
    void moveFocus(int direction);
    [[nodiscard]] std::vector<MenuItem> &entriesAt(std::size_t depth);
    [[nodiscard]] bool selectable(std::size_t depth, int index);
    [[nodiscard]] static std::string itemId(std::size_t depth, int index);

    View &view;
    ::Rml::ElementDocument *document = nullptr;
    std::vector<MenuItem> items;
    std::vector<int> path;
    int focused = -1;
    std::size_t activeDepth = 0;
    Platform::Point anchor;
    std::uint64_t generation = 0;
};

} // namespace Styx::UI::Rml
