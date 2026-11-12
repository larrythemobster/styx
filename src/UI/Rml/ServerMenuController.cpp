#include "ServerMenuController.hpp"

#include "ConfirmModal.hpp"
#include "GuildSettingsSection.hpp"
#include "ProfileEditorController.hpp"
#include "RmlView.hpp"
#include "Shell.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/PermissionManager.hpp"
#include "Core/ReadStateManager.hpp"
#include "Discord/Client.hpp"
#include "Platform/Desktop.hpp"
#include "UI/ChannelList/ChannelTreeModel.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <string_view>

namespace Styx::UI::Rml {
namespace {
using Core::Snowflake;
using PrivacyKind = Proto::GuildPrivacyEdit::Kind;

constexpr struct PrivacyControl {
    const char *key;
    const char *title;
    const char *detail;
    PrivacyKind kind;
} privacyControls[] = {
    {"dms", "Direct Messages", "Allow DMs from other members in this server.", PrivacyKind::DirectMessages},
    {"requests", "Message requests", "Filter messages from server members you may not know.", PrivacyKind::MessageRequests},
    {"activity", "Share my activity", "Share activity from games and connected apps in this server.", PrivacyKind::Activity},
    {"joining", "Activity joining", "Allow users to join your activity in this server.", PrivacyKind::ActivityJoining}
};

bool privacyEnabled(const Proto::PrivacySettings *privacy, PrivacyKind kind, Snowflake guild)
{
    if (!privacy) return true;
    const auto *ids = &privacy->restrictedGuildIds;
    switch (kind) {
    case PrivacyKind::DirectMessages: break;
    case PrivacyKind::MessageRequests: ids = &privacy->messageRequestRestrictedGuildIds; break;
    case PrivacyKind::Activity: ids = &privacy->activityRestrictedGuildIds; break;
    case PrivacyKind::ActivityJoining: ids = &privacy->activityJoiningRestrictedGuildIds; break;
    }
    return std::find(ids->begin(), ids->end(), guild) == ids->end();
}

bool invitable(Core::ClientInstance *instance, const ChannelNode *node)
{
    return instance && node && (node->type == ChannelNode::Type::Channel || node->type == ChannelNode::Type::VoiceChannel ||
           node->type == ChannelNode::Type::Forum) &&
           instance->permissions()->hasChannelPermission(instance->accountId(), node->id, Discord::Permission::VIEW_CHANNEL) &&
           instance->permissions()->hasChannelPermission(instance->accountId(), node->id, Discord::Permission::CREATE_INSTANT_INVITE);
}

template<class Visit> void visitChannels(ChannelNode *node, const Visit &visit)
{
    if (!node) return;
    for (const auto &child : node->children) {
        visit(child.get());
        if (child->type == ChannelNode::Type::Category) visitChannels(child.get(), visit);
    }
}

MenuItem separator() { MenuItem item; item.separator = true; return item; }
MenuItem choice(std::string label, bool checked, std::function<void()> action, bool radio = false)
{
    MenuItem item{std::move(label), std::move(action)};
    item.checked = checked;
    item.radio = radio;
    return item;
}

std::uint64_t number(std::string_view text)
{
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() ? value : 0;
}
}

ServerMenuController::ServerMenuController(Shell &shell) : Controller(shell, nullptr) {}

ServerMenuController::~ServerMenuController()
{
    profileEditor.reset();
    removeListeners();
    if (document) document->Close();
}

void ServerMenuController::show(Snowflake account, Snowflake guild, bool developerMode)
{
    auto *instance = shell.instanceFor(account);
    const auto data = instance ? instance->getGuild(guild) : std::nullopt;
    if (!data) return;
    Platform::Guard<ServerMenuController> self(this);
    auto *node = shell.channelTree()->serverNode(account, guild);
    std::vector<MenuItem> menu;
    menu.reserve(18);
    menu.push_back({"Mark As Read", [this, account, guild] { shell.markGuildRead(account, guild); },
                    node && (node->isUnread || node->mentionCount > 0)});
    menu.push_back(separator());
    bool canInvite = false;
    visitChannels(node, [&](const auto *channel) { if (invitable(instance, channel)) canInvite = true; });
    MenuItem invite{"Invite to Server", [self, account, guild] { if (self) self->openPage(account, guild, Page::Invite); }, canInvite};
    invite.color = "#00a8fc";
    menu.push_back(std::move(invite));
    menu.push_back(separator());
    auto *readState = instance->readState();
    const auto *preferences = readState->guildSettings(guild);
    const bool muted = readState->isGuildMuted(guild);
    if (muted) {
        MenuItem unmute{"Unmute Server", [self, account, guild] { if (self) self->setMute(account, guild, 0); }};
        if (preferences && preferences->muteConfig.hasValue() && preferences->muteConfig->endTime.hasValue()) {
            const auto until = Platform::DateTime::parse(preferences->muteConfig->endTime.get());
            if (until.valid()) unmute.subtitle = "Muted until " + until.format("MMM d, h:mm AP");
        }
        menu.push_back(std::move(unmute));
    } else {
        MenuItem mute{"Mute Server"};
        for (const auto &[label, seconds] : std::array<std::pair<const char *, int>, 6>{{
                 {"For 15 Minutes", 900}, {"For 1 Hour", 3600}, {"For 3 Hours", 10800},
                 {"For 8 Hours", 28800}, {"For 24 Hours", 86400}, {"Until I turn it back on", -1}}})
            mute.children.push_back({label, [self, account, guild, seconds] { if (self) self->setMute(account, guild, seconds); }});
        menu.push_back(std::move(mute));
    }
    MenuItem notifications{"Notification Settings"};
    const auto level = readState->resolveMessageNotifications(guild, {}, {});
    notifications.subtitle = level == Discord::MessageNotificationLevel::ALL_MESSAGES ? "All Messages" :
                             level == Discord::MessageNotificationLevel::NO_MESSAGES ? "Nothing" : "Only @mentions";
    notifications.children = notificationItems(account, guild);
    menu.push_back(std::move(notifications));
    const bool hideMuted = preferences && preferences->hideMutedChannels.valueOr(false);
    menu.push_back(choice("Hide Muted Channels", hideMuted, [self, account, guild, hideMuted] {
        if (!self) return;
        Discord::UserGuildSettingsEdit edit; edit.hideMutedChannels = !hideMuted;
        self->applyGuildEdit(account, guild, edit);
    }));
    const auto &features = data->features.get();
    if (std::find(features.begin(), features.end(), "COMMUNITY") != features.end()) {
        const bool showAll = !readState->isOptInGuild(guild);
        const int flags = preferences ? preferences->flags.valueOr(0) : 0;
        menu.push_back(choice("Show All Channels", showAll, [self, account, guild, showAll, flags] {
            if (!self) return;
            constexpr int off = int(Discord::UserGuildSettingsFlag::OPT_IN_CHANNELS_OFF);
            constexpr int on = int(Discord::UserGuildSettingsFlag::OPT_IN_CHANNELS_ON);
            Discord::UserGuildSettingsEdit edit; edit.flags = (flags & ~(off | on)) | (showAll ? on : off);
            self->applyGuildEdit(account, guild, edit);
        }));
    }
    menu.push_back(separator());
    MenuItem administration{"Server Settings"};
    for (auto section : GuildSettingsAccess::visibleSections(instance, guild)) {
        if (section != GuildSettingsSection::DeleteServer && GuildSettingsAccess::canEdit(instance, guild, section))
            administration.children.push_back({GuildSettingsAccess::title(section), [this, account, guild, section] {
                shell.openGuildSettings(account, guild, section);
            }});
    }
    if (!administration.children.empty()) menu.push_back(std::move(administration));
    menu.push_back({"Privacy Settings", [self, account, guild] { if (self) self->openPage(account, guild, Page::Privacy); }});
    menu.push_back({"Edit Per-server Profile", [self, account, guild] { if (self) self->openPage(account, guild, Page::Profile); }});
    menu.push_back(separator());
    MenuItem move{"Move to..."}; move.children = moveItems(account, guild); menu.push_back(std::move(move));
    if (!data->ownerId.hasValue() || data->ownerId.get() != account) {
        menu.push_back(separator());
        const auto name = data->name.get();
        menu.push_back({"Leave Server", [self, account, guild, name] { if (self) self->leave(account, guild, name); }, true, true});
    }
    if (developerMode) {
        menu.push_back(separator());
        MenuItem copy{"Copy Server ID", [guild] { Platform::Desktop::setClipboardText(guild.toString()); }};
        copy.iconText = "ID"; menu.push_back(std::move(copy));
    }
    shell.showContextMenu(std::move(menu));
}

void ServerMenuController::setMute(Snowflake account, Snowflake guild, int seconds)
{
    Discord::UserGuildSettingsEdit edit;
    edit.muted = seconds != 0;
    if (seconds == 0) {
        edit.muteConfig = nullptr;
    } else {
        Discord::MuteConfig config;
        config.selectedTimeWindow = seconds;
        if (seconds < 0) config.endTime = nullptr;
        else config.endTime = Platform::DateTime::now().addSeconds(seconds).iso8601();
        edit.muteConfig = std::move(config);
    }
    applyGuildEdit(account, guild, edit);
}

void ServerMenuController::applyGuildEdit(Snowflake account, Snowflake guild, const Discord::UserGuildSettingsEdit &edit)
{
    auto *instance = shell.instanceFor(account);
    if (!instance) return;
    Platform::Guard<ServerMenuController> self(this);
    instance->discord()->modifyUserGuildSettings(guild, edit, [self](const Core::Result<void> &result) {
        if (self && !result.success()) showConfirmModal(self->shell, ConfirmOptions{
            "Could not save server preferences", Platform::htmlEscape(result.error), "Close", ""});
    });
}

void ServerMenuController::applyProtoEdit(Snowflake account, Proto::PreloadedSettingsEdit edit)
{
    auto *instance = shell.instanceFor(account);
    if (!instance) return;
    Platform::Guard<ServerMenuController> self(this);
    instance->discord()->modifyPreloadedSettings(std::move(edit), [self](const Core::Result<void> &result) {
        if (self && !result.success()) showConfirmModal(self->shell, ConfirmOptions{
            "Could not update server settings", Platform::htmlEscape(result.error), "Close", ""});
    });
}

std::vector<MenuItem> ServerMenuController::notificationItems(Snowflake account, Snowflake guild)
{
    auto *instance = shell.instanceFor(account);
    auto *readState = instance->readState();
    const auto *settings = readState->guildSettings(guild);
    const auto level = readState->resolveMessageNotifications(guild, {}, {});
    Platform::Guard<ServerMenuController> self(this);
    std::vector<MenuItem> items;
    items.reserve(10);
    for (const auto &[label, value] : std::array<std::pair<const char *, Discord::MessageNotificationLevel>, 3>{{
             {"All Messages", Discord::MessageNotificationLevel::ALL_MESSAGES},
             {"Only @mentions", Discord::MessageNotificationLevel::ONLY_MENTIONS},
             {"Nothing", Discord::MessageNotificationLevel::NO_MESSAGES}}}) {
        items.push_back(choice(label, level == value, [self, account, guild, value] {
            if (!self) return;
            Discord::UserGuildSettingsEdit edit; edit.messageNotifications = value;
            self->applyGuildEdit(account, guild, edit);
        }, true));
    }
    items.push_back(separator());
    const bool everyone = settings && settings->suppressEveryone.valueOr(false);
    items.push_back(choice("Suppress @everyone and @here", everyone, [self, account, guild, everyone] {
        if (!self) return;
        Discord::UserGuildSettingsEdit edit; edit.suppressEveryone = !everyone; self->applyGuildEdit(account, guild, edit);
    }));
    const bool roles = settings && settings->suppressRoles.valueOr(false);
    items.push_back(choice("Suppress All Role @mentions", roles, [self, account, guild, roles] {
        if (!self) return;
        Discord::UserGuildSettingsEdit edit; edit.suppressRoles = !roles; self->applyGuildEdit(account, guild, edit);
    }));
    const bool highlights = settings && settings->notifyHighlights.valueOr(0) == 1;
    items.push_back(choice("Suppress Highlights", highlights, [self, account, guild, highlights] {
        if (!self) return;
        Discord::UserGuildSettingsEdit edit; edit.notifyHighlights = highlights ? 0 : 1; self->applyGuildEdit(account, guild, edit);
    }));
    const bool events = settings && settings->muteScheduledEvents.valueOr(false);
    items.push_back(choice("Mute New Events", events, [self, account, guild, events] {
        if (!self) return;
        Discord::UserGuildSettingsEdit edit; edit.muteScheduledEvents = !events; self->applyGuildEdit(account, guild, edit);
    }));
    items.push_back(separator());
    const bool push = !settings || settings->mobilePush.valueOr(true);
    items.push_back(choice("Mobile Push Notifications", push, [self, account, guild, push] {
        if (!self) return;
        Discord::UserGuildSettingsEdit edit; edit.mobilePush = !push; self->applyGuildEdit(account, guild, edit);
    }));
    return items;
}

std::vector<MenuItem> ServerMenuController::moveItems(Snowflake account, Snowflake guild)
{
    auto *instance = shell.instanceFor(account);
    const auto &settings = instance->discord()->getSettings();
    const Proto::GuildFolder *current = nullptr;
    if (settings.guildFolders) {
        for (const auto &folder : settings.guildFolders->folders)
            if (std::find(folder.guildIds.begin(), folder.guildIds.end(), guild) != folder.guildIds.end()) { current = &folder; break; }
    }
    Platform::Guard<ServerMenuController> self(this);
    auto action = [self, account, guild](std::optional<int64_t> folderId, Proto::GuildFolderMove::Kind kind) {
        return [self, account, guild, folderId, kind] {
            if (!self) return;
            Proto::PreloadedSettingsEdit edit; edit.guildFolderMove = Proto::GuildFolderMove{guild, folderId, kind};
            self->applyProtoEdit(account, std::move(edit));
        };
    };
    MenuItem folders{"Folder"};
    const bool root = !current || !current->id.has_value();
    folders.children.push_back(choice("No Folder", root, action({}, Proto::GuildFolderMove::Kind::Folder), true));
    if (settings.guildFolders) {
        for (const auto &folder : settings.guildFolders->folders) {
            if (!folder.id.has_value()) continue;
            std::string name = folder.name.value_or("");
            if (name.empty()) {
                for (auto id : folder.guildIds) {
                    const auto data = instance->getGuild(id);
                    if (!data) continue;
                    if (!name.empty()) name += ", ";
                    name += data->name.get();
                }
            }
            if (name.empty()) name = "Folder";
            folders.children.push_back(choice(std::move(name), current && current->id == folder.id,
                action(folder.id, Proto::GuildFolderMove::Kind::Folder), true));
        }
    }
    const bool first = current && current->id && !current->guildIds.empty() && current->guildIds.front() != guild;
    const bool last = current && current->id && !current->guildIds.empty() && current->guildIds.back() != guild;
    return {std::move(folders), separator(),
            {"First in Folder", action({}, Proto::GuildFolderMove::Kind::First), first},
            {"Last in Folder", action({}, Proto::GuildFolderMove::Kind::Last), last}};
}

void ServerMenuController::ensureDocument()
{
    if (document || !shell.view() || !shell.view()->context()) return;
    document = shell.view()->context()->LoadDocument(":/ui/server_menu.rml");
    if (!document) return;
    regionRoot = document;
    listen("click", [this](::Rml::Event &event) {
        if (auto *target = closest(event.GetTargetElement(), "data-action")) {
            const auto action = attribute(target, "data-action");
            if (action == "close") close();
            else if (action == "create-invite") createInvite();
            else if (action == "copy-invite" && !inviteUrl.empty()) {
                Platform::Desktop::setClipboardText(inviteUrl);
                if (auto *label = document->GetElementById("server-invite-copy-label")) label->SetInnerRML("Copied!");
            }
        } else if (auto *target = closest(event.GetTargetElement(), "data-privacy")) {
            togglePrivacy(attribute(target, "data-privacy"));
        }
    });
    listen("keydown", [this](::Rml::Event &event) {
        const int key = event.GetParameter<int>("key_identifier", 0);
        if (key == ::Rml::Input::KI_ESCAPE) {
            close(); event.StopPropagation();
        } else if ((key == ::Rml::Input::KI_RETURN || key == ::Rml::Input::KI_SPACE) &&
                   closest(event.GetTargetElement(), "data-privacy")) {
            togglePrivacy(attribute(closest(event.GetTargetElement(), "data-privacy"), "data-privacy"));
            event.StopPropagation();
        }
    });
}

void ServerMenuController::openPage(Snowflake account, Snowflake guild, Page next)
{
    auto *instance = shell.instanceFor(account);
    const auto data = instance ? instance->getGuild(guild) : std::nullopt;
    if (!data) return;
    ensureDocument();
    if (!document) return;
    ++generation;
    profileEditor.reset();
    origin = Platform::Guard<Core::ClientInstance>(instance);
    originDestroyed.disconnect();
    Platform::Guard<ServerMenuController> self(this);
    originDestroyed = instance->destroyed.connect([self] { if (self) self->hide(); });
    currentGuild = guild;
    page = next;
    saving = false;
    visible = true;
    inviteUrl.clear();
    setError({});
    const std::string title = next == Page::Privacy ? "Privacy Settings" : next == Page::Profile ? "Server Profile" : "Invite to Server";
    document->GetElementById("server-options-title")->SetInnerRML(title);
    document->GetElementById("server-options-name")->SetInnerRML(Platform::htmlEscape(data->name.get()));
    document->GetElementById("server-options-dialog")->SetClass("profile", next == Page::Profile);
    document->GetElementById("server-options-footer")->SetProperty("display", next == Page::Profile ? "none" : "flex");
    auto *content = document->GetElementById("server-options-content");
    if (next == Page::Privacy) renderPrivacy();
    else if (next == Page::Invite) renderInvite();
    else {
        content->SetInnerRML("<div id='server-profile-host'></div>");
        profileEditor = std::make_unique<ProfileEditorController>(shell, *content->GetElementById("server-profile-host"));
        profileEditor->open(guild, account);
    }
    document->Show(::Rml::ModalFlag::Modal);
    document->PullToFront();
    requestRender();
}

void ServerMenuController::close()
{
    if (profileEditor && profileEditor->isSaving()) return;
    if (profileEditor && profileEditor->hasPendingChanges()) {
        Platform::Guard<ServerMenuController> self(this);
        showConfirmModal(shell, "Discard server profile changes?", "Your unsaved changes will be lost.", "Discard",
            [self] { if (self) self->hide(); }, nullptr, true);
        return;
    }
    hide();
}

void ServerMenuController::hide()
{
    ++generation;
    profileEditor.reset();
    originDestroyed.disconnect();
    origin.reset();
    visible = false;
    saving = false;
    if (document) document->Hide();
    requestRender();
}

void ServerMenuController::setError(const std::string &message)
{
    if (!document) return;
    auto *error = document->GetElementById("server-options-error");
    error->SetInnerRML(Platform::htmlEscape(message));
    error->SetProperty("display", message.empty() ? "none" : "block");
    requestRender();
}

void ServerMenuController::renderPrivacy()
{
    if (!origin || !document) return;
    const auto &preferences = origin->discord()->getSettings().privacy;
    std::string markup;
    for (const auto &control : privacyControls) {
        const bool enabled = privacyEnabled(preferences ? &*preferences : nullptr, control.kind, currentGuild);
        markup += "<div class='settings-switch-row' data-privacy='" + std::string(control.key) + "'><div class='settings-switch-text'>";
        markup += "<div class='settings-switch-title'>" + std::string(control.title) + "</div>";
        markup += "<div class='settings-switch-desc'>" + std::string(control.detail) + "</div></div>";
        markup += "<div id='server-privacy-" + std::string(control.key) + "' class='settings-switch" + (enabled ? " checked" : "") +
                  (saving ? " disabled" : "") + "' role='switch' aria-label='" + control.title + "' aria-checked='" +
                  (enabled ? "true" : "false") + "' tabindex='0'><div class='settings-switch-thumb'></div></div></div>";
    }
    document->GetElementById("server-options-content")->SetInnerRML(markup);
    requestRender();
}

void ServerMenuController::togglePrivacy(const std::string &key)
{
    if (!origin || saving || page != Page::Privacy || !visible) return;
    const auto control = std::find_if(std::begin(privacyControls), std::end(privacyControls), [&](const auto &item) { return key == item.key; });
    if (control == std::end(privacyControls)) return;
    const auto &privacy = origin->discord()->getSettings().privacy;
    Proto::PreloadedSettingsEdit edit;
    edit.guildPrivacy = Proto::GuildPrivacyEdit{currentGuild, privacyEnabled(privacy ? &*privacy : nullptr, control->kind, currentGuild), control->kind};
    saving = true;
    setError({});
    renderPrivacy();
    const auto request = generation;
    Platform::Guard<ServerMenuController> self(this);
    origin->discord()->modifyPreloadedSettings(std::move(edit), [self, request, key](const Core::Result<void> &result) {
        if (!self || !self->visible || self->generation != request || self->page != Page::Privacy) return;
        self->saving = false;
        self->renderPrivacy();
        self->setError(result.success() ? "" : result.error);
        if (auto *control = self->document->GetElementById("server-privacy-" + key)) control->Focus();
    });
}

void ServerMenuController::renderInvite()
{
    if (!origin || !document) return;
    std::string options;
    auto *node = shell.channelTree()->serverNode(origin->accountId(), currentGuild);
    visitChannels(node, [&](const ChannelNode *channel) {
        if (invitable(origin.get(), channel))
            options += "<option value='" + channel->id.toString() + "'># " + Platform::htmlEscape(channel->name) + "</option>";
    });
    std::string markup = "<div class='settings-note'>Share an invite link to bring someone into this server.</div>";
    markup += "<label class='server-field-label'>Invite channel</label><select id='server-invite-channel' class='settings-select'>" + options + "</select>";
    markup += "<label class='server-field-label'>Expire after</label><select id='server-invite-age' class='settings-select'>"
              "<option value='604800'>7 days</option><option value='86400'>1 day</option><option value='43200'>12 hours</option>"
              "<option value='21600'>6 hours</option><option value='3600'>1 hour</option><option value='1800'>30 minutes</option><option value='0'>Never</option></select>";
    markup += "<label class='server-field-label'>Maximum uses</label><select id='server-invite-uses' class='settings-select'>"
              "<option value='0'>No limit</option><option value='1'>1</option><option value='5'>5</option><option value='10'>10</option>"
              "<option value='25'>25</option><option value='50'>50</option><option value='100'>100</option></select>";
    markup += "<div class='server-invite-actions'><button class='settings-btn settings-btn-primary' data-action='create-invite' id='server-invite-create'"
              + std::string(options.empty() ? " disabled='disabled'" : "") + "><span class='settings-btn-label'>Create Invite Link</span></button></div>";
    markup += "<div id='server-invite-result' style='display:none'><input id='server-invite-link' class='settings-input' type='text' readonly='readonly'/>"
              "<button class='settings-btn settings-btn-secondary' data-action='copy-invite'><span id='server-invite-copy-label' class='settings-btn-label'>Copy</span></button></div>";
    document->GetElementById("server-options-content")->SetInnerRML(markup);
}

void ServerMenuController::createInvite()
{
    if (!origin || saving || !visible || page != Page::Invite) return;
    auto selected = [this](const char *id) {
        auto *select = static_cast<::Rml::ElementFormControlSelect *>(document->GetElementById(id));
        return select ? number(select->GetValue()) : 0;
    };
    const Snowflake channel(selected("server-invite-channel"));
    const auto *node = shell.channelTree()->findChannelTreeNode(channel, origin->accountId());
    if (!invitable(origin.get(), node)) { setError("Select a channel you can create invites for."); return; }
    saving = true;
    setError({});
    document->GetElementById("server-invite-create")->SetAttribute("disabled", "disabled");
    const auto request = generation;
    Platform::Guard<ServerMenuController> self(this);
    origin->discord()->createInvite(channel, int(selected("server-invite-age")), int(selected("server-invite-uses")), false,
        [self, request](const Core::Result<Discord::Invite> &result) {
            if (!self || !self->visible || self->generation != request || self->page != Page::Invite) return;
            self->saving = false;
            self->document->GetElementById("server-invite-create")->RemoveAttribute("disabled");
            if (!result.success()) { self->setError(result.error); return; }
            self->inviteUrl = "https://discord.gg/" + result.value->code.get();
            self->document->GetElementById("server-invite-link")->SetAttribute("value", self->inviteUrl);
            self->document->GetElementById("server-invite-result")->SetProperty("display", "flex");
            self->requestRender();
        });
}

void ServerMenuController::leave(Snowflake account, Snowflake guild, const std::string &name)
{
    Platform::Guard<ServerMenuController> self(this);
    Platform::Guard<Core::ClientInstance> instance(shell.instanceFor(account));
    showConfirmModal(shell, "Leave " + name + "?", "You will need a new invite to join <b>" + Platform::htmlEscape(name) + "</b> again.",
        "Leave Server", [self, instance, guild] {
            if (!self || !instance) return;
            instance->discord()->leaveGuild(guild);
        }, nullptr, true);
}

} // namespace Styx::UI::Rml
