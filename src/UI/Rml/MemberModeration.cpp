#include "MemberModeration.hpp"
#include "ContextMenu.hpp"
#include "RmlView.hpp"
#include "Shell.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/PermissionComputer.hpp"
#include "Core/RoleHierarchy.hpp"

#include "Platform/Timer.hpp"
#include "Platform/Trackable.hpp"
#include "Platform/Utf8.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <unordered_map>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>

namespace Styx::UI::MemberModeration {
using Discord::Permission;
namespace {
std::string tr(const char *s) { return s; }
std::string format(std::string text, std::initializer_list<std::string> values)
{
    int index = 1;
    for (const auto &value : values) {
        const auto token = "%" + std::to_string(index++);
        size_t pos = 0;
        while ((pos = text.find(token, pos)) != std::string::npos) {
            text.replace(pos, token.size(), value);
            pos += value.size();
        }
    }
    return text;
}
int integer(const std::string &value)
{
    int result = 0;
    std::from_chars(value.data(), value.data() + value.size(), result);
    return result;
}
std::string truncate(const std::string &value, size_t limit)
{
    auto utf16 = Platform::toUtf16(value);
    if (utf16.size() > limit) utf16.resize(limit);
    return Platform::fromUtf16(utf16);
}
bool canManageUser(Core::ClientInstance *instance, const MemberModerationTarget &target, Discord::Permissions permission)
{
    if (!instance || target.userId == instance->accountId())
        return false;
    if (!instance->permissions()->hasGuildPermission(instance->accountId(), target.guildId, permission))
        return false;
    const auto hierarchy = instance->selfRoleHierarchy(target.guildId);
    return hierarchy && hierarchy->outranksMember(target.userId, target.roleIds);
}
bool isAdministrator(Core::ClientInstance *instance, const MemberModerationTarget &target)
{
    const auto guild = instance->getGuild(target.guildId);
    if (!guild)
        return false;
    const auto permissions = Core::PermissionComputer::computeBasePermissions(guild->ownerId.get(), target.userId, target.guildId, target.roleIds, instance->getRolesForGuild(target.guildId));
    return permissions.testFlag(Permission::ADMINISTRATOR);
}
struct Choice
{
    int seconds;
    const char *label;
};
constexpr Choice historyChoices[] = {
    { 0, "Don't Delete Any" }, { 3600, "Previous Hour" }, { 21600, "Previous 6 Hours" }, { 43200, "Previous 12 Hours" }, { 86400, "Previous 24 Hours" }, { 259200, "Previous 3 Days" }, { 604800, "Previous 7 Days" }
};
constexpr Choice timeoutChoices[] = {
    { 60, "60 secs" }, { 300, "5 mins" }, { 600, "10 mins" }, { 3600, "1 hour" }, { 86400, "1 day" }, { 604800, "1 week" }
};
const char *const banReasons[] = { "Suspicious or spam account", "Compromised or hacked account", "Breaking server rules" };
std::unordered_map<Shell *, Controller::Impl *> controllers;
std::string esc(const std::string &s) { return Platform::htmlEscape(s); }
} // namespace

std::optional<MemberModerationTarget> targetFor(Core::ClientInstance *instance, Core::Snowflake guildId, Core::Snowflake userId)
{
    if (!instance || !guildId.isValid())
        return std::nullopt;
    const auto member = instance->users()->getMember(guildId, userId);
    if (!member)
        return std::nullopt;

    MemberModerationTarget target;
    target.guildId = guildId;
    target.userId = userId;
    target.roleIds = member->roles.valueOr({});
    if (member->communicationDisabledUntil.hasValue())
        target.timeoutUntil = member->communicationDisabledUntil.get();
    const auto user = member->user.hasValue() ? std::optional<Discord::User>(member->user.get()) : instance->users()->getUser(userId);
    if (user) {
        target.username = user->username.get();
        target.bot = user->bot.valueOr(false);
    }
    return target;
}

bool canKick(Core::ClientInstance *instance, const MemberModerationTarget &target)
{
    return canManageUser(instance, target, Permission::KICK_MEMBERS);
}

bool canBan(Core::ClientInstance *instance, const MemberModerationTarget &target)
{
    return !target.bot && canManageUser(instance, target, Permission::BAN_MEMBERS);
}

bool canTimeout(Core::ClientInstance *instance, const MemberModerationTarget &target)
{
    return canManageUser(instance, target, Permission::MODERATE_MEMBERS) && !isAdministrator(instance, target);
}

bool canChangeNickname(Core::ClientInstance *instance, const MemberModerationTarget &target)
{
    return canManageUser(instance, target, Permission::MANAGE_NICKNAMES);
}

bool canEditRoles(Core::ClientInstance *instance, const MemberModerationTarget &target)
{
    return instance && instance->permissions()->hasGuildPermission(instance->accountId(), target.guildId, Permission::MANAGE_ROLES);
}

std::string timeoutRemaining(const Platform::DateTime &until)
{
    const auto remaining = std::max<int64_t>(1, Platform::DateTime::now().secondsUntil(until));
    char clock[64];
    std::snprintf(clock, sizeof(clock), "%02lldh %02lldm %02llds",
                  static_cast<long long>((remaining % 86400) / 3600),
                  static_cast<long long>((remaining % 3600) / 60),
                  static_cast<long long>(remaining % 60));
    if (remaining < 86400) return clock;
    char days[32];
    std::snprintf(days, sizeof(days), "%02lldd ", static_cast<long long>(remaining / 86400));
    return std::string(days) + clock;
}

struct Controller::Impl : Platform::Trackable, ::Rml::EventListener
{
    enum class Mode { Kick,
                      Ban,
                      Timeout,
                      RemoveTimeout,
                      Nickname,
                      Roles };
    struct Role
    {
        Core::Snowflake id;
        std::string name;
        std::string color;
        bool assignable;
    };
    Shell &shell;
    ::Rml::ElementDocument *doc = nullptr;
    Platform::Guard<Core::ClientInstance> instance;
    MemberModerationTarget target;
    Mode mode = Mode::Kick;
    std::vector<Role> roles;
    std::vector<Core::Snowflake> roleIds;
    std::string currentNick;
    std::string filter;
    bool busy = false;
    uint64_t generation = 0;
    Platform::Timer ticker;
    Platform::Connection accountConnection;

