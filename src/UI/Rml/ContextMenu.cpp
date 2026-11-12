#include "ContextMenu.hpp"
#include "RmlView.hpp"
#include "Platform/Native.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <algorithm>

namespace Styx::UI::Rml {

namespace {
std::string panelId(std::size_t depth)
{
    return depth == 0 ? "context-menu-panel" : "context-submenu-" + std::to_string(depth);
}
}

ContextMenu::ContextMenu(View &view) : view(view)
{
    track(view.documentLoaded.connect([this](auto *) { hide(); }));
}

ContextMenu::~ContextMenu()
{
    if (document) {
        for (const char *type : { "click", "mousedown", "keydown", "mouseover" })
            document->RemoveEventListener(type, this);
        document->RemoveEventListener("scroll", this, true);
        document->Close();
    }
}

bool ContextMenu::isVisible() const
{
    return document && document->IsVisible();
}

void ContextMenu::show(std::vector<MenuItem> entries, Platform::Point position)
{
    if (!view.context()) return;
    if (!document) {
        document = view.context()->LoadDocument(":/ui/context_menu.rml");
        if (!document) return;
        for (const char *type : { "click", "mousedown", "keydown", "mouseover" })
            document->AddEventListener(type, this);
        document->AddEventListener("scroll", this, true);
    }
    closeSubmenus(0);
    items = std::move(entries);
    activeDepth = 0;
    focused = -1;
    anchor = position;
    renderPanel(0);
    document->Show(::Rml::ModalFlag::Modal);
    document->PullToFront();
    positionPanels();
    document->Focus();
    view.requestRender();
}

void ContextMenu::hide()
{
    if (document) document->Hide();
    closeSubmenus(0);
    items.clear();
    activeDepth = 0;
    focused = -1;
    view.requestRender();
}

const std::vector<MenuItem> &ContextMenu::entriesAt(std::size_t depth) const
{
    auto *entries = &items;
    for (std::size_t level = 0; level < depth; ++level)
        entries = &(*entries)[path[level]].children;
    return *entries;
}

bool ContextMenu::selectable(std::size_t depth, int index) const
{
    const auto &entries = entriesAt(depth);
    return index >= 0 && index < int(entries.size()) && entries[index].enabled &&
           !entries[index].separator && (entries[index].action || !entries[index].children.empty());
}

std::string ContextMenu::itemId(std::size_t depth, int index)
{
    return "menu-item-" + (depth ? std::to_string(depth) + "-" : "") + std::to_string(index);
}

void ContextMenu::renderPanel(std::size_t depth)
{
    auto *panel = document->GetElementById(panelId(depth));
    if (!panel) {
        auto element = document->CreateElement("div");
        element->SetId(panelId(depth));
        element->SetClass("context-menu-panel", true);
        element->SetAttribute("role", "menu");
        element->SetAttribute("data-menu-depth", int(depth));
        element->SetProperty("z-index", std::to_string(depth));
        panel = document->AppendChild(std::move(element));
    }
    const auto &entries = entriesAt(depth);
    std::string markup;
    markup.reserve(entries.size() * 256);
    for (int index = 0; index < int(entries.size()); ++index) {
        const auto &item = entries[index];
        if (item.separator) {
            markup += "<div class='menu-separator' role='separator'></div>";
            continue;
        }
        const bool actionable = bool(item.action) || !item.children.empty();
        const auto tag = actionable ? "button" : "div";
        markup += std::string("<") + tag + " id='" + itemId(depth, index) + "' data-menu-index='" +
                  std::to_string(index) + "' data-menu-depth='" + std::to_string(depth) +
                  "' class='menu-item" + (item.danger ? " danger" : "") +
                  (actionable ? "" : " menu-label") + (item.enabled ? "" : " disabled") + "'";
        if (actionable) {
            markup += " role='" + std::string(item.checked ? (item.radio ? "menuitemradio" : "menuitemcheckbox") : "menuitem") + "'";
            if (item.checked) markup += std::string(" aria-checked='") + (*item.checked ? "true" : "false") + "'";
            if (!item.children.empty()) markup += " aria-haspopup='menu' aria-expanded='false'";
        }
        if (!item.enabled || !actionable) markup += " disabled='disabled'";
        markup += ">";
        if (!item.iconText.empty())
            markup += "<span class='menu-icon-text'><span>" + Platform::htmlEscape(item.iconText) + "</span></span>";
        if (!item.imageSource.empty())
            markup += "<img class='menu-avatar' src='" + Platform::htmlEscape(item.imageSource) + "'/>";
        markup += "<div class='menu-text'";
        if (item.enabled && !item.color.empty()) markup += " style='color:" + Platform::htmlEscape(item.color) + ";'";
        markup += ">" + Platform::htmlEscape(item.label);
        if (!item.subtitle.empty())
            markup += "<div class='menu-subtitle'>" + Platform::htmlEscape(item.subtitle) + "</div>";
        markup += "</div>";
        if (item.checked) {
            markup += std::string("<span class='menu-check") + (*item.checked ? " checked" : "") + (item.radio ? " radio" : "") + "'>";
            if (*item.checked) markup += item.radio ? "<span class='menu-radio-dot'></span>" : "<span class='menu-check-mark'>&#10003;</span>";
            markup += "</span>";
        }
        if (!item.children.empty()) markup += "<span class='menu-arrow'>&gt;</span>";
        markup += std::string("</") + tag + ">";
    }
    panel->SetInnerRML(markup);
}

void ContextMenu::positionPanels()
{
    if (!document || !view.context()) return;
    const auto dimensions = view.context()->GetDimensions();
    const float scale = float(view.devicePixelRatioF());
    const float margin = 4 * scale;
    view.context()->Update();
    for (std::size_t depth = 0; depth <= path.size(); ++depth) {
        auto *panel = document->GetElementById(panelId(depth));
        if (!panel) break;
        float x = anchor.x * scale;
        float y = anchor.y * scale;
        if (depth) {
            auto *parent = document->GetElementById(panelId(depth - 1));
            auto *row = document->GetElementById(itemId(depth - 1, path[depth - 1]));
            if (!parent || !row) break;
            const auto origin = parent->GetAbsoluteOffset(::Rml::BoxArea::Border);
            const float right = origin.x + parent->GetOffsetWidth() - 1;
            const float left = origin.x - panel->GetOffsetWidth() + 1;
            auto *ancestor = depth > 1 ? document->GetElementById(panelId(depth - 2)) : nullptr;
            const bool continueLeft = ancestor && origin.x < ancestor->GetAbsoluteOffset(::Rml::BoxArea::Border).x;
            x = continueLeft && left >= margin ? left : right;
            if (x + panel->GetOffsetWidth() > dimensions.x - margin) x = left;
            y = row->GetAbsoluteOffset(::Rml::BoxArea::Border).y - 6 * scale;
        }
        x = std::clamp(x, margin, std::max(margin, dimensions.x - panel->GetOffsetWidth() - margin));
        y = std::clamp(y, margin, std::max(margin, dimensions.y - panel->GetOffsetHeight() - margin));
        panel->SetProperty("left", std::to_string(x) + "px");
        panel->SetProperty("top", std::to_string(y) + "px");
        view.context()->Update();
    }
    view.requestRender();
}

void ContextMenu::closeSubmenus(std::size_t depth)
{
    while (path.size() > depth) {
        const auto level = path.size();
        if (document) {
            if (auto *parent = document->GetElementById(itemId(level - 1, path.back()))) {
                parent->SetClass("submenu-open", false);
                parent->SetAttribute("aria-expanded", "false");
            }
            if (auto *panel = document->GetElementById(panelId(level)))
                panel->GetParentNode()->RemoveChild(panel);
        }
        path.pop_back();
    }
    if (activeDepth > depth) {
        activeDepth = depth;
        focused = -1;
    }
}

void ContextMenu::focusItem(int index, bool scroll)
{
    if (!selectable(activeDepth, index)) return;
    focused = index;
    if (auto *element = document->GetElementById(itemId(activeDepth, index))) {
        element->Focus();
        if (scroll) element->ScrollIntoView();
    }
    view.requestRender();
}

void ContextMenu::moveFocus(int direction)
{
    const int count = int(entriesAt(activeDepth).size());
    const int start = focused < 0 && direction < 0 ? 0 : focused;
    for (int step = 1; step <= count; ++step) {
        const int index = (start + direction * step + count) % count;
        if (selectable(activeDepth, index)) {
            if (path.size() > activeDepth && path[activeDepth] != index) closeSubmenus(activeDepth);
            focusItem(index);
            return;
        }
    }
}

void ContextMenu::openSubmenu(std::size_t depth, int index, bool enter)
{
    if (!selectable(depth, index) || entriesAt(depth)[index].children.empty()) return;
    if (path.size() <= depth || path[depth] != index) {
        closeSubmenus(depth);
        path.push_back(index);
        if (auto *parent = document->GetElementById(itemId(depth, index))) {
            parent->SetClass("submenu-open", true);
            parent->SetAttribute("aria-expanded", "true");
        }
        renderPanel(depth + 1);
        positionPanels();
    }
    if (enter) {
        activeDepth = depth + 1;
        focused = -1;
        moveFocus(1);
    }
}

void ContextMenu::ProcessEvent(::Rml::Event &event)
{
    if (!isVisible()) return;
    auto *target = event.GetTargetElement();
    const auto &type = event.GetType();
    if ((type == "mousedown" || type == "click") && target == document) {
        event.StopPropagation();
        hide();
        return;
    }
    if (type == "scroll") {
        if (target && target->HasAttribute("data-menu-depth"))
            closeSubmenus(std::size_t(target->GetAttribute<int>("data-menu-depth", 0)));
        return;
    }
    if (type == "keydown") {
        const int key = event.GetParameter<int>("key_identifier", 0);
        using namespace ::Rml::Input;
        if (key == KI_ESCAPE) {
            hide();
        } else if (key == KI_UP || key == KI_DOWN || key == KI_TAB) {
            moveFocus(key == KI_UP || (key == KI_TAB && event.GetParameter<bool>("shift_key", false)) ? -1 : 1);
        } else if (key == KI_HOME || key == KI_END) {
            focused = key == KI_HOME ? -1 : 0;
            moveFocus(key == KI_HOME ? 1 : -1);
        } else if (key == KI_LEFT && !path.empty()) {
            const auto depth = activeDepth ? activeDepth - 1 : 0;
            const int parent = path[depth];
            closeSubmenus(depth);
            activeDepth = depth;
            focusItem(parent);
        } else if (key == KI_RIGHT) {
            openSubmenu(activeDepth, focused, true);
        } else if (key == KI_RETURN || key == KI_NUMPADENTER || key == KI_SPACE) {
            if (selectable(activeDepth, focused)) {
                const auto &entry = entriesAt(activeDepth)[focused];
                if (!entry.children.empty()) {
                    openSubmenu(activeDepth, focused, true);
                } else {
                    auto action = entry.action;
                    hide();
                    if (action) action();
                }
            }
        } else {
            return;
        }
        event.StopPropagation();
        return;
    }
    while (target && target != document && !target->HasAttribute("data-menu-index"))
        target = target->GetParentNode();
    if (!target || target == document) return;
    const auto depth = std::size_t(target->GetAttribute<int>("data-menu-depth", 0));
    const int index = target->GetAttribute<int>("data-menu-index", -1);
    if (depth > path.size() || !selectable(depth, index)) return;
    if (type == "mouseover") {
        activeDepth = depth;
        if (entriesAt(depth)[index].children.empty()) closeSubmenus(depth);
        focusItem(index, false);
        if (!entriesAt(depth)[index].children.empty()) openSubmenu(depth, index, false);
    } else if (type == "click") {
        event.StopPropagation();
        activeDepth = depth;
        if (!entriesAt(depth)[index].children.empty()) {
            openSubmenu(depth, index, true);
        } else {
            auto action = entriesAt(depth)[index].action;
            hide();
            if (action) action();
        }
    }
}

} // namespace Styx::UI::Rml
