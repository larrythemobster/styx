#include "ContextMenu.hpp"
#include "RmlView.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <algorithm>

namespace Styx::UI::Rml {

ContextMenu::ContextMenu(View &view) : view(view)
{
    connect(&view, &View::documentLoaded, this, [this] { hide(); });
}

ContextMenu::~ContextMenu()
{
    if (document) {
        for (const char *type : { "click", "mousedown", "keydown", "mouseover" })
            document->RemoveEventListener(type, this);
        document->Close();
    }
}

bool ContextMenu::isVisible() const
{
    return document && document->IsVisible();
}

void ContextMenu::show(std::vector<MenuItem> entries, const QPoint &position)
{
    if (!view.context())
        return;
    if (!document) {
        document = view.context()->LoadDocument(":/ui/context_menu.rml");
        if (!document)
            return;
        for (const char *type : { "click", "mousedown", "keydown", "mouseover" })
            document->AddEventListener(type, this);
    }
    items = std::move(entries);
    path.clear();
    focused = -1;
    anchor = position;
    document->Show(::Rml::ModalFlag::Modal);
    document->PullToFront();
    render();
    moveFocus(1);
}

void ContextMenu::hide()
{
    if (document)
        document->Hide();
    items.clear();
    path.clear();
    focused = -1;
    view.requestRender();
}

const std::vector<MenuItem> &ContextMenu::currentItems() const
{
    auto *result = &items;
    for (int index : path)
        result = &(*result)[index].children;
    return *result;
}

bool ContextMenu::selectable(int index) const
{
    const auto &entries = currentItems();
    return index >= 0 && index < int(entries.size()) && entries[index].enabled &&
           !entries[index].separator && (entries[index].action || !entries[index].children.empty());
}

void ContextMenu::render()
{
    auto *panel = document->GetElementById("context-menu-panel");
    QString markup;
    if (!path.empty())
        markup += QStringLiteral("<button id='context-menu-back'>&lt; %1</button>").arg(tr("Back").toHtmlEscaped());
    const auto &entries = currentItems();
    for (int i = 0; i < int(entries.size()); ++i) {
        const auto &item = entries[i];
        if (item.separator) {
            markup += QStringLiteral("<div class='menu-separator'></div>");
            continue;
        }
        const bool actionable = bool(item.action) || !item.children.empty();
        markup += QStringLiteral("<%1 id='menu-item-%2' data-menu-index='%2' class='menu-item%3%4%5' %6>")
                          .arg(actionable ? QStringLiteral("button") : QStringLiteral("div"))
                          .arg(i)
                          .arg(item.danger ? QStringLiteral(" danger") : QString())
                          .arg(actionable ? QString() : QStringLiteral(" menu-label"))
                          .arg(item.enabled ? QString() : QStringLiteral(" disabled"))
                          .arg(item.enabled && actionable ? QString() : QStringLiteral("disabled='disabled'"));
        if (!item.imageSource.isEmpty())
            markup += QStringLiteral("<img class='menu-avatar' src='%1'/>").arg(item.imageSource.toHtmlEscaped());
        markup += QStringLiteral("<div class='menu-text'%1>%2")
                          .arg(item.color.isEmpty() ? QString() : QStringLiteral(" style='color:%1;'").arg(item.color.toHtmlEscaped()), item.label.toHtmlEscaped());
        if (!item.subtitle.isEmpty())
            markup += QStringLiteral("<div class='menu-subtitle'>%1</div>").arg(item.subtitle.toHtmlEscaped());
        markup += QStringLiteral("</div>");
        if (!item.children.empty())
            markup += QStringLiteral("<span class='menu-arrow'>&gt;</span>");
        markup += actionable ? QStringLiteral("</button>") : QStringLiteral("</div>");
    }
    panel->SetInnerRML(markup.toStdString());
    const float dpr = float(view.devicePixelRatioF());
    panel->SetProperty("left", "0px");
    panel->SetProperty("top", "0px");
    view.context()->Update();
    const auto size = panel->GetOffsetWidth();
    const auto height = panel->GetOffsetHeight();
    const auto dimensions = view.context()->GetDimensions();
    const float margin = 4 * dpr;
    const float x = std::clamp(float(anchor.x()) * dpr, margin, std::max(margin, dimensions.x - size - margin));
    const float y = std::clamp(float(anchor.y()) * dpr, margin, std::max(margin, dimensions.y - height - margin));
    panel->SetProperty("left", std::to_string(x) + "px");
    panel->SetProperty("top", std::to_string(y) + "px");
    view.requestRender();
}

void ContextMenu::focusItem(int index)
{
    if (!selectable(index))
        return;
    focused = index;
    if (auto *element = document->GetElementById("menu-item-" + std::to_string(index))) {
        element->Focus();
        element->ScrollIntoView();
    }
    view.requestRender();
}

void ContextMenu::moveFocus(int direction)
{
    const int count = int(currentItems().size());
    for (int step = 1; step <= count; ++step) {
        const int index = (focused + direction * step + count) % count;
        if (selectable(index)) {
            focusItem(index);
            return;
        }
    }
}

void ContextMenu::openSubmenu(int index)
{
    if (!selectable(index) || currentItems()[index].children.empty())
        return;
    path.push_back(index);
    focused = -1;
    render();
    moveFocus(1);
}

void ContextMenu::ProcessEvent(::Rml::Event &event)
{
    if (!isVisible())
        return;
    auto *target = event.GetTargetElement();
    if ((event.GetType() == "mousedown" || event.GetType() == "click") && target == document) {
        event.StopPropagation();
        hide();
        return;
    }
    if (event.GetType() == "keydown") {
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
            const int parent = path.back();
            path.pop_back();
            render();
            focusItem(parent);
        } else if (key == KI_RIGHT) {
            openSubmenu(focused);
        } else if (key == KI_RETURN || key == KI_NUMPADENTER || key == KI_SPACE) {
            if (selectable(focused)) {
                if (!currentItems()[focused].children.empty()) {
                    openSubmenu(focused);
                } else {
                    auto action = currentItems()[focused].action;
                    hide();
                    if (action)
                        action();
                }
            }
        } else {
            return;
        }
        event.StopPropagation();
        return;
    }
    if (event.GetType() == "click" && target && target->GetId() == "context-menu-back" && !path.empty()) {
        const int parent = path.back();
        path.pop_back();
        render();
        focusItem(parent);
        event.StopPropagation();
        return;
    }
    while (target && target != document && !target->HasAttribute("data-menu-index"))
        target = target->GetParentNode();
    if (!target || target == document)
        return;
    const int index = target->GetAttribute<int>("data-menu-index", -1);
    if (!selectable(index))
        return;
    if (event.GetType() == "mouseover") {
        focusItem(index);
    } else if (event.GetType() == "click") {
        event.StopPropagation();
        if (!currentItems()[index].children.empty()) {
            openSubmenu(index);
        } else {
            auto action = currentItems()[index].action;
            hide();
            if (action)
                action();
        }
    }
}

} // namespace Styx::UI::Rml
