#include "GuildSettingsController.hpp"

#include "Shell.hpp"
#include "RmlView.hpp"
#include "RmlRuntime.hpp"
#include "ImageUpload.hpp"
#include "ConfirmModal.hpp"

#include "Core/ClientInstance.hpp"
#include "Core/Session.hpp"
#include "Core/UserManager.hpp"
#include "Core/PermissionManager.hpp"
#include "Discord/CdnUrls.hpp"
#include "Discord/Client.hpp"

#include <QFileDialog>
#include <QFileInfo>
#include <QDateTime>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>

namespace Styx {
namespace UI {
namespace Rml {

namespace {


struct BrandColorPreset {
    const char *name;
    const char *color;
};
constexpr BrandColorPreset BrandColors[] = {
    { "Soul", "#ff1c90" },
    { "Volcano", "#e81d1e" },
    { "Marsh", "#e86e1d" },
    { "Thunder", "#e8c02f" },
    { "Rising", "#71368a" },
    { "Zephyr", "#029FFC" },
    { "Cascade", "#4fe2ca" },
    { "Earth", "#406601" },
    { "Boulder", "#272727" },
};

struct PermissionDef {
    Discord::Permission perm;
    const char *name;
    const char *desc;
    const char *category;
};
const PermissionDef AllPermissions[] = {
    { Discord::Permission::VIEW_CHANNEL, "View Channels", "Allows members to view channels by default.", "General" },
    { Discord::Permission::MANAGE_CHANNELS, "Manage Channels", "Allows members to create, edit, or delete channels.", "General" },
    { Discord::Permission::MANAGE_ROLES, "Manage Roles", "Allows members to create, edit, and delete roles lower than this one.", "General" },
    { Discord::Permission::MANAGE_EXPRESSIONS, "Manage Expressions", "Allows members to manage custom emojis and stickers.", "General" },
    { Discord::Permission::VIEW_AUDIT_LOG, "View Audit Log", "Allows members to view the server audit log.", "General" },
    { Discord::Permission::MANAGE_GUILD, "Manage Server", "Allows members to change server name, region, and settings.", "General" },
    { Discord::Permission::CREATE_INSTANT_INVITE, "Create Invite", "Allows members to invite new people to this server.", "Membership" },
    { Discord::Permission::CHANGE_NICKNAME, "Change Nickname", "Allows members to change their own nickname.", "Membership" },
    { Discord::Permission::MANAGE_NICKNAMES, "Manage Nicknames", "Allows members to change other members' nicknames.", "Membership" },
    { Discord::Permission::KICK_MEMBERS, "Kick Members", "Allows members to kick members lower than them.", "Membership" },
    { Discord::Permission::BAN_MEMBERS, "Ban Members", "Allows members to permanently ban members lower than them.", "Membership" },
    { Discord::Permission::MODERATE_MEMBERS, "Timeout Members", "Allows members to timeout other members lower than them.", "Membership" },
    { Discord::Permission::SEND_MESSAGES, "Send Messages", "Allows members to send messages in text channels.", "Text" },
    { Discord::Permission::SEND_MESSAGES_IN_THREADS, "Send Messages in Threads", "Allows members to send messages in threads.", "Text" },
    { Discord::Permission::CREATE_PUBLIC_THREADS, "Create Public Threads", "Allows members to create public threads.", "Text" },
    { Discord::Permission::CREATE_PRIVATE_THREADS, "Create Private Threads", "Allows members to create private threads.", "Text" },
    { Discord::Permission::EMBED_LINKS, "Embed Links", "Allows links sent by members to show rich embeds.", "Text" },
    { Discord::Permission::ATTACH_FILES, "Attach Files", "Allows members to upload files and media.", "Text" },
    { Discord::Permission::ADD_REACTIONS, "Add Reactions", "Allows members to add new emoji reactions.", "Text" },
    { Discord::Permission::USE_EXTERNAL_EMOJIS, "Use External Emojis", "Allows members to use emojis from other servers.", "Text" },
    { Discord::Permission::USE_EXTERNAL_STICKERS, "Use External Stickers", "Allows members to use stickers from other servers.", "Text" },
    { Discord::Permission::MENTION_EVERYONE, "Mention @everyone, @here, and All Roles", "Allows members to ping @everyone or @here.", "Text" },
    { Discord::Permission::MANAGE_MESSAGES, "Manage Messages", "Allows members to delete messages by other members or pin messages.", "Text" },
    { Discord::Permission::READ_MESSAGE_HISTORY, "Read Message History", "Allows members to read past messages in channels.", "Text" },
    { Discord::Permission::CONNECT, "Connect", "Allows members to join voice channels.", "Voice" },
    { Discord::Permission::SPEAK, "Speak", "Allows members to speak in voice channels.", "Voice" },
    { Discord::Permission::STREAM, "Video", "Allows members to share screen and stream video.", "Voice" },
    { Discord::Permission::USE_VAD, "Use Voice Activity", "Allows members to speak without Push-to-Talk.", "Voice" },
    { Discord::Permission::PRIORITY_SPEAKER, "Priority Speaker", "Allows members to be heard more easily.", "Voice" },
    { Discord::Permission::MUTE_MEMBERS, "Mute Members", "Allows members to mute other members in voice.", "Voice" },
    { Discord::Permission::DEAFEN_MEMBERS, "Deafen Members", "Allows members to deafen other members in voice.", "Voice" },
    { Discord::Permission::MOVE_MEMBERS, "Move Members", "Allows members to move other members between voice channels.", "Voice" },
    { Discord::Permission::ADMINISTRATOR, "Administrator", "Members with this permission have every permission and bypass channel overwrites.", "Advanced" }
};

} // namespace

GuildSettingsController::GuildSettingsController(Shell &shell, QObject *parent)
    : Controller(shell, nullptr, parent)
{
}

GuildSettingsController::~GuildSettingsController()
{
    if (document) {
        document->Close();
        document = nullptr;
    }
}

void GuildSettingsController::ensureDocument()
{
    if (document || !shell.view() || !shell.view()->context())
        return;

    document = shell.view()->context()->LoadDocument(":/ui/guild_settings.rml");
    if (!document)
        return;

    regionRoot = document;
    setupEventListeners();
}

bool GuildSettingsController::canEditCurrentSection() const
{
    return GuildSettingsAccess::canEdit(instance(), currentGuildId, currentSection);
}

bool GuildSettingsController::isCurrentRequest(Core::Snowflake guildId, quint64 generation) const
{
    return visible && currentGuildId == guildId && requestGeneration == generation;
}

void GuildSettingsController::resetGuildState()
{
    savedProfile = {};
    draftProfile = {};
    savedGuildProfile = {};
    savedEngagement = {};
    draftEngagement = {};
    savedBoost = {};
    draftBoost = {};
    emojis.clear();
    stickers.clear();
    members.clear();
    roles.clear();
    invites.clear();
    bans.clear();
    savedRoleDrafts.clear();
    dirtyRoleDrafts.clear();
    selectedRoleId = Core::Snowflake::Invalid;
    memberSearchQuery.clear();
    permissionSearchQuery.clear();
    banSearchQuery.clear();
    auditFormatter.reset();
    auditRows.clear();
    expandedAuditRows.clear();
    auditFilterUserId = Core::Snowflake::Invalid;
    auditFilterAction = -1;
    profileLoaded = engagementLoaded = boostLoaded = emojisLoaded = stickersLoaded = membersLoaded = rolesLoaded = invitesLoaded = auditLogLoaded = bansLoaded = false;
    errorMessage.clear();
}

void GuildSettingsController::reportError(const QString &message)
{
    errorMessage = message;
    if (document) {
        if (auto *text = document->GetElementById("guild-unsaved-changes-text"))
            text->SetInnerRML(escape(message).toUtf8().constData());
        if (auto *bar = document->GetElementById("guild-unsaved-changes-bar")) {
            bar->SetClass("visible", true);
            bar->SetClass("flash", true);
        }
    }
    requestRender(100);
}
Core::ClientInstance *GuildSettingsController::instance() const
{
    return shell.instanceFor(currentAccountId);
}

std::optional<Discord::Guild> GuildSettingsController::guild() const
{
    auto *inst = instance();
    return inst ? inst->getGuild(currentGuildId) : std::nullopt;
}

QString GuildSettingsController::getGuildIconUrl() const
{
    if (!draftProfile.iconDataUri.isEmpty())
        return draftProfile.iconDataUri;
    if (draftProfile.iconRemoved)
        return QString();

    auto g = guild();
    if (g && g->icon.hasValue() && !g->icon->isEmpty()) {
        QUrl url = Discord::Cdn::guildIcon(currentGuildId, g->icon.get(), 128);
        return shell.imageSource(url, QSize(96, 96));
    }
    return QString();
}

void GuildSettingsController::open(Core::Snowflake accountId, Core::Snowflake guildId, GuildSettingsSection section)
{
    const bool guildChanged = currentAccountId != accountId || currentGuildId != guildId;
    ++requestGeneration;
    currentAccountId = accountId;
    currentGuildId = guildId;
    currentSection = section;
    if (guildChanged)
        resetGuildState();

    ensureDocument();
    if (!document)
        return;

    visible = true;
    loadCurrentSectionData();

    render();
    document->Show(::Rml::ModalFlag::Modal);
    document->PullToFront();
    requestRender(200);
}

void GuildSettingsController::close()
{
    if (hasUnsavedChanges()) {
        showConfirmModal(
            shell, tr("Discard unsaved changes?"),
            tr("You have unsaved changes, are you sure you want to discard them?"),
            tr("Discard"), [this]() { discardChanges(); close(); }, nullptr, true);
        return;
    }

    ++requestGeneration;
    if (document)
        document->Hide();
    visible = false;
    requestRender(100);
}

void GuildSettingsController::setSection(GuildSettingsSection section)
{
    if (currentSection == section)
        return;

    if (hasUnsavedChanges()) {
        showConfirmModal(
            shell,
            tr("Discard unsaved changes?"),
            tr("You have unsaved changes, are you sure you want to discard them?"),
            tr("Discard"),
            [this, section]() {
                discardChanges();
                currentSection = section;
                loadCurrentSectionData();
                render();
            },
            nullptr,
            /*danger=*/true
        );
        return;
    }

    currentSection = section;
    loadCurrentSectionData();
    render();
}

void GuildSettingsController::render()
{
    if (!document)
        return;

    renderSidebar();
    renderContent();
    updateUnsavedChangesBar();
    requestRender(100);
}

void GuildSettingsController::renderSidebar()
{
    auto *sidebar = document->GetElementById("guild-settings-sidebar");
    if (!sidebar)
        return;

    QString rml;
    auto g = guild();
    QString guildName = g ? g->name.valueOr("Server") : "Server";

    rml += QStringLiteral("<div class=\"settings-group-header\" style=\"color:#ffffff; font-size:14dp; margin-bottom:12dp;\">%1</div>")
               .arg(escape(guildName));

    auto sections = GuildSettingsAccess::visibleSections(instance(), currentGuildId);
    GuildSettingsGroup lastGroup = static_cast<GuildSettingsGroup>(-1);

    for (GuildSettingsSection sec : sections) {
        GuildSettingsGroup grp = GuildSettingsAccess::group(sec);
        if (grp != lastGroup) {
            lastGroup = grp;
            QString grpTitle = GuildSettingsAccess::groupTitle(grp);
            if (!grpTitle.isEmpty()) {
                rml += QStringLiteral("<div class=\"settings-group-header\">%1</div>").arg(escape(grpTitle));
            } else if (sec == GuildSettingsSection::Profile) {
                rml += QStringLiteral("<div class=\"settings-group-header\">Server Settings</div>");
            } else if (grp == GuildSettingsGroup::Danger) {
                rml += QStringLiteral("<div class=\"settings-divider\"></div>");
            }
        }

        QString itemClass = QStringLiteral("settings-nav-item");
        if (currentSection == sec)
            itemClass += QStringLiteral(" active");
        if (sec == GuildSettingsSection::DeleteServer)
            itemClass += QStringLiteral(" settings-btn-danger");

        rml += QStringLiteral("<div class=\"%1\" data-action=\"nav-section\" data-section=\"%2\">%3</div>")
                   .arg(itemClass, QString::number(static_cast<int>(sec)), escape(GuildSettingsAccess::title(sec)));
    }

    sidebar->SetInnerRML(rml.toUtf8().constData());
}

void GuildSettingsController::renderContent()
{
    auto *content = document->GetElementById("guild-settings-content-wrapper");
    if (!content)
        return;

    QString rml;
    switch (currentSection) {
    case GuildSettingsSection::Profile:
        renderProfile(rml);
        break;
    case GuildSettingsSection::Engagement:
        renderEngagement(rml);
        break;
    case GuildSettingsSection::BoostPerks:
        renderBoostPerks(rml);
        break;
    case GuildSettingsSection::Emoji:
        renderEmoji(rml);
        break;
    case GuildSettingsSection::Stickers:
        renderStickers(rml);
        break;
    case GuildSettingsSection::Members:
        renderMembers(rml);
        break;
    case GuildSettingsSection::Roles:
        renderRoles(rml);
        break;
    case GuildSettingsSection::Invites:
        renderInvites(rml);
        break;
    case GuildSettingsSection::AuditLog:
        renderAuditLog(rml);
        break;
    case GuildSettingsSection::Bans:
        renderBans(rml);
        break;
    case GuildSettingsSection::DeleteServer:
        renderDeleteServer(rml);
        break;
    }

    content->SetInnerRML(rml.toUtf8().constData());
}

// 1. Overview / Profile
void GuildSettingsController::renderProfile(QString &rml)
{
    rml += QStringLiteral("<div class=\"settings-section-title\">Server Overview & Profile</div>");

    // Icon Row
    rml += QStringLiteral("<div class=\"guild-icon-upload-row\">");
    QString iconSrc = getGuildIconUrl();
    rml += QStringLiteral("<div class=\"guild-icon-preview\">");
    if (!iconSrc.isEmpty()) {
        rml += QStringLiteral("<img src=\"%1\"/>").arg(escape(iconSrc));
    } else {
        auto g = guild();
        QString initials = g ? g->name.valueOr("?").left(2).toUpper() : "?";
        rml += QStringLiteral("<div class=\"guild-icon-initials\">%1</div>").arg(escape(initials));
    }
    rml += QStringLiteral("</div>");

    rml += QStringLiteral("<div class=\"guild-icon-actions\">");
    rml += QStringLiteral("<div class=\"settings-btn settings-btn-primary\" data-action=\"choose-icon\">Change Icon</div>");
    if (!iconSrc.isEmpty())
        rml += QStringLiteral("<div class=\"settings-btn settings-btn-danger\" data-action=\"remove-icon\">Remove Icon</div>");
    rml += QStringLiteral("<div class=\"settings-note\" style=\"color:#949ba4; font-style:normal;\">Minimum Size: 128x128</div>");
    rml += QStringLiteral("</div>");
    rml += QStringLiteral("</div>");

    // Server Name
    rml += QStringLiteral("<div class=\"settings-row\">");
    rml += QStringLiteral("<div class=\"settings-label\">Server Name</div>");
    rml += QStringLiteral("<input type=\"text\" class=\"settings-text-input\" id=\"profile-name-input\" value=\"%1\" maxlength=\"100\"/>")
               .arg(escape(draftProfile.name));
    rml += QStringLiteral("</div>");

    // Server Description
    rml += QStringLiteral("<div class=\"settings-row\">");
    rml += QStringLiteral("<div class=\"settings-label\">Server Description</div>");
    rml += QStringLiteral("<div class=\"settings-description\">Tell the world what your server is about.</div>");
    rml += QStringLiteral("<textarea class=\"settings-textarea\" id=\"profile-desc-input\">%1</textarea>")
               .arg(escape(draftProfile.description));
    rml += QStringLiteral("</div>");

    // Server Banner Brand Color
    rml += QStringLiteral("<div class=\"settings-row\">");
    rml += QStringLiteral("<div class=\"settings-label\">Server Brand Color</div>");
    rml += QStringLiteral("<div class=\"brand-colors-row\">");
    for (const auto &preset : BrandColors) {
        QString activeClass = (draftProfile.brandColor.compare(QLatin1String(preset.color), Qt::CaseInsensitive) == 0)
                                  ? QStringLiteral(" active") : QString();
        rml += QStringLiteral("<div class=\"brand-color-swatch%1\" style=\"background-color:%2;\" data-action=\"select-brand-color\" data-color=\"%2\" title=\"%3\"></div>")
                   .arg(activeClass, QLatin1String(preset.color), QLatin1String(preset.name));
    }
    rml += QStringLiteral("</div>");
    rml += QStringLiteral("</div>");

    // Server Traits
    rml += QStringLiteral("<div class=\"settings-row\">");
    rml += QStringLiteral("<div class=\"settings-label\">Server Traits</div>");
    rml += QStringLiteral("<div class=\"settings-description\">Add up to 6 traits that define your community.</div>");
    rml += QStringLiteral("<div class=\"traits-grid\">");
    for (int i = 0; i < 6; ++i) {
        rml += QStringLiteral("<div class=\"trait-input-col\">");
        rml += QStringLiteral("<input type=\"text\" class=\"settings-text-input\" id=\"trait-input-%1\" data-trait-idx=\"%1\" value=\"%2\" maxlength=\"24\" placeholder=\"Trait %3\"/>")
                   .arg(QString::number(i), escape(draftProfile.traits[i]), QString::number(i + 1));
        rml += QStringLiteral("</div>");
    }
    rml += QStringLiteral("</div>");
    rml += QStringLiteral("</div>");

    // Private Server Switch
    rml += QStringLiteral("<div class=\"settings-switch-row\">");
    rml += QStringLiteral("<div class=\"settings-switch-text\">");
    rml += QStringLiteral("<div class=\"settings-switch-title\">Private Server</div>");
    rml += QStringLiteral("<div class=\"settings-switch-desc\">Only members can view profile content and server details.</div>");
    rml += QStringLiteral("</div>");
    QString switchClass = draftProfile.isPrivate ? QStringLiteral(" checked") : QString();
    rml += QStringLiteral("<div class=\"settings-switch%1\" data-action=\"toggle-private\"><div class=\"settings-switch-thumb\"></div></div>")
               .arg(switchClass);
    rml += QStringLiteral("</div>");
}

// 2. Engagement
void GuildSettingsController::renderEngagement(QString &rml)
{
    rml += QStringLiteral("<div class=\"settings-section-title\">Engagement & System Messages</div>");

    auto *inst = instance();
    QList<Discord::Channel> channels;
    if (inst)
        channels = inst->getGuildChannels(currentGuildId);
    // System Messages Channel
    rml += QStringLiteral("<div class=\"settings-row\">");
    rml += QStringLiteral("<div class=\"settings-label\">System Messages Channel</div>");
    rml += QStringLiteral("<div class=\"settings-description\">Channel where welcome messages and server boost announcements are posted.</div>");
    rml += QStringLiteral("<select class=\"settings-select\" id=\"select-system-channel\">");
    rml += QStringLiteral("<option value=\"0\"%1>No System Messages</option>")
               .arg(!draftEngagement.systemChannelId.isValid() ? QStringLiteral(" selected") : QString());
    for (const auto &ch : channels) {
        if (ch.type.valueOr(Discord::ChannelType::GUILD_TEXT) == Discord::ChannelType::GUILD_TEXT) {
            QString sel = (ch.id.get() == draftEngagement.systemChannelId) ? QStringLiteral(" selected") : QString();
            rml += QStringLiteral("<option value=\"%1\"%2># %3</option>")
                       .arg(QString::number(quint64(ch.id.get())), sel, escape(ch.name.valueOr("channel")));
        }
    }
    rml += QStringLiteral("</select>");
    rml += QStringLiteral("</div>");

    // System Flags
    rml += QStringLiteral("<div class=\"settings-subheading\">System Message Notifications</div>");

    auto addFlag = [&](const QString &title, const QString &desc, bool checked, const QString &action) {
        rml += QStringLiteral("<div class=\"settings-switch-row\">");
        rml += QStringLiteral("<div class=\"settings-switch-text\">");
        rml += QStringLiteral("<div class=\"settings-switch-title\">%1</div>").arg(escape(title));
        rml += QStringLiteral("<div class=\"settings-switch-desc\">%1</div>").arg(escape(desc));
        rml += QStringLiteral("</div>");
        QString chk = checked ? QStringLiteral(" checked") : QString();
        rml += QStringLiteral("<div class=\"settings-switch%1\" data-action=\"%2\"><div class=\"settings-switch-thumb\"></div></div>")
                   .arg(chk, action);
        rml += QStringLiteral("</div>");
    };

    addFlag("Suppress member join notifications", "Don't send a message when someone joins this server.", draftEngagement.suppressJoin, "toggle-flag-join");
    addFlag("Suppress premium subscription notifications", "Don't send a message when someone boosts this server.", draftEngagement.suppressBoost, "toggle-flag-boost");
    addFlag("Suppress server setup tips", "Don't send helpful server setup tips to new members.", draftEngagement.suppressTips, "toggle-flag-tips");
    addFlag("Suppress member prompt reply notifications", "Don't notify members when someone replies to member prompts.", draftEngagement.suppressPrompt, "toggle-flag-prompt");

    // Default Notifications
    rml += QStringLiteral("<div class=\"settings-subheading\">Default Notification Settings</div>");
    rml += QStringLiteral("<div class=\"settings-description\">Determines whether members receive notifications for all messages or only @mentions by default.</div>");
    rml += QStringLiteral("<div class=\"settings-radio-group\">");

    bool allMsgs = (draftEngagement.defaultNotifications == Discord::MessageNotificationLevel::ALL_MESSAGES);
    rml += QStringLiteral("<div class=\"settings-radio-card%1\" data-action=\"select-notif-all\">")
               .arg(allMsgs ? QStringLiteral(" active") : QString());
    rml += QStringLiteral("<div class=\"settings-radio-indicator\"></div>");
    rml += QStringLiteral("<div class=\"settings-radio-info\"><div class=\"settings-radio-title\">All Messages</div><div class=\"settings-radio-desc\">Recommended for small communities and private servers.</div></div>");
    rml += QStringLiteral("</div>");

    rml += QStringLiteral("<div class=\"settings-radio-card%1\" data-action=\"select-notif-mentions\">")
               .arg(!allMsgs ? QStringLiteral(" active") : QString());
    rml += QStringLiteral("<div class=\"settings-radio-indicator\"></div>");
    rml += QStringLiteral("<div class=\"settings-radio-info\"><div class=\"settings-radio-title\">Only @mentions</div><div class=\"settings-radio-desc\">Recommended for public or larger servers.</div></div>");
    rml += QStringLiteral("</div>");
    rml += QStringLiteral("</div>");

    // AFK Channel & Timeout
    rml += QStringLiteral("<div class=\"settings-subheading\">Inactive (AFK) Settings</div>");
    rml += QStringLiteral("<div class=\"settings-grid-2col\">");
    rml += QStringLiteral("<div class=\"settings-grid-col settings-grid-col-left\">");
    rml += QStringLiteral("<div class=\"settings-label\">Inactive Channel</div>");
    rml += QStringLiteral("<select class=\"settings-select\" id=\"select-afk-channel\">");
    rml += QStringLiteral("<option value=\"0\"%1>No Inactive Channel</option>")
               .arg(!draftEngagement.afkChannelId.isValid() ? QStringLiteral(" selected") : QString());
    for (const auto &ch : channels) {
        if (ch.type.valueOr(Discord::ChannelType::GUILD_VOICE) == Discord::ChannelType::GUILD_VOICE) {
            QString sel = (ch.id.get() == draftEngagement.afkChannelId) ? QStringLiteral(" selected") : QString();
            rml += QStringLiteral("<option value=\"%1\"%2>%3</option>")
                       .arg(QString::number(quint64(ch.id.get())), sel, escape(ch.name.valueOr("voice")));
        }
    }
    rml += QStringLiteral("</select>");
    rml += QStringLiteral("</div>");

    rml += QStringLiteral("<div class=\"settings-grid-col settings-grid-col-right\">");
    rml += QStringLiteral("<div class=\"settings-label\">Inactive Timeout</div>");
    rml += QStringLiteral("<select class=\"settings-select\" id=\"select-afk-timeout\">");
    const int timeouts[] = { 60, 300, 900, 1800, 3600 };
    const char *timeoutLabels[] = { "1 minute", "5 minutes", "15 minutes", "30 minutes", "1 hour" };
    for (int t = 0; t < 5; ++t) {
        QString sel = (draftEngagement.afkTimeout == timeouts[t]) ? QStringLiteral(" selected") : QString();
        rml += QStringLiteral("<option value=\"%1\"%2>%3</option>").arg(QString::number(timeouts[t]), sel, timeoutLabels[t]);
    }
    rml += QStringLiteral("</select>");
    rml += QStringLiteral("</div>");
    rml += QStringLiteral("</div>");
}

// 3. Boost Perks
void GuildSettingsController::renderBoostPerks(QString &rml)
{
    rml += QStringLiteral("<div class=\"settings-section-title\">Server Boost Perks</div>");

    auto g = guild();
    int boostCount = g ? g->premiumSubscriptionCount.valueOr(0) : 0;
    int tier = g ? static_cast<int>(g->premiumTier.valueOr(Discord::PremiumTier::NONE)) : 0;

    rml += QStringLiteral("<div class=\"settings-card\" style=\"display:flex; flex-direction:row; align-items:center; justify-content:space-between;\">");
    rml += QStringLiteral("<div>");
    rml += QStringLiteral("<div class=\"boost-badge\">LEVEL %1</div>").arg(QString::number(tier));
    rml += QStringLiteral("<div style=\"font-size:16dp; font-weight:bold; color:#ffffff;\">%1 Server Boosts</div>").arg(QString::number(boostCount));
    rml += QStringLiteral("<div style=\"font-size:13dp; color:#949ba4;\">Unlock custom banners, vanity URLs, and extra emoji slots.</div>");
    rml += QStringLiteral("</div>");
    rml += QStringLiteral("</div>");

    // Progress Bar Toggle
    rml += QStringLiteral("<div class=\"settings-switch-row\">");
    rml += QStringLiteral("<div class=\"settings-switch-text\">");
    rml += QStringLiteral("<div class=\"settings-switch-title\">Show Boost Progress Bar</div>");
    rml += QStringLiteral("<div class=\"settings-switch-desc\">Displays current Server Boost progress at the top of your channel list.</div>");
    rml += QStringLiteral("</div>");
    QString barChk = draftBoost.progressBar ? QStringLiteral(" checked") : QString();
    rml += QStringLiteral("<div class=\"settings-switch%1\" data-action=\"toggle-progress-bar\"><div class=\"settings-switch-thumb\"></div></div>")
               .arg(barChk);
    rml += QStringLiteral("</div>");

    // Banner slot
    rml += QStringLiteral("<div class=\"settings-card\">");
    rml += QStringLiteral("<div class=\"settings-label\">Server Banner Background</div>");
    rml += QStringLiteral("<div class=\"settings-description\">Displayed at the top of your channel list. Minimum 960x540 (16:9 ratio).</div>");
    if (tier >= 2) {
        rml += QStringLiteral("<div class=\"settings-button-group\">");
        rml += QStringLiteral("<div class=\"settings-btn settings-btn-primary\" data-action=\"choose-banner\">Upload Banner</div>");
        rml += QStringLiteral("<div class=\"settings-btn settings-btn-danger\" data-action=\"remove-banner\">Remove Banner</div>");
        rml += QStringLiteral("</div>");
    } else {
        rml += QStringLiteral("<div class=\"settings-note\">Locked — Requires Level 2 Server Boosts (7 Boosts).</div>");
    }
    rml += QStringLiteral("</div>");

    // Invite Splash slot
    rml += QStringLiteral("<div class=\"settings-card\">");
    rml += QStringLiteral("<div class=\"settings-label\">Server Invite Splash</div>");
    rml += QStringLiteral("<div class=\"settings-description\">Shown when people visit your invite links in a web browser. Minimum 1920x1080.</div>");
    if (tier >= 1) {
        rml += QStringLiteral("<div class=\"settings-button-group\">");
        rml += QStringLiteral("<div class=\"settings-btn settings-btn-primary\" data-action=\"choose-splash\">Upload Splash</div>");
        rml += QStringLiteral("<div class=\"settings-btn settings-btn-danger\" data-action=\"remove-splash\">Remove Splash</div>");
        rml += QStringLiteral("</div>");
    } else {
        rml += QStringLiteral("<div class=\"settings-note\">Locked — Requires Level 1 Server Boosts (2 Boosts).</div>");
    }
    rml += QStringLiteral("</div>");
}

// 4. Emoji
void GuildSettingsController::renderEmoji(QString &rml)
{
    rml += QStringLiteral("<div class=\"settings-section-title\" style=\"display:flex; flex-direction:row; justify-content:space-between; align-items:center;\">");
    rml += QStringLiteral("<span>Emoji</span>");
    rml += QStringLiteral("<div class=\"settings-btn settings-btn-primary\" data-action=\"choose-emoji-upload\">Upload Emoji</div>");
    rml += QStringLiteral("</div>");

    int staticCount = 0;
    int animCount = 0;
    for (const auto &em : emojis) {
        if (em.animated.valueOr(false))
            animCount++;
        else
            staticCount++;
    }

    rml += QStringLiteral("<div class=\"slots-counter\">Static Emojis: %1 / 50 slots used · Animated Emojis: %2 / 50 slots used</div>")
               .arg(QString::number(staticCount), QString::number(animCount));

    if (emojis.isEmpty()) {
        rml += QStringLiteral("<div class=\"empty-state\"><div class=\"empty-state-title\">No Custom Emojis</div><div class=\"empty-state-desc\">Upload images (PNG, JPG, GIF) under 256 KB to use them anywhere on your server.</div></div>");
        return;
    }

    rml += QStringLiteral("<div class=\"guild-table\">");
    rml += QStringLiteral("<div class=\"guild-table-header\"><div class=\"col-image\">Image</div><div class=\"col-name\">Name</div><div class=\"col-uploader\">Uploaded By</div><div class=\"col-actions\">Actions</div></div>");

    for (const auto &em : emojis) {
        rml += QStringLiteral("<div class=\"guild-table-row\">");
        QUrl emUrl = Discord::Cdn::emoji(em.id.get(), 32, em.animated.valueOr(false));
        QString emSrc = shell.imageSource(emUrl, QSize(32, 32));
        rml += QStringLiteral("<div class=\"col-image\"><img class=\"emoji-preview-img\" src=\"%1\"/></div>").arg(escape(emSrc));
        rml += QStringLiteral("<div class=\"col-name\" style=\"font-weight:600; color:#f2f3f5;\">:%1:</div>").arg(escape(em.name.valueOr("emoji")));
        QString uploaderName = em.user.hasValue() ? em.user->username.valueOr("Unknown") : "Server";
        rml += QStringLiteral("<div class=\"col-uploader\">%1</div>").arg(escape(uploaderName));
        rml += QStringLiteral("<div class=\"col-actions\"><div class=\"settings-btn settings-btn-danger\" style=\"padding:4dp 8dp; font-size:12dp;\" data-action=\"delete-emoji\" data-id=\"%1\">Delete</div></div>")
                   .arg(QString::number(quint64(em.id.get())));
        rml += QStringLiteral("</div>");
    }
    rml += QStringLiteral("</div>");
}

// 5. Stickers
void GuildSettingsController::renderStickers(QString &rml)
{
    rml += QStringLiteral("<div class=\"settings-section-title\" style=\"display:flex; flex-direction:row; justify-content:space-between; align-items:center;\">");
    rml += QStringLiteral("<span>Stickers</span>");
    rml += QStringLiteral("<div class=\"settings-btn settings-btn-primary\" data-action=\"choose-sticker-upload\">Upload Sticker</div>");
    rml += QStringLiteral("</div>");

    rml += QStringLiteral("<div class=\"slots-counter\">%1 / 5 Custom Sticker slots used</div>").arg(QString::number(stickers.size()));

    if (stickers.isEmpty()) {
        rml += QStringLiteral("<div class=\"empty-state\"><div class=\"empty-state-title\">No Custom Stickers</div><div class=\"empty-state-desc\">Upload PNG, APNG, or Lottie sticker files under 512 KB to express yourself.</div></div>");
        return;
    }

    rml += QStringLiteral("<div class=\"guild-table\">");
    rml += QStringLiteral("<div class=\"guild-table-header\"><div class=\"col-image\">Sticker</div><div class=\"col-name\">Name & Related Emoji</div><div class=\"col-uploader\">Description</div><div class=\"col-actions\">Actions</div></div>");

    for (const auto &st : stickers) {
        rml += QStringLiteral("<div class=\"guild-table-row\">");
        QUrl stUrl = Discord::Cdn::stickerStill(st.id.get(), st.formatType.valueOr(Discord::StickerFormatType::PNG), 64);
        QString stSrc = shell.imageSource(stUrl, QSize(48, 48));
        rml += QStringLiteral("<div class=\"col-image\"><img class=\"sticker-preview-img\" src=\"%1\"/></div>").arg(escape(stSrc));
        rml += QStringLiteral("<div class=\"col-name\"><span style=\"font-weight:600; color:#f2f3f5;\">%1</span> <span style=\"color:#949ba4;\">(:%2:)</span></div>")
                   .arg(escape(st.name.valueOr("sticker")), escape(st.tags.valueOr("")));
        rml += QStringLiteral("<div class=\"col-uploader\">%1</div>").arg(escape(st.description.valueOr("")));
        rml += QStringLiteral("<div class=\"col-actions\"><div class=\"settings-btn settings-btn-danger\" style=\"padding:4dp 8dp; font-size:12dp;\" data-action=\"delete-sticker\" data-id=\"%1\">Delete</div></div>")
                   .arg(QString::number(quint64(st.id.get())));
        rml += QStringLiteral("</div>");
    }
    rml += QStringLiteral("</div>");
}

// 6. Members
void GuildSettingsController::renderMembers(QString &rml)
{
    rml += QStringLiteral("<div class=\"settings-section-title\" style=\"display:flex; flex-direction:row; justify-content:space-between; align-items:center;\">");
    rml += QStringLiteral("<span>Members (%1)</span>").arg(QString::number(members.size()));
    rml += QStringLiteral("<div class=\"settings-btn settings-btn-danger\" data-action=\"prune-members\">Prune Members</div>");
    rml += QStringLiteral("</div>");

    // Search bar
    rml += QStringLiteral("<div style=\"margin-bottom:16dp;\">");
    rml += QStringLiteral("<input type=\"text\" class=\"settings-text-input\" id=\"member-search-input\" value=\"%1\" placeholder=\"Search Members by username or nickname…\" style=\"width:100%;\"/>")
               .arg(escape(memberSearchQuery));
    rml += QStringLiteral("</div>");

    auto *inst = instance();

    rml += QStringLiteral("<div class=\"guild-table\">");
    rml += QStringLiteral("<div class=\"guild-table-header\"><div style=\"width:48dp;\">User</div><div class=\"col-name\">Name</div><div class=\"col-channel\">Roles</div><div class=\"col-expires\">Joined</div><div class=\"col-actions\">Actions</div></div>");

    int shown = 0;
    for (const auto &m : members) {
        if (!m.user.hasValue())
            continue;

        QString name = m.nick.hasValue() && !m.nick->isEmpty() ? m.nick.get() : m.user->username.valueOr("Member");
        QString username = m.user->username.valueOr("");

        if (!memberSearchQuery.isEmpty()) {
            if (!name.contains(memberSearchQuery, Qt::CaseInsensitive) &&
                !username.contains(memberSearchQuery, Qt::CaseInsensitive))
                continue;
        }

        shown++;
        rml += QStringLiteral("<div class=\"guild-table-row\">");

        // Avatar
        QUrl avUrl = Discord::Cdn::userAvatar(m.user->id.get(), m.user->avatar.valueOr(""), 36);
        QString avSrc = shell.imageSource(avUrl, QSize(36, 36));
        rml += QStringLiteral("<div style=\"width:48dp;\"><img class=\"member-avatar-img\" src=\"%1\"/></div>").arg(escape(avSrc));

        // Name
        rml += QStringLiteral("<div class=\"col-name\"><div class=\"member-name\">%1</div><div class=\"member-subtext\">@%2</div></div>")
                   .arg(escape(name), escape(username));

        // Roles
        rml += QStringLiteral("<div class=\"col-channel\"><div class=\"member-roles\">");
        if (inst) {
            auto roleIds = m.roles.valueOr(QList<Core::Snowflake>{});
            for (const auto &rId : roleIds) {
                for (const auto &r : roles) {
                    if (r.id.get() == rId) {
                        QString rColor = (r.color.valueOr(0) != 0) ? QStringLiteral("#%1").arg(r.color.valueOr(0), 6, 16, QLatin1Char('0')) : QStringLiteral("#949ba4");
                        rml += QStringLiteral("<div class=\"role-badge\"><div class=\"role-badge-dot\" style=\"background-color:%1;\"></div>%2</div>")
                                   .arg(rColor, escape(r.name.valueOr("role")));
                        break;
                    }
                }
            }
        }
        rml += QStringLiteral("</div></div>");

        // Joined
        QString joined = m.joinedAt.hasValue() ? m.joinedAt->toString(QStringLiteral("yyyy-MM-dd")) : QStringLiteral("—");
        rml += QStringLiteral("<div class=\"col-expires\">%1</div>").arg(joined);

        // Actions: Kick, Ban
        rml += QStringLiteral("<div class=\"col-actions\" style=\"display:flex; flex-direction:row; justify-content:flex-end;\">");
        auto target = MemberModeration::targetFor(inst, currentGuildId, m.user->id.get());
        if (target && MemberModeration::canKick(inst, *target)) {
            rml += QStringLiteral("<div class=\"settings-btn settings-btn-danger\" style=\"padding:4dp 8dp; font-size:11dp; margin-right:4dp;\" data-action=\"kick-member\" data-id=\"%1\" data-name=\"%2\">Kick</div>")
                       .arg(QString::number(quint64(m.user->id.get())), escape(username));
        }
        if (target && MemberModeration::canBan(inst, *target)) {
            rml += QStringLiteral("<div class=\"settings-btn settings-btn-danger\" style=\"padding:4dp 8dp; font-size:11dp;\" data-action=\"ban-member\" data-id=\"%1\" data-name=\"%2\">Ban</div>")
                       .arg(QString::number(quint64(m.user->id.get())), escape(username));
        }
        rml += QStringLiteral("</div>");

        rml += QStringLiteral("</div>");
        if (shown >= 100)
            break;
    }

    if (shown == 0) {
        rml += QStringLiteral("<div class=\"empty-state\"><div class=\"empty-state-title\">No Members Found</div></div>");
    }
    rml += QStringLiteral("</div>");
}

// 7. Roles
void GuildSettingsController::renderRoles(QString &rml)
{
    rml += QStringLiteral("<div class=\"settings-section-title\">Roles</div>");

    rml += QStringLiteral("<div class=\"roles-container\">");

    // Left List
    rml += QStringLiteral("<div class=\"roles-list-pane\">");
    rml += QStringLiteral("<div class=\"settings-btn settings-btn-primary\" style=\"margin-bottom:12dp; text-align:center;\" data-action=\"create-role\">+ Create Role</div>");

    if (roles.isEmpty() && instance())
        roles = instance()->getRolesForGuild(currentGuildId);

    if (!selectedRoleId.isValid() && !roles.isEmpty())
        selectedRoleId = roles.first().id.get();

    for (const auto &role : roles) {
        QString active = (role.id.get() == selectedRoleId) ? QStringLiteral(" active") : QString();
        QString color = (role.color.valueOr(0) != 0) ? QStringLiteral("#%1").arg(role.color.valueOr(0), 6, 16, QLatin1Char('0')) : QStringLiteral("#949ba4");

        rml += QStringLiteral("<div class=\"role-list-item%1\" data-action=\"select-role\" data-id=\"%2\">")
                   .arg(active, QString::number(quint64(role.id.get())));
        rml += QStringLiteral("<div class=\"role-item-dot\" style=\"background-color:%1;\"></div>").arg(color);
        rml += QStringLiteral("<span style=\"overflow:hidden; text-overflow:ellipsis; white-space:nowrap;\">%1</span>").arg(escape(role.name.valueOr("new role")));
        rml += QStringLiteral("</div>");
    }
    rml += QStringLiteral("</div>");

    // Right Editor
    rml += QStringLiteral("<div class=\"role-editor-pane\">");

    RoleDraft activeDraft;
    if (dirtyRoleDrafts.contains(selectedRoleId))
        activeDraft = dirtyRoleDrafts[selectedRoleId];
    else if (savedRoleDrafts.contains(selectedRoleId))
        activeDraft = savedRoleDrafts[selectedRoleId];
    else {
        for (const auto &r : roles) {
            if (r.id.get() == selectedRoleId) {
                activeDraft.name = r.name.valueOr("role");
                activeDraft.color = r.color.valueOr(0);
                activeDraft.hoist = r.hoist.valueOr(false);
                activeDraft.mentionable = r.mentionable.valueOr(false);
                activeDraft.permissions = r.permissions.valueOr(Discord::NO_PERMISSIONS);
                savedRoleDrafts[selectedRoleId] = activeDraft;
                break;
            }
        }
    }

    // Role tabs
    rml += QStringLiteral("<div class=\"tabs-bar\">");
    auto addTab = [&](RoleTab tab, const QString &label) {
        QString act = (currentRoleTab == tab) ? QStringLiteral(" active") : QString();
        rml += QStringLiteral("<div class=\"tab-btn%1\" data-action=\"select-role-tab\" data-tab=\"%2\">%3</div>")
                   .arg(act, QString::number(static_cast<int>(tab)), label);
    };
    addTab(RoleTab::Display, "Display");
    addTab(RoleTab::Permissions, "Permissions");
    addTab(RoleTab::Members, "Manage Members");
    rml += QStringLiteral("</div>");

    if (currentRoleTab == RoleTab::Display) {
        rml += QStringLiteral("<div class=\"settings-row\">");
        rml += QStringLiteral("<div class=\"settings-label\">Role Name</div>");
        rml += QStringLiteral("<input type=\"text\" class=\"settings-text-input\" id=\"role-name-input\" value=\"%1\" style=\"width:100%;\"/>")
                   .arg(escape(activeDraft.name));
        rml += QStringLiteral("</div>");

        // Role Color
        rml += QStringLiteral("<div class=\"settings-row\">");
        rml += QStringLiteral("<div class=\"settings-label\">Role Color</div>");
        rml += QStringLiteral("<div class=\"brand-colors-row\">");
        const int roleColors[] = { 0, 0x1abc9c, 0x2ecc71, 0x3498db, 0x9b59b6, 0xe91e63, 0xf1c40f, 0xe67e22, 0xe74c3c, 0x95a5a6 };
        for (int c : roleColors) {
            QString cHex = (c != 0) ? QStringLiteral("#%1").arg(c, 6, 16, QLatin1Char('0')) : QStringLiteral("#4e5058");
            QString act = (activeDraft.color == c) ? QStringLiteral(" active") : QString();
            rml += QStringLiteral("<div class=\"brand-color-swatch%1\" style=\"background-color:%2;\" data-action=\"select-role-color\" data-color=\"%3\"></div>")
                       .arg(act, cHex, QString::number(c));
        }
        rml += QStringLiteral("</div>");
        rml += QStringLiteral("</div>");

        // Hoist & Mentionable
        rml += QStringLiteral("<div class=\"settings-switch-row\">");
        rml += QStringLiteral("<div class=\"settings-switch-text\"><div class=\"settings-switch-title\">Display role members separately from online members</div></div>");
        QString hoistChk = activeDraft.hoist ? QStringLiteral(" checked") : QString();
        rml += QStringLiteral("<div class=\"settings-switch%1\" data-action=\"toggle-role-hoist\"><div class=\"settings-switch-thumb\"></div></div>").arg(hoistChk);
        rml += QStringLiteral("</div>");

        rml += QStringLiteral("<div class=\"settings-switch-row\">");
        rml += QStringLiteral("<div class=\"settings-switch-text\"><div class=\"settings-switch-title\">Allow anyone to @mention this role</div></div>");
        QString mentionChk = activeDraft.mentionable ? QStringLiteral(" checked") : QString();
        rml += QStringLiteral("<div class=\"settings-switch%1\" data-action=\"toggle-role-mention\"><div class=\"settings-switch-thumb\"></div></div>").arg(mentionChk);
        rml += QStringLiteral("</div>");

        // Delete Role
        if (selectedRoleId != currentGuildId) {
            rml += QStringLiteral("<div style=\"margin-top:24dp; border-top:1dp #202024; padding-top:16dp;\">");
            rml += QStringLiteral("<div class=\"settings-btn settings-btn-danger\" data-action=\"delete-role\" data-id=\"%1\">Delete Role</div>")
                       .arg(QString::number(quint64(selectedRoleId)));
            rml += QStringLiteral("</div>");
        }
    } else if (currentRoleTab == RoleTab::Permissions) {
        rml += QStringLiteral("<div style=\"margin-bottom:16dp; display:flex; flex-direction:row; justify-content:space-between; align-items:center;\">");
        rml += QStringLiteral("<input type=\"text\" class=\"settings-text-input\" id=\"perm-search-input\" value=\"%1\" placeholder=\"Search Permissions…\" style=\"flex:1; margin-right:12dp;\"/>")
                   .arg(escape(permissionSearchQuery));
        rml += QStringLiteral("<div class=\"settings-btn settings-btn-secondary\" data-action=\"clear-perms\">Clear Permissions</div>");
        rml += QStringLiteral("</div>");

        const char *currentCategory = "";
        for (const auto &p : AllPermissions) {
            if (!permissionSearchQuery.isEmpty() &&
                !QString::fromLatin1(p.name).contains(permissionSearchQuery, Qt::CaseInsensitive) &&
                !QString::fromLatin1(p.desc).contains(permissionSearchQuery, Qt::CaseInsensitive))
                continue;

            if (strcmp(p.category, currentCategory) != 0) {
                currentCategory = p.category;
                rml += QStringLiteral("<div class=\"settings-subheading\">%1 Permissions</div>").arg(currentCategory);
            }

            bool granted = (activeDraft.permissions & p.perm) == p.perm;
            rml += QStringLiteral("<div class=\"settings-switch-row\">");
            rml += QStringLiteral("<div class=\"settings-switch-text\"><div class=\"settings-switch-title\">%1</div><div class=\"settings-switch-desc\">%2</div></div>")
                       .arg(p.name, p.desc);
            QString chk = granted ? QStringLiteral(" checked") : QString();
            rml += QStringLiteral("<div class=\"settings-switch%1\" data-action=\"toggle-perm\" data-perm=\"%2\"><div class=\"settings-switch-thumb\"></div></div>")
                       .arg(chk, QString::number(static_cast<quint64>(p.perm)));
            rml += QStringLiteral("</div>");
        }
    } else {
        rml += QStringLiteral("<div class=\"empty-state\"><div class=\"empty-state-title\">Role Members</div><div class=\"empty-state-desc\">Assigned to members from the Members tab.</div></div>");
    }

    rml += QStringLiteral("</div>"); // role-editor-pane
    rml += QStringLiteral("</div>"); // roles-container
}

// 8. Invites
void GuildSettingsController::renderInvites(QString &rml)
{
    rml += QStringLiteral("<div class=\"settings-section-title\" style=\"display:flex; flex-direction:row; justify-content:space-between; align-items:center;\">");
    rml += QStringLiteral("<span>Active Invites (%1)</span>").arg(QString::number(invites.size()));
    rml += QStringLiteral("</div>");

    if (invites.isEmpty()) {
        rml += QStringLiteral("<div class=\"empty-state\"><div class=\"empty-state-title\">No Active Invites</div><div class=\"empty-state-desc\">No one has created an instant invite for this server yet.</div></div>");
        return;
    }

    rml += QStringLiteral("<div class=\"guild-table\">");
    rml += QStringLiteral("<div class=\"guild-table-header\"><div class=\"col-name\">Inviter</div><div class=\"col-name\">Invite Code</div><div class=\"col-channel\">Channel</div><div class=\"col-uses\">Uses</div><div class=\"col-expires\">Expires</div><div class=\"col-actions\">Actions</div></div>");

    for (const auto &inv : invites) {
        rml += QStringLiteral("<div class=\"guild-table-row\">");

        QString inviterName = inv.inviter.hasValue() ? inv.inviter->username.valueOr("Unknown") : "System";
        rml += QStringLiteral("<div class=\"col-name\" style=\"font-weight:600; color:#f2f3f5;\">%1</div>").arg(escape(inviterName));
        rml += QStringLiteral("<div class=\"col-name\" style=\"color:#00a8fc;\">%1</div>").arg(escape(inv.code.valueOr("")));
        QString chName = inv.channel.hasValue() ? inv.channel->name.valueOr("channel") : "—";
        rml += QStringLiteral("<div class=\"col-channel\"># %1</div>").arg(escape(chName));
        rml += QStringLiteral("<div class=\"col-uses\">%1</div>").arg(QString::number(inv.uses.valueOr(0)));

        QString expText = "Never";
        if (inv.maxAge.valueOr(0) > 0)
            expText = QStringLiteral("%1s").arg(QString::number(inv.maxAge.valueOr(0)));
        rml += QStringLiteral("<div class=\"col-expires\">%1</div>").arg(expText);

        rml += QStringLiteral("<div class=\"col-actions\"><div class=\"settings-btn settings-btn-danger\" style=\"padding:4dp 8dp; font-size:12dp;\" data-action=\"revoke-invite\" data-code=\"%1\">Revoke</div></div>")
                   .arg(escape(inv.code.valueOr("")));
        rml += QStringLiteral("</div>");
    }
    rml += QStringLiteral("</div>");
}

// 9. Audit Log
void GuildSettingsController::renderAuditLog(QString &rml)
{
    rml += QStringLiteral("<div class=\"settings-section-title\">Server Audit Log</div>");

    // Filter bar
    rml += QStringLiteral("<div style=\"display:flex; flex-direction:row; margin-bottom:16dp;\">");
    rml += QStringLiteral("<div style=\"flex:1; margin-right:12dp;\">");
    rml += QStringLiteral("<div class=\"settings-label\">Filter by Action</div>");
    rml += QStringLiteral("<select class=\"settings-select\" id=\"select-audit-action\">");
    rml += QStringLiteral("<option value=\"-1\"%1>All Actions</option>").arg(auditFilterAction == -1 ? QStringLiteral(" selected") : QString());
    for (const auto &filter : AuditLogFormatter::actionFilters()) {
        QString sel = (auditFilterAction == static_cast<int>(filter.first)) ? QStringLiteral(" selected") : QString();
        rml += QStringLiteral("<option value=\"%1\"%2>%3</option>").arg(QString::number(static_cast<int>(filter.first)), sel, escape(filter.second));
    }
    rml += QStringLiteral("</select>");
    rml += QStringLiteral("</div>");
    rml += QStringLiteral("</div>");

    if (auditRows.isEmpty()) {
        rml += QStringLiteral("<div class=\"empty-state\"><div class=\"empty-state-title\">No Audit Logs</div><div class=\"empty-state-desc\">No logged administrative actions found for this server.</div></div>");
        return;
    }

    int idx = 0;
    for (const auto &row : auditRows) {
        if (auditFilterAction != -1 && static_cast<int>(row.action) != auditFilterAction) {
            idx++;
            continue;
        }

        bool isExpanded = expandedAuditRows.contains(idx);
        rml += QStringLiteral("<div class=\"audit-log-card\" data-action=\"toggle-audit-row\" data-idx=\"%1\" style=\"cursor:pointer;\">")
                   .arg(QString::number(idx));

        rml += QStringLiteral("<div class=\"audit-log-header\">");
        rml += QStringLiteral("<div class=\"audit-log-title\">%1</div>").arg(row.titleHtml);
        rml += QStringLiteral("<div class=\"audit-log-time\">%1</div>").arg(AuditLogFormatter::timeText(row.start, row.end));
        rml += QStringLiteral("</div>");

        if (isExpanded && !row.lines.isEmpty()) {
            rml += QStringLiteral("<div class=\"audit-log-details\">");
            for (const auto &line : row.lines) {
                rml += QStringLiteral("<div class=\"audit-change-line\">%1</div>").arg(line.html);
            }
            rml += QStringLiteral("</div>");
        }

        rml += QStringLiteral("</div>");
        idx++;
    }
}

// 10. Bans
void GuildSettingsController::renderBans(QString &rml)
{
    rml += QStringLiteral("<div class=\"settings-section-title\">Bans (%1)</div>").arg(QString::number(bans.size()));

    rml += QStringLiteral("<div style=\"margin-bottom:16dp;\">");
    rml += QStringLiteral("<input type=\"text\" class=\"settings-text-input\" id=\"ban-search-input\" value=\"%1\" placeholder=\"Search bans by username or ID…\" style=\"width:100%;\"/>")
               .arg(escape(banSearchQuery));
    rml += QStringLiteral("</div>");

    if (bans.isEmpty()) {
        rml += QStringLiteral("<div class=\"empty-state\"><div class=\"empty-state-title\">No Bans</div><div class=\"empty-state-desc\">No members are currently banned from this server.</div></div>");
        return;
    }

    rml += QStringLiteral("<div class=\"guild-table\">");
    rml += QStringLiteral("<div class=\"guild-table-header\"><div class=\"col-name\">User</div><div class=\"col-uploader\">Reason</div><div class=\"col-actions\">Actions</div></div>");

    for (const auto &b : bans) {
        if (!b.user.hasValue())
            continue;

        QString userTag = b.user->username.valueOr("Banned User");
        QString reason = b.reason.valueOr("No reason provided.");

        if (!banSearchQuery.isEmpty() &&
            !userTag.contains(banSearchQuery, Qt::CaseInsensitive) &&
            !QString::number(quint64(b.user->id.get())).contains(banSearchQuery))
            continue;

        rml += QStringLiteral("<div class=\"guild-table-row\">");
        rml += QStringLiteral("<div class=\"col-name\"><span style=\"font-weight:600; color:#f2f3f5;\">%1</span> <span style=\"color:#949ba4;\">(ID: %2)</span></div>")
                   .arg(escape(userTag), QString::number(quint64(b.user->id.get())));
        rml += QStringLiteral("<div class=\"col-uploader\">%1</div>").arg(escape(reason));
        rml += QStringLiteral("<div class=\"col-actions\"><div class=\"settings-btn settings-btn-secondary\" style=\"padding:4dp 8dp; font-size:12dp;\" data-action=\"revoke-ban\" data-id=\"%1\" data-name=\"%2\">Revoke Ban</div></div>")
                   .arg(QString::number(quint64(b.user->id.get())), escape(userTag));
        rml += QStringLiteral("</div>");
    }
    rml += QStringLiteral("</div>");
}

// 11. Delete Server
void GuildSettingsController::renderDeleteServer(QString &rml)
{
    rml += QStringLiteral("<div class=\"settings-section-title\" style=\"color:#f23f43;\">Delete Server</div>");
    rml += QStringLiteral("<div class=\"settings-card danger-card\">");
    rml += QStringLiteral("<div style=\"font-size:15dp; font-weight:bold; color:#f23f43; margin-bottom:8dp;\">Are you sure you want to delete this server?</div>");
    rml += QStringLiteral("<div class=\"settings-description\" style=\"color:#dbdee1;\">Deleting this server is permanent and cannot be undone. All channels, messages, roles, and members will be immediately removed.</div>");
    rml += QStringLiteral("<div class=\"settings-btn settings-btn-danger\" style=\"margin-top:12dp;\" data-action=\"delete-server\">Delete Server</div>");
    rml += QStringLiteral("</div>");
}

// Data loaders
void GuildSettingsController::loadCurrentSectionData()
{
    switch (currentSection) {
    case GuildSettingsSection::Profile:
        loadProfile();
        break;
    case GuildSettingsSection::Engagement:
        loadEngagement();
        break;
    case GuildSettingsSection::BoostPerks:
        loadBoostPerks();
        break;
    case GuildSettingsSection::Emoji:
        loadEmoji();
        break;
    case GuildSettingsSection::Stickers:
        loadStickers();
        break;
    case GuildSettingsSection::Members:
        loadMembers();
        break;
    case GuildSettingsSection::Roles:
        loadRoles();
        break;
    case GuildSettingsSection::Invites:
        loadInvites();
        break;
    case GuildSettingsSection::AuditLog:
        loadAuditLog();
        break;
    case GuildSettingsSection::Bans:
        loadBans();
        break;
    case GuildSettingsSection::DeleteServer:
        break;
    }
}

void GuildSettingsController::loadProfile()
{
    auto g = guild();
    if (!g)
        return;

    savedProfile.name = g->name.valueOr("");
    savedProfile.description = g->description.valueOr("");
    savedProfile.iconDataUri.clear();
    savedProfile.iconRemoved = false;
    savedProfile.brandColor = "#5865f2";
    savedProfile.isPrivate = false;
    for (int i = 0; i < 6; ++i)
        savedProfile.traits[i].clear();

    draftProfile = savedProfile;
    profileLoaded = true;

    auto *inst = instance();
    if (inst && inst->discord()) {
        QPointer<GuildSettingsController> self(this);
        inst->discord()->fetchGuildProfile(currentGuildId, [self](const Core::Result<Discord::GuildProfileEdit> &res) {
            if (!self || !res.success())
                return;
            self->savedProfile.name = res.value->name;
            self->savedProfile.description = res.value->description;
            self->savedProfile.brandColor = res.value->brandColorPrimary.toString();
            self->savedProfile.isPrivate = (res.value->visibility.toInt() != 1);
            for (int i = 0; i < 6 && i < res.value->traits.size(); ++i)
                self->savedProfile.traits[i] = res.value->traits[i].label;

            self->draftProfile = self->savedProfile;
            if (self->currentSection == GuildSettingsSection::Profile)
                self->render();
        });
    }
}

void GuildSettingsController::loadEngagement()
{
    auto g = guild();
    if (!g)
        return;

    savedEngagement.systemChannelId = g->systemChannelId.valueOr(Core::Snowflake::Invalid);
    savedEngagement.systemChannelFlags = g->systemChannelFlags.valueOr(Discord::SystemChannelFlags{});
    savedEngagement.suppressJoin = savedEngagement.systemChannelFlags.testFlag(Discord::SystemChannelFlag::SUPPRESS_JOIN_NOTIFICATIONS);
    savedEngagement.suppressBoost = savedEngagement.systemChannelFlags.testFlag(Discord::SystemChannelFlag::SUPPRESS_PREMIUM_SUBSCRIPTIONS);
    savedEngagement.suppressTips = savedEngagement.systemChannelFlags.testFlag(Discord::SystemChannelFlag::SUPPRESS_GUILD_REMINDER_NOTIFICATIONS);
    savedEngagement.suppressPrompt = savedEngagement.systemChannelFlags.testFlag(Discord::SystemChannelFlag::SUPPRESS_JOIN_NOTIFICATION_REPLIES);
    savedEngagement.defaultNotifications = g->defaultMessageNotifications.valueOr(Discord::MessageNotificationLevel::ALL_MESSAGES);
    savedEngagement.afkChannelId = g->afkChannelId.valueOr(Core::Snowflake::Invalid);
    savedEngagement.afkTimeout = g->afkTimeout.valueOr(300);

    draftEngagement = savedEngagement;
    engagementLoaded = true;
}

void GuildSettingsController::loadBoostPerks()
{
    auto g = guild();
    if (!g)
        return;

    savedBoost.progressBar = g->premiumProgressBarEnabled.valueOr(false);
    savedBoost.bannerUri.clear();
    savedBoost.bannerRemoved = false;
    savedBoost.splashUri.clear();
    savedBoost.splashRemoved = false;

    draftBoost = savedBoost;
    boostLoaded = true;
}

void GuildSettingsController::loadEmoji()
{
    auto *inst = instance();
    if (!inst || !inst->discord())
        return;
    QPointer<GuildSettingsController> self(this);
    inst->discord()->fetchGuildEmojis(currentGuildId, [self](const Core::Result<QList<Discord::Emoji>> &res) {
        if (!self || !res.success())
            return;
        self->emojis = *res.value;
        self->emojisLoaded = true;
        if (self->currentSection == GuildSettingsSection::Emoji)
            self->render();
    });
}

void GuildSettingsController::loadStickers()
{
    auto *inst = instance();
    if (!inst || !inst->discord())
        return;
    QPointer<GuildSettingsController> self(this);
    inst->discord()->fetchGuildStickers(currentGuildId, [self](const Core::Result<QList<Discord::Sticker>> &res) {
        if (!self || !res.success())
            return;
        self->stickers = *res.value;
        self->stickersLoaded = true;
        if (self->currentSection == GuildSettingsSection::Stickers)
            self->render();
    });
}

void GuildSettingsController::loadMembers()
{
    auto *inst = instance();
    if (!inst || !inst->discord())
        return;
    Discord::MemberSearchQuery query;
    query.limit = 100;
    QPointer<GuildSettingsController> self(this);
    inst->discord()->searchGuildMembers(currentGuildId, query, [self](const Core::Result<Discord::Client::MemberSearchPage> &res) {
        if (!self || !res.success())
            return;
        self->membersLoaded = true;
        self->members.clear();
        for (const auto &item : res.value->members) {
            if (item.member.hasValue())
                self->members.append(item.member.get());
        }
        if (self->currentSection == GuildSettingsSection::Members)
            self->render();
    });
}

void GuildSettingsController::loadRoles()
{
    auto *inst = instance();
    if (!inst)
        return;

    roles = inst->getRolesForGuild(currentGuildId);
    rolesLoaded = true;
}

void GuildSettingsController::loadInvites()
{
    auto *inst = instance();
    if (!inst || !inst->discord())
        return;

    QPointer<GuildSettingsController> self(this);
    inst->discord()->fetchGuildInvites(currentGuildId, [self](const Core::Result<QList<Discord::Invite>> &res) {
        if (!self || !res.success())
            return;
        self->invites = *res.value;
        self->invitesLoaded = true;
        if (self->currentSection == GuildSettingsSection::Invites)
            self->render();
    });
}

void GuildSettingsController::loadAuditLog()
{
    auto *inst = instance();
    if (!inst || !inst->discord())
        return;

    if (!auditFormatter)
        auditFormatter = std::make_unique<AuditLogFormatter>(inst, currentGuildId);

    Discord::Client::AuditLogQuery query;
    QPointer<GuildSettingsController> self(this);
    inst->discord()->fetchAuditLog(currentGuildId, query, [self](const Core::Result<Discord::AuditLog> &res) {
        if (!self || !res.success())
            return;
        self->auditRows = self->auditFormatter->addPage(*res.value);
        self->auditLogLoaded = true;
        if (self->currentSection == GuildSettingsSection::AuditLog)
            self->render();
    });
}

void GuildSettingsController::loadBans()
{
    auto *inst = instance();
    if (!inst || !inst->discord())
        return;

    QPointer<GuildSettingsController> self(this);
    inst->discord()->fetchBans(currentGuildId, Core::Snowflake::Invalid, [self](const Core::Result<QList<Discord::Ban>> &res) {
        if (!self || !res.success())
            return;
        self->bans = *res.value;
        self->bansLoaded = true;
        if (self->currentSection == GuildSettingsSection::Bans)
            self->render();
    });
}
// Unsaved changes bar
bool GuildSettingsController::hasUnsavedChanges() const
{
    switch (currentSection) {
    case GuildSettingsSection::Profile:
        if (draftProfile.name != savedProfile.name ||
            draftProfile.description != savedProfile.description ||
            !draftProfile.iconDataUri.isEmpty() ||
            draftProfile.iconRemoved != savedProfile.iconRemoved ||
            draftProfile.brandColor != savedProfile.brandColor ||
            draftProfile.isPrivate != savedProfile.isPrivate)
            return true;
        for (int i = 0; i < 6; ++i)
            if (draftProfile.traits[i] != savedProfile.traits[i])
                return true;
        return false;

    case GuildSettingsSection::Engagement:
        return (draftEngagement.systemChannelId != savedEngagement.systemChannelId ||
                draftEngagement.suppressJoin != savedEngagement.suppressJoin ||
                draftEngagement.suppressBoost != savedEngagement.suppressBoost ||
                draftEngagement.suppressTips != savedEngagement.suppressTips ||
                draftEngagement.suppressPrompt != savedEngagement.suppressPrompt ||
                draftEngagement.defaultNotifications != savedEngagement.defaultNotifications ||
                draftEngagement.afkChannelId != savedEngagement.afkChannelId ||
                draftEngagement.afkTimeout != savedEngagement.afkTimeout);

    case GuildSettingsSection::BoostPerks:
        return (draftBoost.progressBar != savedBoost.progressBar ||
                !draftBoost.bannerUri.isEmpty() ||
                draftBoost.bannerRemoved != savedBoost.bannerRemoved ||
                !draftBoost.splashUri.isEmpty() ||
                draftBoost.splashRemoved != savedBoost.splashRemoved);

    case GuildSettingsSection::Roles:
        return !dirtyRoleDrafts.isEmpty();

    default:
        return false;
    }
}

void GuildSettingsController::discardChanges()
{
    draftProfile = savedProfile;
    draftEngagement = savedEngagement;
    draftBoost = savedBoost;
    dirtyRoleDrafts.clear();
    updateUnsavedChangesBar();
}

void GuildSettingsController::updateUnsavedChangesBar()
{
    if (!document)
        return;

    auto *bar = document->GetElementById("guild-unsaved-changes-bar");
    if (!bar)
        return;

    bool dirty = hasUnsavedChanges();
    bar->SetClass("visible", dirty);
}

// Event Listeners
void GuildSettingsController::saveChanges()
{
    if (!canEditCurrentSection()) {
        reportError(tr("You do not have permission to change these settings."));
        return;
    }
    auto *inst = instance();
    if (!inst || !inst->discord())
        return;
    const auto guildId = currentGuildId;
    const auto generation = requestGeneration;
    QPointer<GuildSettingsController> self(this);
    auto complete = [self, guildId, generation](const Core::Result<void> &result) {
        if (!self || !self->isCurrentRequest(guildId, generation))
            return;
        if (!result.success()) {
            self->reportError(result.error);
            return;
        }
        self->errorMessage.clear();
        self->loadCurrentSectionData();
        self->render();
    };
    if (currentSection == GuildSettingsSection::Profile) {
        Discord::GuildProfileEdit edit = savedGuildProfile;
        edit.name = draftProfile.name;
        edit.description = draftProfile.description;
        edit.icon = draftProfile.iconRemoved ? QJsonValue(QJsonValue::Null)
                                             : (draftProfile.iconDataUri.isEmpty() ? savedGuildProfile.icon : QJsonValue(draftProfile.iconDataUri));
        edit.brandColorPrimary = QJsonValue(draftProfile.brandColor);
        edit.visibility = QJsonValue(draftProfile.isPrivate ? 2 : 1);
        edit.traits.clear();
        for (int i = 0; i < Discord::GuildProfileEdit::TraitSlots; ++i)
            edit.traits.append({ draftProfile.traits[i], QJsonValue(), QJsonValue(), QJsonValue() });
        inst->discord()->modifyGuildProfile(guildId, edit, [self, guildId, generation](const Core::Result<Discord::GuildProfileEdit> &result) {
            if (!self || !self->isCurrentRequest(guildId, generation))
                return;
            if (!result.success()) {
                self->reportError(result.error);
                return;
            }
            self->errorMessage.clear();
            self->loadProfile();
            self->render();
        });
    } else if (currentSection == GuildSettingsSection::Engagement || currentSection == GuildSettingsSection::BoostPerks) {
        Discord::GuildEdit edit;
        if (currentSection == GuildSettingsSection::Engagement) {
            Discord::SystemChannelFlags flags;
            flags.setFlag(Discord::SystemChannelFlag::SUPPRESS_JOIN_NOTIFICATIONS, draftEngagement.suppressJoin);
            flags.setFlag(Discord::SystemChannelFlag::SUPPRESS_PREMIUM_SUBSCRIPTIONS, draftEngagement.suppressBoost);
            flags.setFlag(Discord::SystemChannelFlag::SUPPRESS_GUILD_REMINDER_NOTIFICATIONS, draftEngagement.suppressTips);
            flags.setFlag(Discord::SystemChannelFlag::SUPPRESS_JOIN_NOTIFICATION_REPLIES, draftEngagement.suppressPrompt);
            edit.systemChannelId = draftEngagement.systemChannelId;
            edit.systemChannelFlags = flags;
            edit.defaultMessageNotifications = draftEngagement.defaultNotifications;
            edit.afkChannelId = draftEngagement.afkChannelId;
            edit.afkTimeout = draftEngagement.afkTimeout;
        } else {
            edit.premiumProgressBarEnabled = draftBoost.progressBar;
            edit.banner = draftBoost.bannerRemoved ? QString() : draftBoost.bannerUri;
            edit.splash = draftBoost.splashRemoved ? QString() : draftBoost.splashUri;
        }
        inst->discord()->modifyGuild(guildId, edit, {}, complete);
    } else if (currentSection == GuildSettingsSection::Roles) {
        for (auto it = dirtyRoleDrafts.cbegin(); it != dirtyRoleDrafts.cend(); ++it) {
            Discord::RoleEdit edit;
            edit.name = it->name;
            edit.color = it->color;
            edit.hoist = it->hoist;
            edit.mentionable = it->mentionable;
            edit.permissions = it->permissions;
            inst->discord()->modifyRole(guildId, it.key(), edit, complete);
        }
    }
}

void GuildSettingsController::setupEventListeners()
{
    if (!document)
        return;

    listen("click", [this](::Rml::Event &event) {
        handleClick(event.GetTargetElement(), event);
    }, document);

    listen("input", [this](::Rml::Event &event) {
        handleInput(event.GetTargetElement(), event);
    }, document);

    listen("change", [this](::Rml::Event &event) {
        handleChange(event.GetTargetElement(), event);
    }, document);

    listen("keydown", [this](::Rml::Event &event) {
        handleKeyDown(event);
    }, document);
}

void GuildSettingsController::handleClick(::Rml::Element *target, ::Rml::Event &event)
{
    if (!target)
        return;

    auto *actionEl = closest(target, "data-action");
    if (!actionEl)
        return;

    QString action = attribute(actionEl, "data-action");

    if (action != QLatin1String("close") && action != QLatin1String("nav-section") &&
        action != QLatin1String("toggle-audit-row") && !canEditCurrentSection()) {
        reportError(tr("You do not have permission to change these settings."));
        return;
    }

    if (action == QLatin1String("close")) {
        event.StopPropagation();
        close();
    } else if (action == QLatin1String("nav-section")) {
        event.StopPropagation();
        bool ok = false;
        int secInt = attribute(actionEl, "data-section").toInt(&ok);
        if (ok)
            setSection(static_cast<GuildSettingsSection>(secInt));
    } else if (action == QLatin1String("reset-changes")) {
        event.StopPropagation();
        discardChanges();
        renderContent();
        requestRender(100);
    } else if (action == QLatin1String("save-changes")) {
        event.StopPropagation();
        saveChanges();
    }
    // Profile Actions
    else if (action == QLatin1String("choose-icon")) {
        event.StopPropagation();
        QString path = QFileDialog::getOpenFileName(nullptr, tr("Change Server Icon"), QString(), ImageUpload::fileFilter());
        if (!path.isEmpty()) {
            auto prep = ImageUpload::prepareCropped(path, 1024, 1024);
            if (!prep.dataUri.isEmpty()) {
                draftProfile.iconDataUri = prep.dataUri;
                draftProfile.iconRemoved = false;
                renderContent();
                updateUnsavedChangesBar();
            }
        }
    } else if (action == QLatin1String("remove-icon")) {
        event.StopPropagation();
        draftProfile.iconDataUri.clear();
        draftProfile.iconRemoved = true;
        renderContent();
        updateUnsavedChangesBar();
    } else if (action == QLatin1String("select-brand-color")) {
        event.StopPropagation();
        draftProfile.brandColor = attribute(actionEl, "data-color");
        renderContent();
        updateUnsavedChangesBar();
    } else if (action == QLatin1String("toggle-private")) {
        event.StopPropagation();
        draftProfile.isPrivate = !draftProfile.isPrivate;
        renderContent();
        updateUnsavedChangesBar();
    }
    // Engagement Actions
    else if (action == QLatin1String("toggle-flag-join")) {
        event.StopPropagation();
        draftEngagement.suppressJoin = !draftEngagement.suppressJoin;
        renderContent();
        updateUnsavedChangesBar();
    } else if (action == QLatin1String("toggle-flag-boost")) {
        event.StopPropagation();
        draftEngagement.suppressBoost = !draftEngagement.suppressBoost;
        renderContent();
        updateUnsavedChangesBar();
    } else if (action == QLatin1String("toggle-flag-tips")) {
        event.StopPropagation();
        draftEngagement.suppressTips = !draftEngagement.suppressTips;
        renderContent();
        updateUnsavedChangesBar();
    } else if (action == QLatin1String("toggle-flag-prompt")) {
        event.StopPropagation();
        draftEngagement.suppressPrompt = !draftEngagement.suppressPrompt;
        renderContent();
        updateUnsavedChangesBar();
    } else if (action == QLatin1String("select-notif-all")) {
        event.StopPropagation();
        draftEngagement.defaultNotifications = Discord::MessageNotificationLevel::ALL_MESSAGES;
        renderContent();
        updateUnsavedChangesBar();
    } else if (action == QLatin1String("select-notif-mentions")) {
        event.StopPropagation();
        draftEngagement.defaultNotifications = Discord::MessageNotificationLevel::ONLY_MENTIONS;
        renderContent();
        updateUnsavedChangesBar();
    }
    // Boost Actions
    else if (action == QLatin1String("toggle-progress-bar")) {
        event.StopPropagation();
        draftBoost.progressBar = !draftBoost.progressBar;
        renderContent();
        updateUnsavedChangesBar();
    } else if (action == QLatin1String("choose-banner")) {
        event.StopPropagation();
        QString path = QFileDialog::getOpenFileName(nullptr, tr("Upload Banner"), QString(), ImageUpload::fileFilter());
        if (!path.isEmpty()) {
            auto prep = ImageUpload::prepareCropped(path, 960, 540);
            if (!prep.dataUri.isEmpty()) {
                draftBoost.bannerUri = prep.dataUri;
                draftBoost.bannerRemoved = false;
                renderContent();
                updateUnsavedChangesBar();
            }
        }
    } else if (action == QLatin1String("remove-banner")) {
        event.StopPropagation();
        draftBoost.bannerUri.clear();
        draftBoost.bannerRemoved = true;
        renderContent();
        updateUnsavedChangesBar();
    } else if (action == QLatin1String("choose-splash")) {
        event.StopPropagation();
        QString path = QFileDialog::getOpenFileName(nullptr, tr("Upload Splash"), QString(), ImageUpload::fileFilter());
        if (!path.isEmpty()) {
            auto prep = ImageUpload::prepareCropped(path, 1920, 1080);
            if (!prep.dataUri.isEmpty()) {
                draftBoost.splashUri = prep.dataUri;
                draftBoost.splashRemoved = false;
                renderContent();
                updateUnsavedChangesBar();
            }
        }
    } else if (action == QLatin1String("remove-splash")) {
        event.StopPropagation();
        draftBoost.splashUri.clear();
        draftBoost.splashRemoved = true;
        renderContent();
        updateUnsavedChangesBar();
    }
    // Emoji & Stickers
    else if (action == QLatin1String("choose-emoji-upload")) {
        event.StopPropagation();
        const QString path = QFileDialog::getOpenFileName(nullptr, tr("Upload Emoji"), {}, ImageUpload::fileFilter());
        const auto image = ImageUpload::prepareRaw(path, 256 * 1024);
        if (path.isEmpty())
            return;
        if (image.dataUri.isEmpty()) { reportError(image.error); return; }
        const auto guildId = currentGuildId;
        QPointer<GuildSettingsController> self(this);
        instance()->discord()->createGuildEmoji(guildId, QFileInfo(path).baseName(), image.dataUri, image.originalMd5,
            [self, guildId](const Core::Result<Discord::Emoji> &result) {
                if (!self || self->currentGuildId != guildId) return;
                if (!result.success()) { self->reportError(result.error); return; }
                self->loadEmoji();
            });
    } else if (action == QLatin1String("delete-emoji")) {
        event.StopPropagation();
        const auto emojiId = Core::Snowflake(attribute(actionEl, "data-id").toULongLong());
        showConfirmModal(shell, tr("Delete Emoji"), tr("Are you sure you want to delete this emoji?"), tr("Delete"),
            [this, emojiId]() { if (auto *inst = instance()) inst->discord()->deleteGuildEmoji(currentGuildId, emojiId, [this](const Core::Result<void> &r) { if (!r.success()) reportError(r.error); else loadEmoji(); }); }, nullptr, true);
    } else if (action == QLatin1String("choose-sticker-upload")) {
        event.StopPropagation();
        const QString path = QFileDialog::getOpenFileName(nullptr, tr("Upload Sticker"), {}, ImageUpload::fileFilter());
        if (path.isEmpty()) return;
        const auto image = ImageUpload::prepareRaw(path, 512 * 1024);
        if (image.dataUri.isEmpty()) { reportError(image.error); return; }
        Discord::FileUpload file{ QFileInfo(path).fileName(), QByteArray::fromBase64(image.dataUri.section(',', 1).toLatin1()), QStringLiteral("image/png"), {} };
        instance()->discord()->createGuildSticker(currentGuildId, QFileInfo(path).baseName(), QStringLiteral("sticker"), {}, file, image.originalMd5,
            [this](const Core::Result<Discord::Sticker> &r) { if (!r.success()) reportError(r.error); else loadStickers(); });
    } else if (action == QLatin1String("delete-sticker")) {
        event.StopPropagation();
        const auto stickerId = Core::Snowflake(attribute(actionEl, "data-id").toULongLong());
        showConfirmModal(shell, tr("Delete Sticker"), tr("Are you sure you want to delete this sticker?"), tr("Delete"),
            [this, stickerId]() { if (auto *inst = instance()) inst->discord()->deleteGuildSticker(currentGuildId, stickerId, [this](const Core::Result<void> &r) { if (!r.success()) reportError(r.error); else loadStickers(); }); }, nullptr, true);
    }
    // Members Actions
    else if (action == QLatin1String("kick-member") || action == QLatin1String("ban-member")) {
        event.StopPropagation();
        const bool ban = action == QLatin1String("ban-member");
        const auto userId = Core::Snowflake(attribute(actionEl, "data-id").toULongLong());
        const QString name = attribute(actionEl, "data-name");
        showConfirmModal(shell, ban ? tr("Ban Member") : tr("Kick Member"),
            ban ? tr("Are you sure you want to ban <b>@%1</b> from the server?").arg(name) : tr("Are you sure you want to kick <b>@%1</b> from the server?").arg(name),
            ban ? tr("Ban") : tr("Kick"), [this, ban, userId]() {
                if (auto *inst = instance()) {
                    auto done = [this](const Core::Result<void> &r) { if (!r.success()) reportError(r.error); else loadMembers(); };
                    if (ban) inst->discord()->banMember(currentGuildId, userId, 0, {}, done);
                    else inst->discord()->kickMember(currentGuildId, userId, {}, done);
                }
            }, nullptr, true);
    } else if (action == QLatin1String("prune-members")) {
        event.StopPropagation();
        showConfirmModal(shell, tr("Prune Members"), tr("Pruning will kick members who have not been seen in over 7 days and have no roles. Continue?"), tr("Prune"),
            [this]() { if (auto *inst = instance()) inst->discord()->pruneMembers(currentGuildId, 7, {}, [this](const Core::Result<void> &r) { if (!r.success()) reportError(r.error); else loadMembers(); }); }, nullptr, true);
    }
    // Roles Actions
    else if (action == QLatin1String("select-role")) {
        event.StopPropagation();
        selectedRoleId = Core::Snowflake(attribute(actionEl, "data-id").toULongLong());
        renderContent();
    } else if (action == QLatin1String("select-role-tab")) {
        event.StopPropagation();
        currentRoleTab = static_cast<RoleTab>(attribute(actionEl, "data-tab").toInt());
        renderContent();
    } else if (action == QLatin1String("create-role")) {
        event.StopPropagation();
        if (auto *inst = instance())
            inst->discord()->createRole(currentGuildId, [this](const Core::Result<Discord::Role> &r) { if (!r.success()) reportError(r.error); else { roles = instance()->getRolesForGuild(currentGuildId); selectedRoleId = r.value->id.get(); render(); } });
    } else if (action == QLatin1String("delete-role")) {
        event.StopPropagation();
        const auto roleId = selectedRoleId;
        showConfirmModal(shell, tr("Delete Role"), tr("Are you sure you want to delete this role? This action cannot be undone."), tr("Delete"),
            [this, roleId]() { if (auto *inst = instance()) inst->discord()->deleteRole(currentGuildId, roleId, [this](const Core::Result<void> &r) { if (!r.success()) reportError(r.error); else { loadRoles(); render(); } }); }, nullptr, true);
    } else if (action == QLatin1String("select-role-color")) {
        event.StopPropagation();
        int c = attribute(actionEl, "data-color").toInt();
        RoleDraft d = dirtyRoleDrafts.contains(selectedRoleId) ? dirtyRoleDrafts[selectedRoleId] : savedRoleDrafts[selectedRoleId];
        d.color = c;
        dirtyRoleDrafts[selectedRoleId] = d;
        renderContent();
        updateUnsavedChangesBar();
    } else if (action == QLatin1String("toggle-role-hoist")) {
        event.StopPropagation();
        RoleDraft d = dirtyRoleDrafts.contains(selectedRoleId) ? dirtyRoleDrafts[selectedRoleId] : savedRoleDrafts[selectedRoleId];
        d.hoist = !d.hoist;
        dirtyRoleDrafts[selectedRoleId] = d;
        renderContent();
        updateUnsavedChangesBar();
    } else if (action == QLatin1String("toggle-role-mention")) {
        event.StopPropagation();
        RoleDraft d = dirtyRoleDrafts.contains(selectedRoleId) ? dirtyRoleDrafts[selectedRoleId] : savedRoleDrafts[selectedRoleId];
        d.mentionable = !d.mentionable;
        dirtyRoleDrafts[selectedRoleId] = d;
        renderContent();
        updateUnsavedChangesBar();
    } else if (action == QLatin1String("toggle-perm")) {
        event.StopPropagation();
        quint64 pVal = attribute(actionEl, "data-perm").toULongLong();
        Discord::Permission perm = static_cast<Discord::Permission>(pVal);
        RoleDraft d = dirtyRoleDrafts.contains(selectedRoleId) ? dirtyRoleDrafts[selectedRoleId] : savedRoleDrafts[selectedRoleId];
        d.permissions.setFlag(perm, !d.permissions.testFlag(perm));
        dirtyRoleDrafts[selectedRoleId] = d;
        renderContent();
        updateUnsavedChangesBar();
    } else if (action == QLatin1String("clear-perms")) {
        event.StopPropagation();
        RoleDraft d = dirtyRoleDrafts.contains(selectedRoleId) ? dirtyRoleDrafts[selectedRoleId] : savedRoleDrafts[selectedRoleId];
        d.permissions = Discord::NO_PERMISSIONS;
        dirtyRoleDrafts[selectedRoleId] = d;
        renderContent();
        updateUnsavedChangesBar();
    }
    // Invites
    else if (action == QLatin1String("revoke-invite")) {
        event.StopPropagation();
        const QString code = attribute(actionEl, "data-code");
        showConfirmModal(shell, tr("Revoke Invite"), tr("Are you sure you want to revoke invite <b>%1</b>?").arg(code), tr("Revoke"),
            [this, code]() { if (auto *inst = instance()) inst->discord()->revokeInvite(code, [this](const Core::Result<void> &r) { if (!r.success()) reportError(r.error); else loadInvites(); }); }, nullptr, true);
    }
    // Audit Log
    else if (action == QLatin1String("toggle-audit-row")) {
        event.StopPropagation();
        int idx = attribute(actionEl, "data-idx").toInt();
        if (expandedAuditRows.contains(idx))
            expandedAuditRows.remove(idx);
        else
            expandedAuditRows.insert(idx);
        renderContent();
    }
    // Bans
    else if (action == QLatin1String("revoke-ban")) {
        event.StopPropagation();
        const auto userId = Core::Snowflake(attribute(actionEl, "data-id").toULongLong());
        const QString name = attribute(actionEl, "data-name");
        showConfirmModal(shell, tr("Revoke Ban"), tr("Are you sure you want to revoke the ban for <b>%1</b>?").arg(name), tr("Revoke Ban"),
            [this, userId]() { if (auto *inst = instance()) inst->discord()->unbanMember(currentGuildId, userId, [this](const Core::Result<void> &r) { if (!r.success()) reportError(r.error); else loadBans(); }); }, nullptr, false);
    }
    // Delete Server
    else if (action == QLatin1String("delete-server")) {
        event.StopPropagation();
        const QString gName = guild() ? guild()->name.valueOr("this server") : tr("this server");
        showConfirmModal(shell, tr("Delete '%1'").arg(gName), tr("Are you sure you want to delete <b>%1</b>? This action cannot be undone.").arg(gName), tr("Delete Server"),
            [this]() { if (auto *inst = instance()) inst->discord()->deleteGuild(currentGuildId, [this](const Core::Result<void> &r) { if (!r.success()) reportError(r.error); else close(); }); }, nullptr, true);
    }
}

void GuildSettingsController::handleInput(::Rml::Element *target, ::Rml::Event &event)
{
    if (!target)
        return;

    QString id = QString::fromStdString(target->GetId());

    if (id == QLatin1String("profile-name-input")) {
        auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(target);
        if (input) {
            draftProfile.name = QString::fromStdString(input->GetValue());
            updateUnsavedChangesBar();
        }
    } else if (id == QLatin1String("profile-desc-input")) {
        if (auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(target))
            draftProfile.description = QString::fromStdString(input->GetValue());
        updateUnsavedChangesBar();
    } else if (id.startsWith(QLatin1String("trait-input-"))) {
        auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(target);
        if (input) {
            int idx = attribute(target, "data-trait-idx").toInt();
            if (idx >= 0 && idx < 6) {
                draftProfile.traits[idx] = QString::fromStdString(input->GetValue());
                updateUnsavedChangesBar();
            }
        }
    } else if (id == QLatin1String("member-search-input")) {
        auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(target);
        if (input) {
            memberSearchQuery = QString::fromStdString(input->GetValue());
            renderContent();
        }
    } else if (id == QLatin1String("perm-search-input")) {
        auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(target);
        if (input) {
            permissionSearchQuery = QString::fromStdString(input->GetValue());
            renderContent();
        }
    } else if (id == QLatin1String("ban-search-input")) {
        auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(target);
        if (input) {
            banSearchQuery = QString::fromStdString(input->GetValue());
            renderContent();
        }
    } else if (id == QLatin1String("role-name-input")) {
        auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(target);
        if (input) {
            RoleDraft d = dirtyRoleDrafts.contains(selectedRoleId) ? dirtyRoleDrafts[selectedRoleId] : savedRoleDrafts[selectedRoleId];
            d.name = QString::fromStdString(input->GetValue());
            dirtyRoleDrafts[selectedRoleId] = d;
            updateUnsavedChangesBar();
        }
    }
}

void GuildSettingsController::handleChange(::Rml::Element *target, ::Rml::Event &event)
{
    if (!target)
        return;

    auto *select = dynamic_cast<::Rml::ElementFormControlSelect *>(target);
    if (!select)
        return;

    QString id = QString::fromStdString(target->GetId());
    QString val = QString::fromStdString(select->GetValue());

    if (id == QLatin1String("select-system-channel")) {
        draftEngagement.systemChannelId = Core::Snowflake(val.toULongLong());
        updateUnsavedChangesBar();
    } else if (id == QLatin1String("select-afk-channel")) {
        draftEngagement.afkChannelId = Core::Snowflake(val.toULongLong());
        updateUnsavedChangesBar();
    } else if (id == QLatin1String("select-afk-timeout")) {
        draftEngagement.afkTimeout = val.toInt();
        updateUnsavedChangesBar();
    } else if (id == QLatin1String("select-audit-action")) {
        auditFilterAction = val.toInt();
        renderContent();
    }
}

void GuildSettingsController::handleKeyDown(::Rml::Event &event)
{
    const auto key = static_cast<::Rml::Input::KeyIdentifier>(event.GetParameter<int>("key_identifier", 0));
    if (key == ::Rml::Input::KI_ESCAPE) {
        event.StopPropagation();
        close();
    }
}

} // namespace Rml
} // namespace UI
} // namespace Styx
