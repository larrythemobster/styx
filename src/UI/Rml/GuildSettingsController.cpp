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

#include "Platform/Desktop.hpp"
#include "Platform/Utf8.hpp"
#include "Platform/Base64.hpp"
#include <charconv>
#include <cstring>
#include <filesystem>
#include <format>
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
template<class T = uint64_t> T number(std::string_view text, bool *ok = nullptr)
{
    T value = 0;
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    const bool valid = parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
    if (ok) *ok = valid;
    return valid ? value : T{};
}


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

GuildSettingsController::GuildSettingsController(Shell &shell)
    : Controller(shell, nullptr)
{
}

GuildSettingsController::~GuildSettingsController()
{
    removeListeners();
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

bool GuildSettingsController::isCurrentRequest(Core::Snowflake guildId, std::uint64_t generation) const
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

void GuildSettingsController::reportError(const std::string &message)
{
    errorMessage = message;
    if (document) {
        if (auto *text = document->GetElementById("guild-unsaved-changes-text"))
            text->SetInnerRML(escape(message));
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

std::string GuildSettingsController::getGuildIconUrl() const
{
    if (!draftProfile.iconDataUri.empty())
        return draftProfile.iconDataUri;
    if (draftProfile.iconRemoved)
        return {};

    auto g = guild();
    if (g && g->icon.hasValue() && !g->icon->empty())
        return shell.imageSource(Discord::Cdn::guildIcon(currentGuildId, g->icon.get(), 128), {96, 96});
    return {};
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
            shell, "Discard unsaved changes?",
            "You have unsaved changes, are you sure you want to discard them?",
            "Discard", [this]() { discardChanges(); close(); }, nullptr, true);
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
            "Discard unsaved changes?",
            "You have unsaved changes, are you sure you want to discard them?",
            "Discard",
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

    std::string rml;
    auto g = guild();
    std::string guildName = g ? g->name.valueOr("Server") : "Server";

    rml += "<div class=\"settings-group-header\" style=\"color:#ffffff; font-size:14dp; margin-bottom:12dp;\">" +
           escape(guildName) + "</div>";

    auto sections = GuildSettingsAccess::visibleSections(instance(), currentGuildId);
    GuildSettingsGroup lastGroup = static_cast<GuildSettingsGroup>(-1);

    for (GuildSettingsSection sec : sections) {
        GuildSettingsGroup grp = GuildSettingsAccess::group(sec);
        if (grp != lastGroup) {
            lastGroup = grp;
            std::string grpTitle = GuildSettingsAccess::groupTitle(grp);
            if (!grpTitle.empty()) {
                rml += "<div class=\"settings-group-header\">" + escape(grpTitle) + "</div>";
            } else if (sec == GuildSettingsSection::Profile) {
                rml += "<div class=\"settings-group-header\">Server Settings</div>";
            } else if (grp == GuildSettingsGroup::Danger) {
                rml += "<div class=\"settings-divider\"></div>";
            }
        }

        std::string itemClass = "settings-nav-item";
        if (currentSection == sec)
            itemClass += " active";
        if (sec == GuildSettingsSection::DeleteServer)
            itemClass += " settings-btn-danger";

        rml += "<div class=\"" + itemClass + "\" data-action=\"nav-section\" data-section=\"" +
               std::to_string(static_cast<int>(sec)) + "\">" + escape(GuildSettingsAccess::title(sec)) + "</div>";
    }

    sidebar->SetInnerRML(rml);
}

void GuildSettingsController::renderContent()
{
    auto *content = document->GetElementById("guild-settings-content-wrapper");
    if (!content)
        return;

    std::string rml;
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

    content->SetInnerRML(rml);
}

// 1. Overview / Profile
void GuildSettingsController::renderProfile(std::string &rml)
{
    rml += "<div class=\"settings-section-title\">Server Overview &amp; Profile</div>";

    // Icon Row
    rml += "<div class=\"guild-icon-upload-row\">";
    std::string iconSrc = getGuildIconUrl();
    rml += "<div class=\"guild-icon-preview\">";
    if (!iconSrc.empty()) {
        rml += "<img src=\"" + escape(iconSrc) + "\"/>";
    } else {
        auto g = guild();
        const std::string name = g ? g->name.valueOr("?") : "?";
        std::string initials = Platform::unicodeUpper(Platform::truncateUtf8(name, 2));
        if (initials.empty())
            initials = "?";
        rml += "<div class=\"guild-icon-initials\">" + escape(initials) + "</div>";
    }
    rml += "</div>";

    rml += "<div class=\"guild-icon-actions\">";
    rml += "<div class=\"settings-btn settings-btn-primary\" data-action=\"choose-icon\">Change Icon</div>";
    if (!iconSrc.empty())
        rml += "<div class=\"settings-btn settings-btn-danger\" data-action=\"remove-icon\">Remove Icon</div>";
    rml += "<div class=\"settings-note\" style=\"color:#949ba4; font-style:normal;\">Minimum Size: 128x128</div>";
    rml += "</div>";
    rml += "</div>";

    // Server Name
    rml += "<div class=\"settings-row\">";
    rml += "<div class=\"settings-label\">Server Name</div>";
    rml += "<input type=\"text\" class=\"settings-text-input\" id=\"profile-name-input\" value=\"" +
           escape(draftProfile.name) + "\" maxlength=\"100\"/>";
    rml += "</div>";

    // Server Description
    rml += "<div class=\"settings-row\">";
    rml += "<div class=\"settings-label\">Server Description</div>";
    rml += "<div class=\"settings-description\">Tell the world what your server is about.</div>";
    rml += "<textarea class=\"settings-textarea\" id=\"profile-desc-input\">" + escape(draftProfile.description) + "</textarea>";
    rml += "</div>";

    // Server Banner Brand Color
    rml += "<div class=\"settings-row\">";
    rml += "<div class=\"settings-label\">Server Brand Color</div>";
    rml += "<div class=\"brand-colors-row\">";
    for (const auto &preset : BrandColors) {
        const bool active = Platform::equalsIgnoreCaseAscii(draftProfile.brandColor, preset.color);
        rml += std::string("<div class=\"brand-color-swatch") + (active ? " active" : "") +
               "\" style=\"background-color:" + preset.color + ";\" data-action=\"select-brand-color\" data-color=\"" +
               preset.color + "\" title=\"" + preset.name + "\"></div>";
    }
    rml += "</div>";
    rml += "</div>";

    // Server Traits
    rml += "<div class=\"settings-row\">";
    rml += "<div class=\"settings-label\">Server Traits</div>";
    rml += "<div class=\"settings-description\">Add up to 6 traits that define your community.</div>";
    rml += "<div class=\"traits-grid\">";
    for (int i = 0; i < 6; ++i) {
        const std::string index = std::to_string(i);
        rml += "<div class=\"trait-input-col\">";
        rml += "<input type=\"text\" class=\"settings-text-input\" id=\"trait-input-" + index +
               "\" data-trait-idx=\"" + index + "\" value=\"" + escape(draftProfile.traits[i]) +
               "\" maxlength=\"24\" placeholder=\"Trait " + std::to_string(i + 1) + "\"/>";
        rml += "</div>";
    }
    rml += "</div>";
    rml += "</div>";

    // Private Server Switch
    rml += "<div class=\"settings-switch-row\">";
    rml += "<div class=\"settings-switch-text\">";
    rml += "<div class=\"settings-switch-title\">Private Server</div>";
    rml += "<div class=\"settings-switch-desc\">Only members can view profile content and server details.</div>";
    rml += "</div>";
    rml += std::string("<div class=\"settings-switch") + (draftProfile.isPrivate ? " checked" : "") +
           "\" data-action=\"toggle-private\"><div class=\"settings-switch-thumb\"></div></div>";
    rml += "</div>";
}

// 2. Engagement
void GuildSettingsController::renderEngagement(std::string &rml)
{
    rml += "<div class=\"settings-section-title\">Engagement &amp; System Messages</div>";

    auto *inst = instance();
    std::vector<Discord::Channel> channels;
    if (inst)
        channels = inst->getGuildChannels(currentGuildId);
    // System Messages Channel
    rml += "<div class=\"settings-row\">";
    rml += "<div class=\"settings-label\">System Messages Channel</div>";
    rml += "<div class=\"settings-description\">Channel where welcome messages and server boost announcements are posted.</div>";
    rml += "<select class=\"settings-select\" id=\"select-system-channel\">";
    rml += std::string("<option value=\"0\"") + (!draftEngagement.systemChannelId.isValid() ? " selected" : "") +
           ">No System Messages</option>";
    for (const auto &ch : channels) {
        if (ch.type.valueOr(Discord::ChannelType::GUILD_TEXT) == Discord::ChannelType::GUILD_TEXT) {
            const bool selected = ch.id.get() == draftEngagement.systemChannelId;
            rml += "<option value=\"" + ch.id.get().toString() + "\"" + (selected ? " selected" : "") + "># " +
                   escape(ch.name.valueOr("channel")) + "</option>";
        }
    }
    rml += "</select>";
    rml += "</div>";

    // System Flags
    rml += "<div class=\"settings-subheading\">System Message Notifications</div>";

    auto addFlag = [&](const std::string &title, const std::string &desc, bool checked, const std::string &action) {
        rml += "<div class=\"settings-switch-row\">";
        rml += "<div class=\"settings-switch-text\">";
        rml += "<div class=\"settings-switch-title\">" + escape(title) + "</div>";
        rml += "<div class=\"settings-switch-desc\">" + escape(desc) + "</div>";
        rml += "</div>";
        rml += std::string("<div class=\"settings-switch") + (checked ? " checked" : "") + "\" data-action=\"" + action +
               "\"><div class=\"settings-switch-thumb\"></div></div>";
        rml += "</div>";
    };

    addFlag("Suppress member join notifications", "Don't send a message when someone joins this server.", draftEngagement.suppressJoin, "toggle-flag-join");
    addFlag("Suppress premium subscription notifications", "Don't send a message when someone boosts this server.", draftEngagement.suppressBoost, "toggle-flag-boost");
    addFlag("Suppress server setup tips", "Don't send helpful server setup tips to new members.", draftEngagement.suppressTips, "toggle-flag-tips");
    addFlag("Suppress member prompt reply notifications", "Don't notify members when someone replies to member prompts.", draftEngagement.suppressPrompt, "toggle-flag-prompt");

    // Default Notifications
    rml += "<div class=\"settings-subheading\">Default Notification Settings</div>";
    rml += "<div class=\"settings-description\">Determines whether members receive notifications for all messages or only @mentions by default.</div>";
    rml += "<div class=\"settings-radio-group\">";

    bool allMsgs = (draftEngagement.defaultNotifications == Discord::MessageNotificationLevel::ALL_MESSAGES);
    rml += std::string("<div class=\"settings-radio-card") + (allMsgs ? " active" : "") + "\" data-action=\"select-notif-all\">";
    rml += "<div class=\"settings-radio-indicator\"></div>";
    rml += "<div class=\"settings-radio-info\"><div class=\"settings-radio-title\">All Messages</div><div class=\"settings-radio-desc\">Recommended for small communities and private servers.</div></div>";
    rml += "</div>";

    rml += std::string("<div class=\"settings-radio-card") + (!allMsgs ? " active" : "") + "\" data-action=\"select-notif-mentions\">";
    rml += "<div class=\"settings-radio-indicator\"></div>";
    rml += "<div class=\"settings-radio-info\"><div class=\"settings-radio-title\">Only @mentions</div><div class=\"settings-radio-desc\">Recommended for public or larger servers.</div></div>";
    rml += "</div>";
    rml += "</div>";

    // AFK Channel & Timeout
    rml += "<div class=\"settings-subheading\">Inactive (AFK) Settings</div>";
    rml += "<div class=\"settings-grid-2col\">";
    rml += "<div class=\"settings-grid-col settings-grid-col-left\">";
    rml += "<div class=\"settings-label\">Inactive Channel</div>";
    rml += "<select class=\"settings-select\" id=\"select-afk-channel\">";
    rml += std::string("<option value=\"0\"") + (!draftEngagement.afkChannelId.isValid() ? " selected" : "") +
           ">No Inactive Channel</option>";
    for (const auto &ch : channels) {
        if (ch.type.valueOr(Discord::ChannelType::GUILD_VOICE) == Discord::ChannelType::GUILD_VOICE) {
            const bool selected = ch.id.get() == draftEngagement.afkChannelId;
            rml += "<option value=\"" + ch.id.get().toString() + "\"" + (selected ? " selected" : "") + ">" +
                   escape(ch.name.valueOr("voice")) + "</option>";
        }
    }
    rml += "</select>";
    rml += "</div>";

    rml += "<div class=\"settings-grid-col settings-grid-col-right\">";
    rml += "<div class=\"settings-label\">Inactive Timeout</div>";
    rml += "<select class=\"settings-select\" id=\"select-afk-timeout\">";
    const int timeouts[] = { 60, 300, 900, 1800, 3600 };
    const char *timeoutLabels[] = { "1 minute", "5 minutes", "15 minutes", "30 minutes", "1 hour" };
    for (int t = 0; t < 5; ++t) {
        const bool selected = draftEngagement.afkTimeout == timeouts[t];
        rml += "<option value=\"" + std::to_string(timeouts[t]) + "\"" + (selected ? " selected" : "") + ">" +
               timeoutLabels[t] + "</option>";
    }
    rml += "</select>";
    rml += "</div>";
    rml += "</div>";
}

// 3. Boost Perks
void GuildSettingsController::renderBoostPerks(std::string &rml)
{
    rml += "<div class=\"settings-section-title\">Server Boost Perks</div>";

    auto g = guild();
    int boostCount = g ? g->premiumSubscriptionCount.valueOr(0) : 0;
    int tier = g ? static_cast<int>(g->premiumTier.valueOr(Discord::PremiumTier::NONE)) : 0;

    rml += "<div class=\"settings-card\" style=\"display:flex; flex-direction:row; align-items:center; justify-content:space-between;\">";
    rml += "<div>";
    rml += "<div class=\"boost-badge\">LEVEL " + std::to_string(tier) + "</div>";
    rml += "<div style=\"font-size:16dp; font-weight:bold; color:#ffffff;\">" + std::to_string(boostCount) + " Server Boosts</div>";
    rml += "<div style=\"font-size:13dp; color:#949ba4;\">Unlock custom banners, vanity URLs, and extra emoji slots.</div>";
    rml += "</div>";
    rml += "</div>";

    // Progress Bar Toggle
    rml += "<div class=\"settings-switch-row\">";
    rml += "<div class=\"settings-switch-text\">";
    rml += "<div class=\"settings-switch-title\">Show Boost Progress Bar</div>";
    rml += "<div class=\"settings-switch-desc\">Displays current Server Boost progress at the top of your channel list.</div>";
    rml += "</div>";
    rml += std::string("<div class=\"settings-switch") + (draftBoost.progressBar ? " checked" : "") +
           "\" data-action=\"toggle-progress-bar\"><div class=\"settings-switch-thumb\"></div></div>";
    rml += "</div>";

    // Banner slot
    rml += "<div class=\"settings-card\">";
    rml += "<div class=\"settings-label\">Server Banner Background</div>";
    rml += "<div class=\"settings-description\">Displayed at the top of your channel list. Minimum 960x540 (16:9 ratio).</div>";
    if (tier >= 2) {
        rml += "<div class=\"settings-button-group\">";
        rml += "<div class=\"settings-btn settings-btn-primary\" data-action=\"choose-banner\">Upload Banner</div>";
        rml += "<div class=\"settings-btn settings-btn-danger\" data-action=\"remove-banner\">Remove Banner</div>";
        rml += "</div>";
    } else {
        rml += "<div class=\"settings-note\">Locked \xE2\x80\x94 Requires Level 2 Server Boosts (7 Boosts).</div>";
    }
    rml += "</div>";

    // Invite Splash slot
    rml += "<div class=\"settings-card\">";
    rml += "<div class=\"settings-label\">Server Invite Splash</div>";
    rml += "<div class=\"settings-description\">Shown when people visit your invite links in a web browser. Minimum 1920x1080.</div>";
    if (tier >= 1) {
        rml += "<div class=\"settings-button-group\">";
        rml += "<div class=\"settings-btn settings-btn-primary\" data-action=\"choose-splash\">Upload Splash</div>";
        rml += "<div class=\"settings-btn settings-btn-danger\" data-action=\"remove-splash\">Remove Splash</div>";
        rml += "</div>";
    } else {
        rml += "<div class=\"settings-note\">Locked \xE2\x80\x94 Requires Level 1 Server Boosts (2 Boosts).</div>";
    }
    rml += "</div>";
}

// 4. Emoji
void GuildSettingsController::renderEmoji(std::string &rml)
{
    rml += "<div class=\"settings-section-title\" style=\"display:flex; flex-direction:row; justify-content:space-between; align-items:center;\">";
    rml += "<span>Emoji</span>";
    rml += "<div class=\"settings-btn settings-btn-primary\" data-action=\"choose-emoji-upload\">Upload Emoji</div>";
    rml += "</div>";

    int staticCount = 0;
    int animCount = 0;
    for (const auto &em : emojis) {
        if (em.animated.valueOr(false))
            animCount++;
        else
            staticCount++;
    }

    rml += "<div class=\"slots-counter\">Static Emojis: " + std::to_string(staticCount) +
           " / 50 slots used \xC2\xB7 Animated Emojis: " + std::to_string(animCount) + " / 50 slots used</div>";

    if (emojis.empty()) {
        rml += "<div class=\"empty-state\"><div class=\"empty-state-title\">No Custom Emojis</div><div class=\"empty-state-desc\">Upload images (PNG, JPG, GIF) under 256 KB to use them anywhere on your server.</div></div>";
        return;
    }

    rml += "<div class=\"guild-table\">";
    rml += "<div class=\"guild-table-header\"><div class=\"col-image\">Image</div><div class=\"col-name\">Name</div><div class=\"col-uploader\">Uploaded By</div><div class=\"col-actions\">Actions</div></div>";

    for (const auto &em : emojis) {
        rml += "<div class=\"guild-table-row\">";
        const std::string emSrc = shell.imageSource(Discord::Cdn::emoji(em.id.get(), 32, em.animated.valueOr(false)), {32, 32});
        rml += "<div class=\"col-image\"><img class=\"emoji-preview-img\" src=\"" + escape(emSrc) + "\"/></div>";
        rml += "<div class=\"col-name\" style=\"font-weight:600; color:#f2f3f5;\">:" + escape(em.name.valueOr("emoji")) + ":</div>";
        const std::string uploaderName = em.user.hasValue() ? em.user->username.valueOr("Unknown") : "Server";
        rml += "<div class=\"col-uploader\">" + escape(uploaderName) + "</div>";
        rml += "<div class=\"col-actions\"><div class=\"settings-btn settings-btn-danger\" style=\"padding:4dp 8dp; font-size:12dp;\" data-action=\"delete-emoji\" data-id=\"" +
               em.id.get().toString() + "\">Delete</div></div>";
        rml += "</div>";
    }
    rml += "</div>";
}

// 5. Stickers
void GuildSettingsController::renderStickers(std::string &rml)
{
    rml += std::string("<div class=\"settings-section-title\" style=\"display:flex; flex-direction:row; justify-content:space-between; align-items:center;\">");
    rml += std::string("<span>Stickers</span>");
    rml += std::string("<div class=\"settings-btn settings-btn-primary\" data-action=\"choose-sticker-upload\">Upload Sticker</div>");
    rml += std::string("</div>");

    rml += std::format("<div class=\"slots-counter\">{0} / 5 Custom Sticker slots used</div>", std::to_string(stickers.size()));

    if (stickers.empty()) {
        rml += std::string("<div class=\"empty-state\"><div class=\"empty-state-title\">No Custom Stickers</div><div class=\"empty-state-desc\">Upload PNG, APNG, or Lottie sticker files under 512 KB to express yourself.</div></div>");
        return;
    }

    rml += std::string("<div class=\"guild-table\">");
    rml += std::string("<div class=\"guild-table-header\"><div class=\"col-image\">Sticker</div><div class=\"col-name\">Name & Related Emoji</div><div class=\"col-uploader\">Description</div><div class=\"col-actions\">Actions</div></div>");

    for (const auto &st : stickers) {
        rml += std::string("<div class=\"guild-table-row\">");
        std::string stUrl = Discord::Cdn::stickerStill(st.id.get(), st.formatType.valueOr(Discord::StickerFormatType::PNG), 64);
        std::string stSrc = shell.imageSource(stUrl, Platform::Size(48, 48));
        rml += std::format("<div class=\"col-image\"><img class=\"sticker-preview-img\" src=\"{0}\"/></div>", escape(stSrc));
        rml += std::format("<div class=\"col-name\"><span style=\"font-weight:600; color:#f2f3f5;\">{0}</span> <span style=\"color:#949ba4;\">(:{1}:)</span></div>", escape(st.name.valueOr("sticker")), escape(st.tags.valueOr("")));
        rml += std::format("<div class=\"col-uploader\">{0}</div>", escape(st.description.valueOr("")));
        rml += std::format("<div class=\"col-actions\"><div class=\"settings-btn settings-btn-danger\" style=\"padding:4dp 8dp; font-size:12dp;\" data-action=\"delete-sticker\" data-id=\"{0}\">Delete</div></div>", std::to_string(uint64_t(st.id.get())));
        rml += std::string("</div>");
    }
    rml += std::string("</div>");
}

// 6. Members
void GuildSettingsController::renderMembers(std::string &rml)
{
    rml += std::string("<div class=\"settings-section-title\" style=\"display:flex; flex-direction:row; justify-content:space-between; align-items:center;\">");
    rml += std::format("<span>Members ({0})</span>", std::to_string(members.size()));
    rml += std::string("<div class=\"settings-btn settings-btn-danger\" data-action=\"prune-members\">Prune Members</div>");
    rml += std::string("</div>");

    // Search bar
    rml += std::string("<div style=\"margin-bottom:16dp;\">");
    rml += std::format("<input type=\"text\" class=\"settings-text-input\" id=\"member-search-input\" value=\"{0}\" placeholder=\"Search Members by username or nickname…\" style=\"width:100%;\"/>", escape(memberSearchQuery));
    rml += std::string("</div>");

    auto *inst = instance();

    rml += std::string("<div class=\"guild-table\">");
    rml += std::string("<div class=\"guild-table-header\"><div style=\"width:48dp;\">User</div><div class=\"col-name\">Name</div><div class=\"col-channel\">Roles</div><div class=\"col-expires\">Joined</div><div class=\"col-actions\">Actions</div></div>");

    int shown = 0;
    const auto search = Platform::caseFold(memberSearchQuery);
    for (const auto &m : members) {
        if (!m.user.hasValue())
            continue;

        std::string name = m.nick.hasValue() && !m.nick->empty() ? m.nick.get() : m.user->username.valueOr("Member");
        std::string username = m.user->username.valueOr("");

        if (!memberSearchQuery.empty()) {
            if (Platform::caseFold(name).find(search) == std::string::npos &&
                Platform::caseFold(username).find(search) == std::string::npos)
                continue;
        }

        shown++;
        rml += std::string("<div class=\"guild-table-row\">");

        // Avatar
        std::string avUrl = Discord::Cdn::userAvatar(m.user->id.get(), m.user->avatar.valueOr(""), 36);
        std::string avSrc = shell.imageSource(avUrl, Platform::Size(36, 36));
        rml += std::format("<div style=\"width:48dp;\"><img class=\"member-avatar-img\" src=\"{0}\"/></div>", escape(avSrc));

        // Name
        rml += std::format("<div class=\"col-name\"><div class=\"member-name\">{0}</div><div class=\"member-subtext\">@{1}</div></div>", escape(name), escape(username));

        // Roles
        rml += std::string("<div class=\"col-channel\"><div class=\"member-roles\">");
        if (inst) {
            auto roleIds = m.roles.valueOr(std::vector<Core::Snowflake>{});
            for (const auto &rId : roleIds) {
                for (const auto &r : roles) {
                    if (r.id.get() == rId) {
                        std::string rColor = (r.color.valueOr(0) != 0) ? std::format("#{:06X}", r.color.valueOr(0)) : std::string("#949ba4");
                        rml += std::format("<div class=\"role-badge\"><div class=\"role-badge-dot\" style=\"background-color:{0};\"></div>{1}</div>", rColor, escape(r.name.valueOr("role")));
                        break;
                    }
                }
            }
        }
        rml += std::string("</div></div>");

        // Joined
        std::string joined = m.joinedAt.hasValue() ? m.joinedAt->format("%Y-%m-%d") : "—";
        rml += std::format("<div class=\"col-expires\">{0}</div>", joined);

        // Actions: Kick, Ban
        rml += std::string("<div class=\"col-actions\" style=\"display:flex; flex-direction:row; justify-content:flex-end;\">");
        auto target = MemberModeration::targetFor(inst, currentGuildId, m.user->id.get());
        if (target && MemberModeration::canKick(inst, *target)) {
            rml += std::format("<div class=\"settings-btn settings-btn-danger\" style=\"padding:4dp 8dp; font-size:11dp; margin-right:4dp;\" data-action=\"kick-member\" data-id=\"{0}\" data-name=\"{1}\">Kick</div>", std::to_string(uint64_t(m.user->id.get())), escape(username));
        }
        if (target && MemberModeration::canBan(inst, *target)) {
            rml += std::format("<div class=\"settings-btn settings-btn-danger\" style=\"padding:4dp 8dp; font-size:11dp;\" data-action=\"ban-member\" data-id=\"{0}\" data-name=\"{1}\">Ban</div>", std::to_string(uint64_t(m.user->id.get())), escape(username));
        }
        rml += std::string("</div>");

        rml += std::string("</div>");
        if (shown >= 100)
            break;
    }

    if (shown == 0) {
        rml += std::string("<div class=\"empty-state\"><div class=\"empty-state-title\">No Members Found</div></div>");
    }
    rml += std::string("</div>");
}

// 7. Roles
void GuildSettingsController::renderRoles(std::string &rml)
{
    rml += std::string("<div class=\"settings-section-title\">Roles</div>");

    rml += std::string("<div class=\"roles-container\">");

    // Left List
    rml += std::string("<div class=\"roles-list-pane\">");
    rml += std::string("<div class=\"settings-btn settings-btn-primary\" style=\"margin-bottom:12dp; text-align:center;\" data-action=\"create-role\">+ Create Role</div>");

    if (roles.empty() && instance())
        roles = instance()->getRolesForGuild(currentGuildId);

    if (!selectedRoleId.isValid() && !roles.empty())
        selectedRoleId = roles.front().id.get();

    for (const auto &role : roles) {
        std::string active = (role.id.get() == selectedRoleId) ? std::string(" active") : std::string();
        std::string color = (role.color.valueOr(0) != 0) ? std::format("#{:06X}", role.color.valueOr(0)) : std::string("#949ba4");

        rml += std::format("<div class=\"role-list-item{0}\" data-action=\"select-role\" data-id=\"{1}\">", active, std::to_string(uint64_t(role.id.get())));
        rml += std::format("<div class=\"role-item-dot\" style=\"background-color:{0};\"></div>", color);
        rml += std::format("<span style=\"overflow:hidden; text-overflow:ellipsis; white-space:nowrap;\">{0}</span>", escape(role.name.valueOr("new role")));
        rml += std::string("</div>");
    }
    rml += std::string("</div>");

    // Right Editor
    rml += std::string("<div class=\"role-editor-pane\">");

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
    rml += std::string("<div class=\"tabs-bar\">");
    auto addTab = [&](RoleTab tab, const std::string &label) {
        std::string act = (currentRoleTab == tab) ? std::string(" active") : std::string();
        rml += std::format("<div class=\"tab-btn{0}\" data-action=\"select-role-tab\" data-tab=\"{1}\">{2}</div>", act, std::to_string(static_cast<int>(tab)), label);
    };
    addTab(RoleTab::Display, "Display");
    addTab(RoleTab::Permissions, "Permissions");
    addTab(RoleTab::Members, "Manage Members");
    rml += std::string("</div>");

    if (currentRoleTab == RoleTab::Display) {
        rml += std::string("<div class=\"settings-row\">");
        rml += std::string("<div class=\"settings-label\">Role Name</div>");
        rml += std::format("<input type=\"text\" class=\"settings-text-input\" id=\"role-name-input\" value=\"{0}\" style=\"width:100%;\"/>", escape(activeDraft.name));
        rml += std::string("</div>");

        // Role Color
        rml += std::string("<div class=\"settings-row\">");
        rml += std::string("<div class=\"settings-label\">Role Color</div>");
        rml += std::string("<div class=\"brand-colors-row\">");
        const int roleColors[] = { 0, 0x1abc9c, 0x2ecc71, 0x3498db, 0x9b59b6, 0xe91e63, 0xf1c40f, 0xe67e22, 0xe74c3c, 0x95a5a6 };
        for (int c : roleColors) {
            std::string cHex = (c != 0) ? std::format("#{:06X}", c) : std::string("#4e5058");
            std::string act = (activeDraft.color == c) ? std::string(" active") : std::string();
            rml += std::format("<div class=\"brand-color-swatch{0}\" style=\"background-color:{1};\" data-action=\"select-role-color\" data-color=\"{2}\"></div>", act, cHex, std::to_string(c));
        }
        rml += std::string("</div>");
        rml += std::string("</div>");

        // Hoist & Mentionable
        rml += std::string("<div class=\"settings-switch-row\">");
        rml += std::string("<div class=\"settings-switch-text\"><div class=\"settings-switch-title\">Display role members separately from online members</div></div>");
        std::string hoistChk = activeDraft.hoist ? std::string(" checked") : std::string();
        rml += std::format("<div class=\"settings-switch{0}\" data-action=\"toggle-role-hoist\"><div class=\"settings-switch-thumb\"></div></div>", hoistChk);
        rml += std::string("</div>");

        rml += std::string("<div class=\"settings-switch-row\">");
        rml += std::string("<div class=\"settings-switch-text\"><div class=\"settings-switch-title\">Allow anyone to @mention this role</div></div>");
        std::string mentionChk = activeDraft.mentionable ? std::string(" checked") : std::string();
        rml += std::format("<div class=\"settings-switch{0}\" data-action=\"toggle-role-mention\"><div class=\"settings-switch-thumb\"></div></div>", mentionChk);
        rml += std::string("</div>");

        // Delete Role
        if (selectedRoleId != currentGuildId) {
            rml += std::string("<div style=\"margin-top:24dp; border-top:1dp #202024; padding-top:16dp;\">");
            rml += std::format("<div class=\"settings-btn settings-btn-danger\" data-action=\"delete-role\" data-id=\"{0}\">Delete Role</div>", std::to_string(uint64_t(selectedRoleId)));
            rml += std::string("</div>");
        }
    } else if (currentRoleTab == RoleTab::Permissions) {
        rml += std::string("<div style=\"margin-bottom:16dp; display:flex; flex-direction:row; justify-content:space-between; align-items:center;\">");
        rml += std::format("<input type=\"text\" class=\"settings-text-input\" id=\"perm-search-input\" value=\"{0}\" placeholder=\"Search Permissions…\" style=\"flex:1; margin-right:12dp;\"/>", escape(permissionSearchQuery));
        rml += std::string("<div class=\"settings-btn settings-btn-secondary\" data-action=\"clear-perms\">Clear Permissions</div>");
        rml += std::string("</div>");

        const char *currentCategory = "";
        const auto search = Platform::caseFold(permissionSearchQuery);
        for (const auto &p : AllPermissions) {
            if (!permissionSearchQuery.empty() &&
                Platform::caseFold(p.name).find(search) == std::string::npos &&
                Platform::caseFold(p.desc).find(search) == std::string::npos)
                continue;

            if (strcmp(p.category, currentCategory) != 0) {
                currentCategory = p.category;
                rml += std::format("<div class=\"settings-subheading\">{0} Permissions</div>", currentCategory);
            }

            bool granted = (activeDraft.permissions & p.perm) == p.perm;
            rml += std::string("<div class=\"settings-switch-row\">");
            rml += std::format("<div class=\"settings-switch-text\"><div class=\"settings-switch-title\">{0}</div><div class=\"settings-switch-desc\">{1}</div></div>", p.name, p.desc);
            std::string chk = granted ? std::string(" checked") : std::string();
            rml += std::format("<div class=\"settings-switch{0}\" data-action=\"toggle-perm\" data-perm=\"{1}\"><div class=\"settings-switch-thumb\"></div></div>", chk, std::to_string(static_cast<uint64_t>(p.perm)));
            rml += std::string("</div>");
        }
    } else {
        rml += std::string("<div class=\"empty-state\"><div class=\"empty-state-title\">Role Members</div><div class=\"empty-state-desc\">Assigned to members from the Members tab.</div></div>");
    }

    rml += std::string("</div>"); // role-editor-pane
    rml += std::string("</div>"); // roles-container
}

// 8. Invites
void GuildSettingsController::renderInvites(std::string &rml)
{
    rml += std::string("<div class=\"settings-section-title\" style=\"display:flex; flex-direction:row; justify-content:space-between; align-items:center;\">");
    rml += std::format("<span>Active Invites ({0})</span>", std::to_string(invites.size()));
    rml += std::string("</div>");

    if (invites.empty()) {
        rml += std::string("<div class=\"empty-state\"><div class=\"empty-state-title\">No Active Invites</div><div class=\"empty-state-desc\">No one has created an instant invite for this server yet.</div></div>");
        return;
    }

    rml += std::string("<div class=\"guild-table\">");
    rml += std::string("<div class=\"guild-table-header\"><div class=\"col-name\">Inviter</div><div class=\"col-name\">Invite Code</div><div class=\"col-channel\">Channel</div><div class=\"col-uses\">Uses</div><div class=\"col-expires\">Expires</div><div class=\"col-actions\">Actions</div></div>");

    for (const auto &inv : invites) {
        rml += std::string("<div class=\"guild-table-row\">");

        std::string inviterName = inv.inviter.hasValue() ? inv.inviter->username.valueOr("Unknown") : "System";
        rml += std::format("<div class=\"col-name\" style=\"font-weight:600; color:#f2f3f5;\">{0}</div>", escape(inviterName));
        rml += std::format("<div class=\"col-name\" style=\"color:#00a8fc;\">{0}</div>", escape(inv.code.valueOr("")));
        std::string chName = inv.channel.hasValue() ? inv.channel->name.valueOr("channel") : "—";
        rml += std::format("<div class=\"col-channel\"># {0}</div>", escape(chName));
        rml += std::format("<div class=\"col-uses\">{0}</div>", std::to_string(inv.uses.valueOr(0)));

        std::string expText = "Never";
        if (inv.maxAge.valueOr(0) > 0)
            expText = std::format("{0}s", std::to_string(inv.maxAge.valueOr(0)));
        rml += std::format("<div class=\"col-expires\">{0}</div>", expText);

        rml += std::format("<div class=\"col-actions\"><div class=\"settings-btn settings-btn-danger\" style=\"padding:4dp 8dp; font-size:12dp;\" data-action=\"revoke-invite\" data-code=\"{0}\">Revoke</div></div>", escape(inv.code.valueOr("")));
        rml += std::string("</div>");
    }
    rml += std::string("</div>");
}

// 9. Audit Log
void GuildSettingsController::renderAuditLog(std::string &rml)
{
    rml += std::string("<div class=\"settings-section-title\">Server Audit Log</div>");

    // Filter bar
    rml += std::string("<div style=\"display:flex; flex-direction:row; margin-bottom:16dp;\">");
    rml += std::string("<div style=\"flex:1; margin-right:12dp;\">");
    rml += std::string("<div class=\"settings-label\">Filter by Action</div>");
    rml += std::string("<select class=\"settings-select\" id=\"select-audit-action\">");
    rml += std::format("<option value=\"-1\"{0}>All Actions</option>", auditFilterAction == -1 ? std::string(" selected") : std::string());
    for (const auto &filter : AuditLogFormatter::actionFilters()) {
        std::string sel = (auditFilterAction == static_cast<int>(filter.first)) ? std::string(" selected") : std::string();
        rml += std::format("<option value=\"{0}\"{1}>{2}</option>", std::to_string(static_cast<int>(filter.first)), sel, escape(filter.second));
    }
    rml += std::string("</select>");
    rml += std::string("</div>");
    rml += std::string("</div>");

    if (auditRows.empty()) {
        rml += std::string("<div class=\"empty-state\"><div class=\"empty-state-title\">No Audit Logs</div><div class=\"empty-state-desc\">No logged administrative actions found for this server.</div></div>");
        return;
    }

    int idx = 0;
    for (const auto &row : auditRows) {
        if (auditFilterAction != -1 && static_cast<int>(row.action) != auditFilterAction) {
            idx++;
            continue;
        }

        bool isExpanded = expandedAuditRows.contains(idx);
        rml += std::format("<div class=\"audit-log-card\" data-action=\"toggle-audit-row\" data-idx=\"{0}\" style=\"cursor:pointer;\">", std::to_string(idx));

        rml += std::string("<div class=\"audit-log-header\">");
        rml += std::format("<div class=\"audit-log-title\">{0}</div>", row.titleHtml);
        rml += std::format("<div class=\"audit-log-time\">{0}</div>", AuditLogFormatter::timeText(row.start, row.end));
        rml += std::string("</div>");

        if (isExpanded && !row.lines.empty()) {
            rml += std::string("<div class=\"audit-log-details\">");
            for (const auto &line : row.lines) {
                rml += std::format("<div class=\"audit-change-line\">{0}</div>", line.html);
            }
            rml += std::string("</div>");
        }

        rml += std::string("</div>");
        idx++;
    }
}

// 10. Bans
void GuildSettingsController::renderBans(std::string &rml)
{
    const auto search = Platform::caseFold(banSearchQuery);
    rml += std::format("<div class=\"settings-section-title\">Bans ({0})</div>", std::to_string(bans.size()));

    rml += std::string("<div style=\"margin-bottom:16dp;\">");
    rml += std::format("<input type=\"text\" class=\"settings-text-input\" id=\"ban-search-input\" value=\"{0}\" placeholder=\"Search bans by username or ID…\" style=\"width:100%;\"/>", escape(banSearchQuery));
    rml += std::string("</div>");

    if (bans.empty()) {
        rml += std::string("<div class=\"empty-state\"><div class=\"empty-state-title\">No Bans</div><div class=\"empty-state-desc\">No members are currently banned from this server.</div></div>");
        return;
    }

    rml += std::string("<div class=\"guild-table\">");
    rml += std::string("<div class=\"guild-table-header\"><div class=\"col-name\">User</div><div class=\"col-uploader\">Reason</div><div class=\"col-actions\">Actions</div></div>");

    for (const auto &b : bans) {
        if (!b.user.hasValue())
            continue;

        std::string userTag = b.user->username.valueOr("Banned User");
        std::string reason = b.reason.valueOr("No reason provided.");

        if (!banSearchQuery.empty() &&
            Platform::caseFold(userTag).find(search) == std::string::npos &&
            b.user->id.get().toString().find(banSearchQuery) == std::string::npos)
            continue;

        rml += std::string("<div class=\"guild-table-row\">");
        rml += std::format("<div class=\"col-name\"><span style=\"font-weight:600; color:#f2f3f5;\">{0}</span> <span style=\"color:#949ba4;\">(ID: {1})</span></div>", escape(userTag), std::to_string(uint64_t(b.user->id.get())));
        rml += std::format("<div class=\"col-uploader\">{0}</div>", escape(reason));
        rml += std::format("<div class=\"col-actions\"><div class=\"settings-btn settings-btn-secondary\" style=\"padding:4dp 8dp; font-size:12dp;\" data-action=\"revoke-ban\" data-id=\"{0}\" data-name=\"{1}\">Revoke Ban</div></div>", std::to_string(uint64_t(b.user->id.get())), escape(userTag));
        rml += std::string("</div>");
    }
    rml += std::string("</div>");
}

// 11. Delete Server
void GuildSettingsController::renderDeleteServer(std::string &rml)
{
    rml += std::string("<div class=\"settings-section-title\" style=\"color:#f23f43;\">Delete Server</div>");
    rml += std::string("<div class=\"settings-card danger-card\">");
    rml += std::string("<div style=\"font-size:15dp; font-weight:bold; color:#f23f43; margin-bottom:8dp;\">Are you sure you want to delete this server?</div>");
    rml += std::string("<div class=\"settings-description\" style=\"color:#dbdee1;\">Deleting this server is permanent and cannot be undone. All channels, messages, roles, and members will be immediately removed.</div>");
    rml += std::string("<div class=\"settings-btn settings-btn-danger\" style=\"margin-top:12dp;\" data-action=\"delete-server\">Delete Server</div>");
    rml += std::string("</div>");
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
        Platform::Guard<GuildSettingsController> self(this);
        const auto guildId = currentGuildId;
        const auto generation = requestGeneration;
        inst->discord()->fetchGuildProfile(currentGuildId, [self, guildId, generation](const Core::Result<Discord::GuildProfileEdit> &res) {
            if (!self || !self->isCurrentRequest(guildId, generation))
                return;
            if (!res.success()) { self->reportError(res.error); return; }
            self->savedProfile.name = res.value->name;
            self->savedProfile.description = res.value->description;
            self->savedGuildProfile = *res.value;
            self->savedProfile.brandColor = res.value->brandColorPrimary.is_string() ? res.value->brandColorPrimary.get<std::string>() : "#5865f2";
            self->savedProfile.isPrivate = !res.value->visibility.is_number_integer() || res.value->visibility.get<int>() != 1;
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
    Platform::Guard<GuildSettingsController> self(this);
    const auto guildId = currentGuildId;
    const auto generation = requestGeneration;
    inst->discord()->fetchGuildEmojis(currentGuildId, [self, guildId, generation](const Core::Result<std::vector<Discord::Emoji>> &res) {
        if (!self || !self->isCurrentRequest(guildId, generation))
            return;
        if (!res.success()) { self->reportError(res.error); return; }
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
    Platform::Guard<GuildSettingsController> self(this);
    const auto guildId = currentGuildId;
    const auto generation = requestGeneration;
    inst->discord()->fetchGuildStickers(currentGuildId, [self, guildId, generation](const Core::Result<std::vector<Discord::Sticker>> &res) {
        if (!self || !self->isCurrentRequest(guildId, generation))
            return;
        if (!res.success()) { self->reportError(res.error); return; }
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
    Platform::Guard<GuildSettingsController> self(this);
    const auto guildId = currentGuildId;
    const auto generation = requestGeneration;
    inst->discord()->searchGuildMembers(currentGuildId, query, [self, guildId, generation](const Core::Result<Discord::Client::MemberSearchPage> &res) {
        if (!self || !self->isCurrentRequest(guildId, generation))
            return;
        if (!res.success()) { self->reportError(res.error); return; }
        self->membersLoaded = true;
        self->members.clear();
        for (const auto &item : res.value->members) {
            if (item.member.hasValue())
                self->members.push_back(item.member.get());
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

    Platform::Guard<GuildSettingsController> self(this);

    const auto guildId = currentGuildId;

    const auto generation = requestGeneration;
    inst->discord()->fetchGuildInvites(currentGuildId, [self, guildId, generation](const Core::Result<std::vector<Discord::Invite>> &res) {
        if (!self || !self->isCurrentRequest(guildId, generation))
            return;
        if (!res.success()) { self->reportError(res.error); return; }
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
    Platform::Guard<GuildSettingsController> self(this);
    const auto guildId = currentGuildId;
    const auto generation = requestGeneration;
    inst->discord()->fetchAuditLog(currentGuildId, query, [self, guildId, generation](const Core::Result<Discord::AuditLog> &res) {
        if (!self || !self->isCurrentRequest(guildId, generation))
            return;
        if (!res.success()) { self->reportError(res.error); return; }
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

    Platform::Guard<GuildSettingsController> self(this);

    const auto guildId = currentGuildId;

    const auto generation = requestGeneration;
    inst->discord()->fetchBans(currentGuildId, Core::Snowflake::Invalid, [self, guildId, generation](const Core::Result<std::vector<Discord::Ban>> &res) {
        if (!self || !self->isCurrentRequest(guildId, generation))
            return;
        if (!res.success()) { self->reportError(res.error); return; }
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
            !draftProfile.iconDataUri.empty() ||
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
                !draftBoost.bannerUri.empty() ||
                draftBoost.bannerRemoved != savedBoost.bannerRemoved ||
                !draftBoost.splashUri.empty() ||
                draftBoost.splashRemoved != savedBoost.splashRemoved);

    case GuildSettingsSection::Roles:
        return !dirtyRoleDrafts.empty();

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
        reportError(std::string("You do not have permission to change these settings."));
        return;
    }
    auto *inst = instance();
    if (!inst || !inst->discord())
        return;
    const auto guildId = currentGuildId;
    const auto generation = requestGeneration;
    Platform::Guard<GuildSettingsController> self(this);
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
        edit.icon = draftProfile.iconRemoved ? nlohmann::json(nullptr)
                                             : (draftProfile.iconDataUri.empty() ? savedGuildProfile.icon : nlohmann::json(draftProfile.iconDataUri));
        edit.brandColorPrimary = nlohmann::json(draftProfile.brandColor);
        edit.visibility = nlohmann::json(draftProfile.isPrivate ? 2 : 1);
        edit.traits.clear();
        for (int i = 0; i < Discord::GuildProfileEdit::TraitSlots; ++i)
            edit.traits.push_back({ draftProfile.traits[i], nlohmann::json{}, nlohmann::json{}, nlohmann::json{} });
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
            edit.banner = draftBoost.bannerRemoved ? std::string() : draftBoost.bannerUri;
            edit.splash = draftBoost.splashRemoved ? std::string() : draftBoost.splashUri;
        }
        inst->discord()->modifyGuild(guildId, edit, {}, complete);
    } else if (currentSection == GuildSettingsSection::Roles) {
        for (const auto &[roleId, draft] : dirtyRoleDrafts) {
            Discord::RoleEdit edit;
            edit.name = draft.name;
            edit.color = draft.color;
            edit.hoist = draft.hoist;
            edit.mentionable = draft.mentionable;
            edit.permissions = draft.permissions;
            inst->discord()->modifyRole(guildId, roleId, edit, complete);
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

    std::string action = attribute(actionEl, "data-action");

    if (action != "close" && action != "nav-section" &&
        action != "toggle-audit-row" && !canEditCurrentSection()) {
        reportError(std::string("You do not have permission to change these settings."));
        return;
    }

    if (action == "close") {
        event.StopPropagation();
        close();
    } else if (action == "nav-section") {
        event.StopPropagation();
        bool ok = false;
        int secInt = number<int>(attribute(actionEl, "data-section"), &ok);
        if (ok)
            setSection(static_cast<GuildSettingsSection>(secInt));
    } else if (action == "reset-changes") {
        event.StopPropagation();
        discardChanges();
        renderContent();
        requestRender(100);
    } else if (action == "save-changes") {
        event.StopPropagation();
        saveChanges();
    }
    // Profile Actions
    else if (action == "choose-icon") {
        event.StopPropagation();
        chooseImage(ImageSlot::Icon);
    } else if (action == "remove-icon") {
        event.StopPropagation();
        draftProfile.iconDataUri.clear();
        draftProfile.iconRemoved = true;
        renderContent();
        updateUnsavedChangesBar();
    } else if (action == "select-brand-color") {
        event.StopPropagation();
        draftProfile.brandColor = attribute(actionEl, "data-color");
        renderContent();
        updateUnsavedChangesBar();
    } else if (action == "toggle-private") {
        event.StopPropagation();
        draftProfile.isPrivate = !draftProfile.isPrivate;
        renderContent();
        updateUnsavedChangesBar();
    }
    // Engagement Actions
    else if (action == "toggle-flag-join") {
        event.StopPropagation();
        draftEngagement.suppressJoin = !draftEngagement.suppressJoin;
        renderContent();
        updateUnsavedChangesBar();
    } else if (action == "toggle-flag-boost") {
        event.StopPropagation();
        draftEngagement.suppressBoost = !draftEngagement.suppressBoost;
        renderContent();
        updateUnsavedChangesBar();
    } else if (action == "toggle-flag-tips") {
        event.StopPropagation();
        draftEngagement.suppressTips = !draftEngagement.suppressTips;
        renderContent();
        updateUnsavedChangesBar();
    } else if (action == "toggle-flag-prompt") {
        event.StopPropagation();
        draftEngagement.suppressPrompt = !draftEngagement.suppressPrompt;
        renderContent();
        updateUnsavedChangesBar();
    } else if (action == "select-notif-all") {
        event.StopPropagation();
        draftEngagement.defaultNotifications = Discord::MessageNotificationLevel::ALL_MESSAGES;
        renderContent();
        updateUnsavedChangesBar();
    } else if (action == "select-notif-mentions") {
        event.StopPropagation();
        draftEngagement.defaultNotifications = Discord::MessageNotificationLevel::ONLY_MENTIONS;
        renderContent();
        updateUnsavedChangesBar();
    }
    // Boost Actions
    else if (action == "toggle-progress-bar") {
        event.StopPropagation();
        draftBoost.progressBar = !draftBoost.progressBar;
        renderContent();
        updateUnsavedChangesBar();
    } else if (action == "choose-banner") {
        event.StopPropagation();
        chooseImage(ImageSlot::Banner);
    } else if (action == "remove-banner") {
        event.StopPropagation();
        draftBoost.bannerUri.clear();
        draftBoost.bannerRemoved = true;
        renderContent();
        updateUnsavedChangesBar();
    } else if (action == "choose-splash") {
        event.StopPropagation();
        chooseImage(ImageSlot::Splash);
    } else if (action == "remove-splash") {
        event.StopPropagation();
        draftBoost.splashUri.clear();
        draftBoost.splashRemoved = true;
        renderContent();
        updateUnsavedChangesBar();
    }
    // Emoji & Stickers
    else if (action == "choose-emoji-upload") {
        event.StopPropagation();
        uploadExpression(false);
    } else if (action == "delete-emoji") {
        event.StopPropagation();
        const auto emojiId = Core::Snowflake(number(attribute(actionEl, "data-id")));
        showConfirmModal(shell, std::string("Delete Emoji"), std::string("Are you sure you want to delete this emoji?"), std::string("Delete"),
            [this, emojiId]() { if (auto *inst = instance()) inst->discord()->deleteGuildEmoji(currentGuildId, emojiId, [this](const Core::Result<void> &r) { if (!r.success()) reportError(r.error); else loadEmoji(); }); }, nullptr, true);
    } else if (action == "choose-sticker-upload") {
        event.StopPropagation();
        uploadExpression(true);
    } else if (action == "delete-sticker") {
        event.StopPropagation();
        const auto stickerId = Core::Snowflake(number(attribute(actionEl, "data-id")));
        showConfirmModal(shell, std::string("Delete Sticker"), std::string("Are you sure you want to delete this sticker?"), std::string("Delete"),
            [this, stickerId]() { if (auto *inst = instance()) inst->discord()->deleteGuildSticker(currentGuildId, stickerId, [this](const Core::Result<void> &r) { if (!r.success()) reportError(r.error); else loadStickers(); }); }, nullptr, true);
    }
    // Members Actions
    else if (action == "kick-member" || action == "ban-member") {
        event.StopPropagation();
        const bool ban = action == "ban-member";
        const auto userId = Core::Snowflake(number(attribute(actionEl, "data-id")));
        const std::string name = attribute(actionEl, "data-name");
        showConfirmModal(shell, ban ? std::string("Ban Member") : std::string("Kick Member"),
            ban ? std::format("Are you sure you want to ban <b>@{0}</b> from the server?", name) : std::format("Are you sure you want to kick <b>@{0}</b> from the server?", name),
            ban ? std::string("Ban") : std::string("Kick"), [this, ban, userId]() {
                if (auto *inst = instance()) {
                    auto done = [this](const Core::Result<void> &r) { if (!r.success()) reportError(r.error); else loadMembers(); };
                    if (ban) inst->discord()->banMember(currentGuildId, userId, 0, {}, done);
                    else inst->discord()->kickMember(currentGuildId, userId, {}, done);
                }
            }, nullptr, true);
    } else if (action == "prune-members") {
        event.StopPropagation();
        showConfirmModal(shell, std::string("Prune Members"), std::string("Pruning will kick members who have not been seen in over 7 days and have no roles. Continue?"), std::string("Prune"),
            [this]() { if (auto *inst = instance()) inst->discord()->pruneMembers(currentGuildId, 7, {}, [this](const Core::Result<void> &r) { if (!r.success()) reportError(r.error); else loadMembers(); }); }, nullptr, true);
    }
    // Roles Actions
    else if (action == "select-role") {
        event.StopPropagation();
        selectedRoleId = Core::Snowflake(number(attribute(actionEl, "data-id")));
        renderContent();
    } else if (action == "select-role-tab") {
        event.StopPropagation();
        currentRoleTab = static_cast<RoleTab>(number<int>(attribute(actionEl, "data-tab")));
        renderContent();
    } else if (action == "create-role") {
        event.StopPropagation();
        if (auto *inst = instance())
            inst->discord()->createRole(currentGuildId, [this](const Core::Result<Discord::Role> &r) { if (!r.success()) reportError(r.error); else { roles = instance()->getRolesForGuild(currentGuildId); selectedRoleId = r.value->id.get(); render(); } });
    } else if (action == "delete-role") {
        event.StopPropagation();
        const auto roleId = selectedRoleId;
        showConfirmModal(shell, std::string("Delete Role"), std::string("Are you sure you want to delete this role? This action cannot be undone."), std::string("Delete"),
            [this, roleId]() { if (auto *inst = instance()) inst->discord()->deleteRole(currentGuildId, roleId, [this](const Core::Result<void> &r) { if (!r.success()) reportError(r.error); else { loadRoles(); render(); } }); }, nullptr, true);
    } else if (action == "select-role-color") {
        event.StopPropagation();
        int c = number<int>(attribute(actionEl, "data-color"));
        RoleDraft d = dirtyRoleDrafts.contains(selectedRoleId) ? dirtyRoleDrafts[selectedRoleId] : savedRoleDrafts[selectedRoleId];
        d.color = c;
        dirtyRoleDrafts[selectedRoleId] = d;
        renderContent();
        updateUnsavedChangesBar();
    } else if (action == "toggle-role-hoist") {
        event.StopPropagation();
        RoleDraft d = dirtyRoleDrafts.contains(selectedRoleId) ? dirtyRoleDrafts[selectedRoleId] : savedRoleDrafts[selectedRoleId];
        d.hoist = !d.hoist;
        dirtyRoleDrafts[selectedRoleId] = d;
        renderContent();
        updateUnsavedChangesBar();
    } else if (action == "toggle-role-mention") {
        event.StopPropagation();
        RoleDraft d = dirtyRoleDrafts.contains(selectedRoleId) ? dirtyRoleDrafts[selectedRoleId] : savedRoleDrafts[selectedRoleId];
        d.mentionable = !d.mentionable;
        dirtyRoleDrafts[selectedRoleId] = d;
        renderContent();
        updateUnsavedChangesBar();
    } else if (action == "toggle-perm") {
        event.StopPropagation();
        uint64_t pVal = number(attribute(actionEl, "data-perm"));
        Discord::Permission perm = static_cast<Discord::Permission>(pVal);
        RoleDraft d = dirtyRoleDrafts.contains(selectedRoleId) ? dirtyRoleDrafts[selectedRoleId] : savedRoleDrafts[selectedRoleId];
        d.permissions.setFlag(perm, !d.permissions.testFlag(perm));
        dirtyRoleDrafts[selectedRoleId] = d;
        renderContent();
        updateUnsavedChangesBar();
    } else if (action == "clear-perms") {
        event.StopPropagation();
        RoleDraft d = dirtyRoleDrafts.contains(selectedRoleId) ? dirtyRoleDrafts[selectedRoleId] : savedRoleDrafts[selectedRoleId];
        d.permissions = Discord::NO_PERMISSIONS;
        dirtyRoleDrafts[selectedRoleId] = d;
        renderContent();
        updateUnsavedChangesBar();
    }
    // Invites
    else if (action == "revoke-invite") {
        event.StopPropagation();
        const std::string code = attribute(actionEl, "data-code");
        showConfirmModal(shell, std::string("Revoke Invite"), std::format("Are you sure you want to revoke invite <b>{0}</b>?", code), std::string("Revoke"),
            [this, code]() { if (auto *inst = instance()) inst->discord()->revokeInvite(code, [this](const Core::Result<void> &r) { if (!r.success()) reportError(r.error); else loadInvites(); }); }, nullptr, true);
    }
    // Audit Log
    else if (action == "toggle-audit-row") {
        event.StopPropagation();
        int idx = number<int>(attribute(actionEl, "data-idx"));
        if (expandedAuditRows.contains(idx))
            expandedAuditRows.erase(idx);
        else
            expandedAuditRows.insert(idx);
        renderContent();
    }
    // Bans
    else if (action == "revoke-ban") {
        event.StopPropagation();
        const auto userId = Core::Snowflake(number(attribute(actionEl, "data-id")));
        const std::string name = attribute(actionEl, "data-name");
        showConfirmModal(shell, std::string("Revoke Ban"), std::format("Are you sure you want to revoke the ban for <b>{0}</b>?", name), std::string("Revoke Ban"),
            [this, userId]() { if (auto *inst = instance()) inst->discord()->unbanMember(currentGuildId, userId, [this](const Core::Result<void> &r) { if (!r.success()) reportError(r.error); else loadBans(); }); }, nullptr, false);
    }
    // Delete Server
    else if (action == "delete-server") {
        event.StopPropagation();
        const std::string gName = guild() ? guild()->name.valueOr("this server") : std::string("this server");
        showConfirmModal(shell, std::format("Delete '{0}'", gName), std::format("Are you sure you want to delete <b>{0}</b>? This action cannot be undone.", gName), std::string("Delete Server"),
            [this]() { if (auto *inst = instance()) inst->discord()->deleteGuild(currentGuildId, [this](const Core::Result<void> &r) { if (!r.success()) reportError(r.error); else close(); }); }, nullptr, true);
    }
}

void GuildSettingsController::handleInput(::Rml::Element *target, ::Rml::Event &event)
{
    if (!target)
        return;

    std::string id = std::string(target->GetId());

    if (id == "profile-name-input") {
        auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(target);
        if (input) {
            draftProfile.name = std::string(input->GetValue());
            updateUnsavedChangesBar();
        }
    } else if (id == "profile-desc-input") {
        if (auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(target))
            draftProfile.description = std::string(input->GetValue());
        updateUnsavedChangesBar();
    } else if (id.starts_with("trait-input-")) {
        auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(target);
        if (input) {
            int idx = number<int>(attribute(target, "data-trait-idx"));
            if (idx >= 0 && idx < 6) {
                draftProfile.traits[idx] = std::string(input->GetValue());
                updateUnsavedChangesBar();
            }
        }
    } else if (id == "member-search-input") {
        auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(target);
        if (input) {
            memberSearchQuery = std::string(input->GetValue());
            renderContent();
        }
    } else if (id == "perm-search-input") {
        auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(target);
        if (input) {
            permissionSearchQuery = std::string(input->GetValue());
            renderContent();
        }
    } else if (id == "ban-search-input") {
        auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(target);
        if (input) {
            banSearchQuery = std::string(input->GetValue());
            renderContent();
        }
    } else if (id == "role-name-input") {
        auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(target);
        if (input) {
            RoleDraft d = dirtyRoleDrafts.contains(selectedRoleId) ? dirtyRoleDrafts[selectedRoleId] : savedRoleDrafts[selectedRoleId];
            d.name = std::string(input->GetValue());
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

    std::string id = std::string(target->GetId());
    std::string val = std::string(select->GetValue());

    if (id == "select-system-channel") {
        draftEngagement.systemChannelId = Core::Snowflake(number(val));
        updateUnsavedChangesBar();
    } else if (id == "select-afk-channel") {
        draftEngagement.afkChannelId = Core::Snowflake(number(val));
        updateUnsavedChangesBar();
    } else if (id == "select-afk-timeout") {
        draftEngagement.afkTimeout = number<int>(val);
        updateUnsavedChangesBar();
    } else if (id == "select-audit-action") {
        auditFilterAction = number<int>(val);
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

void GuildSettingsController::chooseImage(ImageSlot slot)
{
    const char *title = slot == ImageSlot::Icon ? "Change Server Icon" : slot == ImageSlot::Banner ? "Upload Banner" : "Upload Splash";
    const Platform::Size bounds = slot == ImageSlot::Icon ? Platform::Size{1024, 1024} : slot == ImageSlot::Banner ? Platform::Size{960, 540} : Platform::Size{1920, 1080};
    Platform::Guard<GuildSettingsController> self(this);
    const auto guildId = currentGuildId;
    const auto generation = requestGeneration;
    Platform::Desktop::openFiles(title, {{"Images", "*.png;*.jpg;*.jpeg;*.gif;*.webp;*.avif;*.bmp;*.svg"}}, [self, guildId, generation, slot, bounds](std::vector<std::string> files) {
        if (!self || files.empty() || !self->isCurrentRequest(guildId, generation)) return;
        auto prepared = ImageUpload::prepareCropped(files.front(), bounds.width, bounds.height);
        if (prepared.dataUri.empty()) { self->reportError(prepared.error); return; }
        if (slot == ImageSlot::Icon) { self->draftProfile.iconDataUri = std::move(prepared.dataUri); self->draftProfile.iconRemoved = false; }
        else if (slot == ImageSlot::Banner) { self->draftBoost.bannerUri = std::move(prepared.dataUri); self->draftBoost.bannerRemoved = false; }
        else { self->draftBoost.splashUri = std::move(prepared.dataUri); self->draftBoost.splashRemoved = false; }
        self->renderContent();
        self->updateUnsavedChangesBar();
        self->requestRender();
    }, false);
}

void GuildSettingsController::uploadExpression(bool sticker)
{
    Platform::Guard<GuildSettingsController> self(this);
    const auto guildId = currentGuildId;
    const auto generation = requestGeneration;
    Platform::Desktop::openFiles(sticker ? "Upload Sticker" : "Upload Emoji", {{"Images", "*.png;*.apng;*.gif;*.jpg;*.jpeg;*.webp;*.avif;*.json"}}, [self, guildId, generation, sticker](std::vector<std::string> files) {
        if (!self || files.empty() || !self->isCurrentRequest(guildId, generation)) return;
        auto *instance = self->instance();
        if (!instance) return;
        const auto prepared = ImageUpload::prepareRaw(files.front(), (sticker ? 512 : 256) * 1024);
        if (prepared.dataUri.empty()) { self->reportError(prepared.error); return; }
        const auto path = Platform::utf8Path(files.front());
        const auto name = Platform::pathUtf8(path.stem());
        auto complete = [self, guildId, generation, sticker](const auto &result) {
            if (!self || !self->isCurrentRequest(guildId, generation)) return;
            if (!result.success()) { self->reportError(result.error); return; }
            if (sticker) self->loadStickers(); else self->loadEmoji();
        };
        if (!sticker) {
            instance->discord()->createGuildEmoji(guildId, name, prepared.dataUri, prepared.originalMd5, complete);
        } else {
            const auto comma = prepared.dataUri.find(',');
            const auto semicolon = prepared.dataUri.find(';');
            const auto bytes = Platform::fromBase64(std::string_view(prepared.dataUri).substr(comma + 1));
            if (!bytes) { self->reportError("Unable to process sticker"); return; }
            Discord::FileUpload file{Platform::pathUtf8(path.filename()), std::string(reinterpret_cast<const char *>(bytes->data()), bytes->size()), prepared.dataUri.substr(5, semicolon - 5), {}};
            instance->discord()->createGuildSticker(guildId, name, "sticker", {}, file, prepared.originalMd5, complete);
        }
    }, false);
}

} // namespace Rml
} // namespace UI
} // namespace Styx
