#include "MemberModeration.hpp"
#include "ContextMenu.hpp"
#include "RmlView.hpp"
#include "Shell.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/PermissionComputer.hpp"
#include "Core/RoleHierarchy.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QHash>
#include <QPointer>
#include <QTimer>
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
QString tr(const char *s)
{
    return QCoreApplication::translate("MemberModeration", s);
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
    { 0, QT_TRANSLATE_NOOP("MemberModeration", "Don't Delete Any") }, { 3600, QT_TRANSLATE_NOOP("MemberModeration", "Previous Hour") }, { 21600, QT_TRANSLATE_NOOP("MemberModeration", "Previous 6 Hours") }, { 43200, QT_TRANSLATE_NOOP("MemberModeration", "Previous 12 Hours") }, { 86400, QT_TRANSLATE_NOOP("MemberModeration", "Previous 24 Hours") }, { 259200, QT_TRANSLATE_NOOP("MemberModeration", "Previous 3 Days") }, { 604800, QT_TRANSLATE_NOOP("MemberModeration", "Previous 7 Days") }
};
constexpr Choice timeoutChoices[] = {
    { 60, QT_TRANSLATE_NOOP("MemberModeration", "60 secs") }, { 300, QT_TRANSLATE_NOOP("MemberModeration", "5 mins") }, { 600, QT_TRANSLATE_NOOP("MemberModeration", "10 mins") }, { 3600, QT_TRANSLATE_NOOP("MemberModeration", "1 hour") }, { 86400, QT_TRANSLATE_NOOP("MemberModeration", "1 day") }, { 604800, QT_TRANSLATE_NOOP("MemberModeration", "1 week") }
};
const char *const banReasons[] = { QT_TRANSLATE_NOOP("MemberModeration", "Suspicious or spam account"), QT_TRANSLATE_NOOP("MemberModeration", "Compromised or hacked account"), QT_TRANSLATE_NOOP("MemberModeration", "Breaking server rules") };
QHash<Shell *, Controller::Impl *> controllers;
QString esc(const QString &s)
{
    QString result = s.toHtmlEscaped();
    return result.replace(QLatin1Char('\''), QStringLiteral("&#39;"));
}
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

QString timeoutRemaining(const QDateTime &until)
{
    const qint64 remaining = qMax<qint64>(1, QDateTime::currentDateTimeUtc().secsTo(until));
    const QString clock = tr("%1h %2m %3s")
                                  .arg((remaining % 86400) / 3600, 2, 10, QLatin1Char('0'))
                                  .arg((remaining % 3600) / 60, 2, 10, QLatin1Char('0'))
                                  .arg(remaining % 60, 2, 10, QLatin1Char('0'));
    const qint64 days = remaining / 86400;
    return days > 0 ? tr("%1d").arg(days, 2, 10, QLatin1Char('0')) + QLatin1Char(' ') + clock : clock;
}