    explicit Impl(Shell &s) : shell(s)
    {
        controllers[&shell] = this;
    }
    void tick()
    {
        if (!instance || !target.isTimedOut()) {
            hide();
            return;
        }
        if (doc && mode == Mode::RemoveTimeout) {
            if (auto *remaining = doc->GetElementById("moderation-remaining"))
                remaining->SetInnerRML(esc(format(tr("%1 has %2 remaining in timeout."), {target.username, timeoutRemaining(target.timeoutUntil)})));
            repaint();
        }
    }
    ~Impl() override
    {
        if (auto it = controllers.find(&shell); it != controllers.end() && it->second == this)
            controllers.erase(it);
        if (doc) {
            doc->RemoveEventListener("click", this);
            doc->RemoveEventListener("keydown", this);
            doc->RemoveEventListener("change", this);
            doc->RemoveEventListener("keyup", this);
            doc->Close();
        }
    }
    void repaint()
    {
        if (shell.view())
            shell.view()->requestRender();
    }
    void hide()
    {
        ++generation;
        ticker.stop();
        accountConnection.disconnect();
        if (doc) {
            doc->SetClass("open", false);
            doc->Hide();
            repaint();
        }
    }
    bool ensure()
    {
        if (doc)
            return true;
        if (!shell.view() || !shell.view()->context())
            return false;
        doc = shell.view()->context()->LoadDocument(":/ui/moderation.rml");
        if (!doc)
            return false;
        for (const char *type : { "click", "keydown", "change", "keyup" })
            doc->AddEventListener(type, this);
        return true;
    }
    ::Rml::Element *element(const char *id) const { return doc ? doc->GetElementById(id) : nullptr; }
    void text(const char *id, const std::string &value)
    {
        if (auto *e = element(id))
            e->SetInnerRML(esc(value));
    }
    std::string input(const char *id) const
    {
        auto *e = element(id);
        if (auto *v = dynamic_cast<::Rml::ElementFormControlInput *>(e))
            return v->GetValue();
        if (auto *v = dynamic_cast<::Rml::ElementFormControlTextArea *>(e))
            return v->GetValue();
        if (auto *v = dynamic_cast<::Rml::ElementFormControlSelect *>(e))
            return v->GetValue();
        return {};
    }
    void show(Mode next, Core::ClientInstance *inst, const MemberModerationTarget &member)
    {
        if (!inst || (next == Mode::RemoveTimeout && !member.isTimedOut()) || !ensure())
            return;
        accountConnection.disconnect();
        ++generation;
        ticker.stop();
        busy = false;
        instance = inst;
        target = member;
        mode = next;
        filter.clear();
        roles.clear();
        roleIds = member.roleIds;
        accountConnection = inst->destroyed.connect([this] { hide(); });
        std::string title, body, confirm;
        switch (mode) {
        case Mode::Kick:
            title = format(tr("Kick %1 from Server"), {target.username});
            confirm = tr("Kick");
            body = "<p>" + esc(format(tr("Are you sure you want to kick %1 from the server? They will be able to rejoin again with a new invite."), {target.username})) + "</p><div class='moderation-label'>" + esc(tr("Reason for Kick")) + "</div><textarea id='moderation-reason' maxlength='512'></textarea>";
            break;
        case Mode::Ban:
            title = format(tr("Ban %1?"), {target.username});
            confirm = tr("Ban");
            body = "<div class='moderation-label'>" + esc(tr("Reason for Ban")) + "</div><select id='moderation-ban-reason'><option value='-1'></option>";
            for (int i = 0; i < 3; ++i)
                body += "<option value='" + std::to_string(i) + "'>" + esc(tr(banReasons[i])) + "</option>";
            body += "<option value='3'>" + esc(tr("Other")) + "</option></select><textarea id='moderation-reason' maxlength='512' style='display:none'></textarea><div class='moderation-label'>" + esc(tr("Delete Message History")) + "</div><select id='moderation-history'>";
            for (const auto &c : historyChoices)
                body += "<option value='" + std::to_string(c.seconds) + "'" + (c.seconds == 3600 ? " selected" : "") + ">" + esc(tr(c.label)) + "</option>";
            body += "</select>";
            break;
        case Mode::Timeout:
            title = format(tr("Timeout %1"), {target.username});
            confirm = tr("Timeout");
            body = "<p>" + esc(tr("Members who are in timeout are temporarily not allowed to chat or react in text channels. They are also not allowed to connect to voice or Stage channels.")) + "</p><div class='moderation-label'>" + esc(tr("Duration")) + "</div><select id='moderation-duration'>";
            for (const auto &c : timeoutChoices)
                body += "<option value='" + std::to_string(c.seconds) + "'>" + esc(tr(c.label)) + "</option>";
            body += "</select><div class='moderation-label'>" + esc(tr("Reason")) + "</div><p>" + esc(tr("Enter a reason. This will only be visible in the Audit Log and will not be shown to the member.")) + "</p><textarea id='moderation-reason' maxlength='512'></textarea>";
            break;
        case Mode::RemoveTimeout:
            title = confirm = tr("Remove Timeout");
            body = "<p id='moderation-remaining'>" + esc(format(tr("%1 has %2 remaining in timeout."), {target.username, timeoutRemaining(target.timeoutUntil)})) + "</p><p>" + esc(tr("Remove it now to let them post and react to messages, and join voice and stage channels.")) + "</p>";
            ticker.start(std::chrono::seconds(1), true, [this] { tick(); });
            break;
        case Mode::Nickname: {
            title = tr("Change Nickname");
            confirm = tr("Save");
            const auto memberData = inst->users()->getMember(target.guildId, target.userId);
            currentNick = memberData ? memberData->nick.valueOr(std::string()) : std::string();
            body = "<p>" + esc(tr("Nicknames are visible to everyone on this server. Do not change them unless you are enforcing a naming system or clearing a bad nickname.")) + "</p><div class='moderation-label'>" + esc(format(tr("Nickname for %1"), {target.username})) + "</div><input id='moderation-nick' maxlength='32' value='" + esc(currentNick) + "'/><button data-action='reset' class='moderation-reset'>" + esc(tr("Reset Nickname")) + "</button>";
            break;
        }
        case Mode::Roles: {
            title = tr("Roles");
            confirm = tr("Done");
            const auto hierarchy = inst->selfRoleHierarchy(target.guildId);
            if (!hierarchy) return;
            for (const Discord::Role &role : hierarchy->sortedRoles()) {
                const bool assignable = !role.isManaged() && hierarchy->outranksRole(role);
                if (role.id.get() != target.guildId && (assignable || std::find(target.roleIds.begin(), target.roleIds.end(), role.id.get()) != target.roleIds.end()))
                    roles.push_back({ role.id.get(), role.name.get(), role.hasColor() ? role.getColor().css() : std::string(), assignable });
            }
            body = "<div class='moderation-label'>" + esc(tr("Search roles")) + "</div><input id='moderation-filter'/><div id='moderation-roles'></div>";
            break;
        }
        }
        text("moderation-title", title);
        text("moderation-confirm", confirm);
        text("moderation-error", {});
        if (auto *e = element("moderation-confirm"))
            e->RemoveAttribute("disabled");
        if (auto *e = element("moderation-body"))
            e->SetInnerRML(body);
        if (auto *e = element("moderation-confirm"))
            e->SetClass("danger", mode != Mode::Nickname && mode != Mode::Roles);
        if (mode == Mode::Roles)
            renderRoles();
        doc->Show(::Rml::ModalFlag::Modal);
        doc->PullToFront();
        doc->SetClass("open", true);
        repaint();
    }
    void renderRoles()
    {
        std::string html;
        const auto query = Platform::unicodeLower(Platform::trim(filter));
        for (size_t i = 0; i < roles.size(); ++i) {
            const auto &role = roles[i];
            if (Platform::unicodeLower(role.name).find(query) == std::string::npos) continue;
            const bool disabled = !role.assignable || busy;
            html += std::string("<button class='moderation-role") + (disabled ? " disabled" : "") + "' data-role='" + std::to_string(i) + "'" + (disabled ? " disabled='true'" : "") + "><span class='moderation-check'>" + (std::find(roleIds.begin(), roleIds.end(), role.id) != roleIds.end() ? "&#10003;" : "") + "</span><span class='moderation-dot' style='background-color:" + (role.color.empty() ? "#949ba4" : role.color) + "'></span>" + esc(role.name) + "</button>";
        }
        if (html.empty()) html = esc(roles.empty() ? tr("No Roles") : tr("No matching roles"));
        if (auto *e = element("moderation-roles")) e->SetInnerRML(html);
        repaint();
    }
    void roleClick(int index)
    {
        if (!instance || busy || index < 0 || static_cast<size_t>(index) >= roles.size() || !roles[index].assignable)
            return;
        const auto id = roles[index].id;
        const bool checked = std::find(roleIds.begin(), roleIds.end(), id) == roleIds.end();
        auto next = roleIds;
        std::erase(next, id);
        if (checked)
            next.push_back(id);
        const std::vector<Core::Snowflake> toggled{ id }, none;
        busy = true;
        text("moderation-error", {});
        renderRoles();
        const auto expected = generation;
        Platform::Guard<Impl> self(this);
        Platform::Guard<Core::ClientInstance> guard(instance.get());
        instance->discord()->setMemberRoles(target.guildId, target.userId, next, checked ? toggled : none, checked ? none : toggled,
                                            [self, guard, expected, next](const Core::Result<void> &result) {
                                                if (!self || !guard || self->generation != expected)
                                                    return;
                                                self->busy = false;
                                                if (result.success())
                                                    self->roleIds = next;
                                                else
                                                    self->text("moderation-error", result.error);
                                                self->renderRoles();
                                            });
    }
    void submit()
    {
        if (mode == Mode::Roles) {
            hide();
            return;
        }
        if (!instance || busy)
            return;
        const std::string reason = truncate(input("moderation-reason"), 512);
        const auto expected = generation;
        Platform::Guard<Impl> self(this);
        Platform::Guard<Core::ClientInstance> guard(instance.get());
        busy = true;
        text("moderation-error", {});
        if (auto *e = element("moderation-confirm"))
            e->SetAttribute("disabled", true);
        auto done = [self, guard, expected](const Core::Result<void> &result) {
            if (!self || !guard || self->generation != expected)
                return;
            self->busy = false;
            if (result.success())
                self->hide();
            else {
                self->text("moderation-error", result.error);
                if (auto *e = self->element("moderation-confirm"))
                    e->RemoveAttribute("disabled");
                self->repaint();
            }
        };
        switch (mode) {
        case Mode::Kick:
            instance->discord()->kickMember(target.guildId, target.userId, reason, done);
            break;
        case Mode::Ban: {
            int selected = integer(input("moderation-ban-reason"));
            std::string banReason = selected == 3 ? (Platform::trim(reason).empty() ? "other" : reason) : (selected >= 0 && selected < 3 ? tr(banReasons[selected]) : std::string());
            instance->discord()->banMember(target.guildId, target.userId, integer(input("moderation-history")), banReason, done);
            break;
        }
        case Mode::Timeout:
            instance->discord()->setMemberTimeout(target.guildId, target.userId, Platform::DateTime::now().addSeconds(integer(input("moderation-duration"))), reason, done);
            break;
        case Mode::RemoveTimeout:
            instance->discord()->setMemberTimeout(target.guildId, target.userId, Platform::DateTime(), std::nullopt, done);
            break;
        case Mode::Nickname: {
            const std::string nick(Platform::trim(truncate(input("moderation-nick"), 32)));
            if (nick == currentNick) {
                hide();
                return;
            }
            instance->discord()->setMemberNickname(target.guildId, target.userId, nick, done);
            break;
        }
        case Mode::Roles:
            break;
        }
        repaint();
    }
    void ProcessEvent(::Rml::Event &event) override
    {
        auto *e = event.GetTargetElement();
        if (!e)
            return;
        const auto type = event.GetType();
        if (type == "keydown") {
            if (event.GetParameter<int>("key_identifier", 0) == ::Rml::Input::KI_ESCAPE) {
                event.StopPropagation();
                hide();
            } else if (const int key = event.GetParameter<int>("key_identifier", 0);
                       (key == ::Rml::Input::KI_RETURN || key == ::Rml::Input::KI_NUMPADENTER || key == ::Rml::Input::KI_SPACE) &&
                       e->GetTagName() == "button") {
                event.StopPropagation();
                e->DispatchEvent("click", {});
            }
            return;
        }
        if (type == "change" || type == "keyup") {
            if (e->GetId() == "moderation-reason") {
                if (auto *area = dynamic_cast<::Rml::ElementFormControlTextArea *>(e)) {
                    const auto value = area->GetValue();
                    const auto limited = truncate(value, 512);
                    if (value != limited) area->SetValue(limited);
                }
            }
            if (e->GetId() == "moderation-filter") {
                const std::string next = input("moderation-filter");
                if (next != filter) {
                    filter = next;
                    renderRoles();
                }
            }
            if (e->GetId() == "moderation-ban-reason")
                if (auto *other = element("moderation-reason")) {
                    other->SetProperty("display", input("moderation-ban-reason") == "3" ? "block" : "none");
                    repaint();
                }
            return;
        }
        if (type != "click")
            return;
        if (e->GetId() == "moderation-backdrop" || e->GetId() == "moderation-cancel") {
            event.StopPropagation();
            hide();
            return;
        }
        if (e->GetId() == "moderation-confirm") {
            event.StopPropagation();
            submit();
            return;
        }
        for (auto *p = e; p && p != doc; p = p->GetParentNode()) {
            if (p->HasAttribute("data-role")) {
                roleClick(integer(p->GetAttribute<::Rml::String>("data-role", "-1")));
                return;
            }
            if (p->HasAttribute("data-action") && p->GetAttribute<::Rml::String>("data-action", "") == "reset") {
                if (auto *nick = dynamic_cast<::Rml::ElementFormControlInput *>(element("moderation-nick")))
                    nick->SetValue("");
                repaint();
                return;
            }
        }
    }
};

Controller::Controller(Shell &shell) : impl(std::make_unique<Impl>(shell)) {}
Controller::~Controller() = default;
namespace {
void open(Shell &shell, Core::ClientInstance *instance, const MemberModerationTarget &target, Controller::Impl::Mode mode)
{
    if (auto it = controllers.find(&shell); it != controllers.end())
        it->second->show(mode, instance, target);
}
} // namespace
void kick(Shell &shell, Core::ClientInstance *instance, const MemberModerationTarget &target)
{
    open(shell, instance, target, Controller::Impl::Mode::Kick);
}
void ban(Shell &shell, Core::ClientInstance *instance, const MemberModerationTarget &target)
{
    open(shell, instance, target, Controller::Impl::Mode::Ban);
}
void timeout(Shell &shell, Core::ClientInstance *instance, const MemberModerationTarget &target)
{
    open(shell, instance, target, Controller::Impl::Mode::Timeout);
}
void removeTimeout(Shell &shell, Core::ClientInstance *instance, const MemberModerationTarget &target)
{
    open(shell, instance, target, Controller::Impl::Mode::RemoveTimeout);
}
void changeNickname(Shell &shell, Core::ClientInstance *instance, const MemberModerationTarget &target)
{
    open(shell, instance, target, Controller::Impl::Mode::Nickname);
}
void addModerationActions(std::vector<Rml::MenuItem> &items, Shell &shell, Core::ClientInstance *instance, const MemberModerationTarget &target)
{
    if (!instance)
        return;
    Platform::Guard<Core::ClientInstance> guard(instance);
    auto add = [&](const std::string &label, auto fn, bool danger = false) {
        items.push_back({ label, [guard, &shell, target, fn] { if (guard) fn(shell, guard.get(), target); }, true, danger });
    };
    if (canChangeNickname(instance, target))
        add(tr("Change Nickname"), &changeNickname);
    if (canEditRoles(instance, target) && instance->selfRoleHierarchy(target.guildId))
        add(tr("Roles"), +[](Shell &s, Core::ClientInstance *i, const MemberModerationTarget &t) { open(s, i, t, Controller::Impl::Mode::Roles); });
    const bool t = canTimeout(instance, target), k = canKick(instance, target), b = canBan(instance, target);
    if (!t && !k && !b)
        return;
    Rml::MenuItem separator;
    separator.separator = true;
    items.push_back(std::move(separator));
    if (t) {
        if (target.isTimedOut())
            add(format(tr("Remove Timeout From %1"), {target.username}), &removeTimeout);
        else
            add(format(tr("Timeout %1"), {target.username}), &timeout);
    }
    if (k)
        add(format(tr("Kick %1"), {target.username}), &kick, true);
    if (b)
        add(format(tr("Ban %1"), {target.username}), &ban, true);
}
} // namespace Styx::UI::MemberModeration
