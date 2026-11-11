#include "TitleBarController.hpp"
#include "InboxController.hpp"
#include "UpdatesController.hpp"
#include "Shell.hpp"
#include "UI/ChannelList/ChannelTreeModel.hpp"
#include "Platform/Native.hpp"
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

namespace Styx::UI::Rml {
TitleBarController::TitleBarController(Shell &shell, ::Rml::Element *root, InboxController &inbox, UpdatesController &updates)
    : Controller(shell, root), inbox(inbox), updates(updates)
{
    listen("click", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        if (auto *button = closest(target, "data-window-action")) {
            auto action = attribute(button, "data-window-action");
            if (action == "minimize") this->shell.minimize();
            else if (action == "maximize") { if (this->shell.isMaximized()) this->shell.showNormal(); else this->shell.showMaximized(); }
            else if (action == "close") this->shell.close();
            return;
        }
        auto *button = closest(target, "data-toolbar-action");
        if (!button || button->HasAttribute("disabled")) return;
        auto action = attribute(button, "data-toolbar-action");
        if (action == "back" && this->shell.canNavigateBack()) this->shell.navigateBack();
        else if (action == "forward" && this->shell.canNavigateForward()) this->shell.navigateForward();
        else if (action == "inbox" && this->shell.currentInstance()) { this->updates.hide(); this->inbox.toggle(); }
        else if (action == "update") { this->inbox.hide(); this->updates.toggle(); }
        else if (action == "help") { this->inbox.hide(); this->updates.hide(); this->shell.toggleShortcutHelp(); }
    });
    listen("keydown", [](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        int key = event.GetParameter<int>("key_identifier", 0);
        if (target && !target->HasAttribute("disabled") && (target->HasAttribute("data-window-action") || target->HasAttribute("data-toolbar-action")) && (key == ::Rml::Input::KI_RETURN || key == ::Rml::Input::KI_NUMPADENTER || key == ::Rml::Input::KI_SPACE)) {
            event.StopPropagation(); target->DispatchEvent("click", {});
        }
    });
    track(shell.windowStateChanged.connect([this] { refresh(); }));
    track(shell.navigationHistoryChanged.connect([this] { refresh(); }));
    track(shell.locationChanged.connect([this](const Location &) { refresh(); }));
    track(shell.accountReady.connect([this](Core::Snowflake) { refresh(); }));
    track(shell.instanceChanged.connect([this](Core::ClientInstance *) { refresh(); }));
    track(updates.changed.connect([this] { refresh(); }));
    if (auto *tree = shell.channelTree()) {
        track(tree->changed.connect([this](ChannelNode *) { refresh(); }));
        track(tree->reset.connect([this] { refresh(); }));
    }
    refresh();
}
TitleBarController::~TitleBarController() = default;
void TitleBarController::refresh() {
    if (!regionRoot) return;
    auto setDisabled = [](auto *button, bool disabled) {
        if (!button) return;
        button->SetClass("disabled", disabled);
        button->SetPseudoClass("disabled", disabled);
        button->SetAttribute("aria-disabled", disabled ? "true" : "false");
        if (disabled) button->SetAttribute("disabled", "");
        else button->RemoveAttribute("disabled");
    };
    setDisabled(regionRoot->GetElementById("titlebar-back"), !shell.canNavigateBack());
    setDisabled(regionRoot->GetElementById("titlebar-forward"), !shell.canNavigateForward());
    if (auto *button = regionRoot->GetElementById("titlebar-inbox")) {
        bool disabled = !shell.currentInstance();
        setDisabled(button, disabled);
        button->SetAttribute("title", disabled ? "Inbox unavailable until an account is connected" : "Inbox");
        if (auto *tooltip = regionRoot->GetElementById("titlebar-inbox-tooltip"))
            tooltip->SetInnerRML(disabled ? "Connect an account to open Inbox" : "Inbox");
    }
    if (auto *button = regionRoot->GetElementById("titlebar-update")) button->SetClass("available", updates.updateAvailable());
    const bool maximized = shell.isMaximized();
    const char *label = maximized ? "Restore" : "Maximize";
    if (auto *button = regionRoot->GetElementById("titlebar-maximize")) { button->SetClass("maximized", maximized); button->SetAttribute("title", label); button->SetAttribute("aria-label", label); }
    if (auto *tooltip = regionRoot->GetElementById("titlebar-maximize-tooltip")) tooltip->SetInnerRML(label);
    std::string context = shell.isFriendsOpen() ? "Friends" : "Direct Messages";
    if (!shell.location().isHome()) {
        auto *tree = shell.channelTree();
        auto *guild = tree ? tree->serverNode(shell.location().accountId, shell.location().guildId) : nullptr;
        context = guild && !guild->name.empty() ? guild->name : "Server";
    }
    if (auto *title = regionRoot->GetElementById("titlebar-context")) title->SetInnerRML(Platform::htmlEscape(context));
    if (auto *icon = regionRoot->GetElementById("titlebar-context-icon")) icon->SetAttribute("src", shell.location().isHome() ? "icon:gamepad-2" : "icon:hash");
    requestRender();
}
}