struct Controller::Impl : QObject, ::Rml::EventListener
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
        QString name;
        QString color;
        bool assignable;
    };
    Shell &shell;
    ::Rml::ElementDocument *doc = nullptr;
    QPointer<Core::ClientInstance> instance;
    MemberModerationTarget target;
    Mode mode = Mode::Kick;
    QList<Role> roles;
    QList<Core::Snowflake> roleIds;
    QString currentNick;
    QString filter;
    bool busy = false;
    quint64 generation = 0;
    QTimer ticker;
    QMetaObject::Connection accountConnection;

    explicit Impl(Shell &s) : shell(s)
    {
        controllers.insert(&shell, this);
        QObject::connect(&ticker, &QTimer::timeout, this, [this] {
            if (!instance || !target.isTimedOut()) {
                hide();
                return;
            }
            if (doc && mode == Mode::RemoveTimeout) {
                auto *remaining = doc->GetElementById("moderation-remaining");
                if (remaining)
                    remaining->SetInnerRML(esc(tr("%1 has %2 remaining in timeout.").arg(target.username, timeoutRemaining(target.timeoutUntil))).toStdString());
                repaint();
            }
        });
    }
    ~Impl() override
    {
        if (controllers.value(&shell) == this)
            controllers.remove(&shell);
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
        QObject::disconnect(accountConnection);
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
    void text(const char *id, const QString &value)
    {
        if (auto *e = element(id))
            e->SetInnerRML(esc(value).toStdString());
    }
    QString input(const char *id) const
    {
        auto *e = element(id);
        if (auto *v = dynamic_cast<::Rml::ElementFormControlInput *>(e))
            return QString::fromStdString(v->GetValue());
        if (auto *v = dynamic_cast<::Rml::ElementFormControlTextArea *>(e))
            return QString::fromStdString(v->GetValue());
        if (auto *v = dynamic_cast<::Rml::ElementFormControlSelect *>(e))
            return QString::fromStdString(v->GetValue());
        return {};
    }
    void show(Mode next, Core::ClientInstance *inst, const MemberModerationTarget &member)
    {
        if (!inst || (next == Mode::RemoveTimeout && !member.isTimedOut()) || !ensure())
            return;
        QObject::disconnect(accountConnection);
        ++generation;
        ticker.stop();
        busy = false;
        instance = inst;
        target = member;
        mode = next;
        filter.clear();
        roles.clear();
        roleIds = member.roleIds;
        accountConnection = QObject::connect(inst, &QObject::destroyed, this, [this] { hide(); });
        QString title, body, confirm;
        switch (mode) {
        case Mode::Kick:
            title = tr("Kick %1 from Server").arg(target.username);
            confirm = tr("Kick");
            body = QStringLiteral("<p>%1</p><div class='moderation-label'>%2</div><textarea id='moderation-reason' maxlength='512'></textarea>")
                           .arg(esc(tr("Are you sure you want to kick %1 from the server? They will be able to rejoin again with a new invite.").arg(target.username)), esc(tr("Reason for Kick")));
            break;
        case Mode::Ban:
            title = tr("Ban %1?").arg(target.username);
            confirm = tr("Ban");
            body = QStringLiteral("<div class='moderation-label'>%1</div><select id='moderation-ban-reason'><option value='-1'></option>").arg(esc(tr("Reason for Ban")));
            for (int i = 0; i < 3; ++i)
                body += QStringLiteral("<option value='%1'>%2</option>").arg(i).arg(esc(tr(banReasons[i])));
            body += QStringLiteral("<option value='3'>%1</option></select><textarea id='moderation-reason' maxlength='512' style='display:none'></textarea><div class='moderation-label'>%2</div><select id='moderation-history'>").arg(esc(tr("Other")), esc(tr("Delete Message History")));
            for (const auto &c : historyChoices)
                body += QStringLiteral("<option value='%1'%2>%3</option>").arg(c.seconds).arg(c.seconds == 3600 ? QStringLiteral(" selected") : QString(), esc(tr(c.label)));
            body += QStringLiteral("</select>");
            break;
        case Mode::Timeout:
            title = tr("Timeout %1").arg(target.username);
            confirm = tr("Timeout");
            body = QStringLiteral("<p>%1</p><div class='moderation-label'>%2</div><select id='moderation-duration'>").arg(esc(tr("Members who are in timeout are temporarily not allowed to chat or react in text channels. They are also not allowed to connect to voice or Stage channels.")), esc(tr("Duration")));
            for (const auto &c : timeoutChoices)
                body += QStringLiteral("<option value='%1'>%2</option>").arg(c.seconds).arg(esc(tr(c.label)));
            body += QStringLiteral("</select><div class='moderation-label'>%1</div><p>%2</p><textarea id='moderation-reason' maxlength='512'></textarea>").arg(esc(tr("Reason")), esc(tr("Enter a reason. This will only be visible in the Audit Log and will not be shown to the member.")));
            break;
        case Mode::RemoveTimeout:
            title = confirm = tr("Remove Timeout");
            body = QStringLiteral("<p id='moderation-remaining'>%1</p><p>%2</p>").arg(esc(tr("%1 has %2 remaining in timeout.").arg(target.username, timeoutRemaining(target.timeoutUntil))), esc(tr("Remove it now to let them post and react to messages, and join voice and stage channels.")));
            ticker.start(1000);
            break;
        case Mode::Nickname: {
            title = tr("Change Nickname");
            confirm = tr("Save");
            const auto memberData = inst->users()->getMember(target.guildId, target.userId);
            currentNick = memberData ? memberData->nick.valueOr(QString()) : QString();
            body = QStringLiteral("<p>%1</p><div class='moderation-label'>%2</div><input id='moderation-nick' maxlength='32' value='%3'/><button data-action='reset' class='moderation-reset'>%4</button>")
                           .arg(esc(tr("Nicknames are visible to everyone on this server. Do not change them unless you are enforcing a naming system or clearing a bad nickname.")), esc(tr("Nickname for %1").arg(target.username)), esc(currentNick), esc(tr("Reset Nickname")));
            break;
        }
        case Mode::Roles: {
            title = tr("Roles");
            confirm = tr("Done");
            const auto hierarchy = inst->selfRoleHierarchy(target.guildId);
            if (!hierarchy)
                return;
            for (const Discord::Role &role : hierarchy->sortedRoles()) {
                const bool assignable = !role.isManaged() && hierarchy->outranksRole(role);
                if (role.id.get() != target.guildId && (assignable || target.roleIds.contains(role.id.get())))
                    roles.append({ role.id.get(), role.name.get(), role.hasColor() ? role.getColor().name() : QString(), assignable });
            }
            body = QStringLiteral("<div class='moderation-label'>%1</div><input id='moderation-filter'/><div id='moderation-roles'></div>").arg(esc(tr("Search roles")));
            break;
        }
        }
        text("moderation-title", title);
        text("moderation-confirm", confirm);
        text("moderation-error", {});
        if (auto *e = element("moderation-confirm"))
            e->RemoveAttribute("disabled");
        if (auto *e = element("moderation-body"))
            e->SetInnerRML(body.toStdString());
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
        QString html;
        for (int i = 0; i < roles.size(); ++i) {
            const auto &role = roles[i];
            if (!role.name.contains(filter.trimmed(), Qt::CaseInsensitive))
                continue;
            html += QStringLiteral("<button class='moderation-role%1' data-role='%2'%3><span class='moderation-check'>%4</span><span class='moderation-dot' style='background-color:%5'></span>%6</button>")
                            .arg(!role.assignable || busy ? QStringLiteral(" disabled") : QString())
                            .arg(i)
                            .arg(!role.assignable || busy ? QStringLiteral(" disabled='true'") : QString())
                            .arg(roleIds.contains(role.id) ? QStringLiteral("&#10003;") : QString(), role.color.isEmpty() ? QStringLiteral("#949ba4") : role.color, esc(role.name));
        }
        if (html.isEmpty())
            html = esc(roles.isEmpty() ? tr("No Roles") : tr("No matching roles"));
        if (auto *e = element("moderation-roles"))
            e->SetInnerRML(html.toStdString());
        repaint();
    }
    void roleClick(int index)
    {
        if (!instance || busy || index < 0 || index >= roles.size() || !roles[index].assignable)
            return;
        const auto id = roles[index].id;
        const bool checked = !roleIds.contains(id);
        auto next = roleIds;
        next.removeAll(id);
        if (checked)
            next.append(id);
        const QList<Core::Snowflake> toggled{ id }, none;
        busy = true;
        text("moderation-error", {});
        renderRoles();
        const auto expected = generation;
        QPointer<Impl> self(this);
        QPointer<Core::ClientInstance> guard(instance);
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
        const QString reason = input("moderation-reason").left(512);
        const auto expected = generation;
        QPointer<Impl> self(this);
        QPointer<Core::ClientInstance> guard(instance);
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
            int selected = input("moderation-ban-reason").toInt();
            QString banReason = selected == 3 ? (reason.trimmed().isEmpty() ? QStringLiteral("other") : reason) : (selected >= 0 && selected < 3 ? tr(banReasons[selected]) : QString());
            instance->discord()->banMember(target.guildId, target.userId, input("moderation-history").toInt(), banReason, done);
            break;
        }
        case Mode::Timeout:
            instance->discord()->setMemberTimeout(target.guildId, target.userId, QDateTime::currentDateTimeUtc().addSecs(input("moderation-duration").toInt()), reason, done);
            break;
        case Mode::RemoveTimeout:
            instance->discord()->setMemberTimeout(target.guildId, target.userId, QDateTime(), std::nullopt, done);
            break;
        case Mode::Nickname: {
            const QString nick = input("moderation-nick").left(32).trimmed();
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
                    const QString value = QString::fromStdString(area->GetValue());
                    if (value.size() > 512)
                        area->SetValue(value.left(512).toStdString());
                }
            }
            if (e->GetId() == "moderation-filter") {
                const QString next = input("moderation-filter");
                if (next != filter) {
                    filter = next;
                    renderRoles();
                }
            }
            if (e->GetId() == "moderation-ban-reason")
                if (auto *other = element("moderation-reason")) {
                    other->SetProperty("display", input("moderation-ban-reason") == QStringLiteral("3") ? "block" : "none");
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
                roleClick(QString::fromStdString(p->GetAttribute<::Rml::String>("data-role", "-1")).toInt());
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
    if (auto *controller = controllers.value(&shell, nullptr))
        controller->show(mode, instance, target);
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
    QPointer<Core::ClientInstance> guard(instance);
    auto add = [&](const QString &label, auto fn, bool danger = false) {
        items.push_back({ label, [guard, &shell, target, fn] { if (guard) fn(shell, guard, target); }, true, danger });
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
            add(tr("Remove Timeout From %1").arg(target.username), &removeTimeout);
        else
            add(tr("Timeout %1").arg(target.username), &timeout);
    }
    if (k)
        add(tr("Kick %1").arg(target.username), &kick, true);
    if (b)
        add(tr("Ban %1").arg(target.username), &ban, true);
}
} // namespace Styx::UI::MemberModeration
