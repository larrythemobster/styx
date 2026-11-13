#include "SettingsController.hpp"

#include "ConfirmModal.hpp"
#include "ProfileEditorController.hpp"
#include "Shell.hpp"
#include "Core/ClientInstance.hpp"
#include "Discord/CdnUrls.hpp"
#include "Core/SettingsKeys.hpp"
#include "Core/Theme/Fonts.hpp"
#include "Core/Theme/Manager.hpp"
#include "Core/Theme/Scale.hpp"
#include "Platform/Desktop.hpp"
#include "Proto/PreloadedSettings.hpp"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <utility>

namespace Styx::UI::Rml {
using namespace Core::Settings;
namespace {
struct SectionInfo {
    SettingsController::Section section;
    const char *id;
    const char *label;
    const char *group;
    const char *keywords;
    const char *icon;
    // Profiles opens from the account summary, as in Discord, and is listed only in search results.
    bool listed = true;
};
// Discord's sidebar order. Billing, Data & Privacy, Registered Games, Game Overlay, Connected Apps and
// Developer have no Styx page yet, so they are not listed.
constexpr SectionInfo sections[] = {
    {SettingsController::Section::MyAccount, "my-account", "Account", "", "account username email security authentication mfa", "id-card"},
    {SettingsController::Section::Profiles, "profiles", "Profiles", "", "profile avatar banner display name pronouns about me bio", "image", false},
    {SettingsController::Section::MessagingPermissions, "messaging-permissions", "Messaging Permissions", "",
        "privacy direct messages dms server members screen reader detection friends requests everyone mutual", "message-circle"},
    {SettingsController::Section::Notifications, "notifications", "Notifications", "", "notification sounds taskbar dot badge flash alerts", "bell"},
    {SettingsController::Section::VoiceVideo, "voice-video", "Voice & Video", "Experience",
        "voice audio microphone speakers input output volume push to talk devices screen sharing streaming quality resolution frame rate", "mic"},
    {SettingsController::Section::Appearance, "appearance", "Appearance", "Experience",
        "theme default light ash dark onyx device sync accent color colour picker palette custom messages embeds links reactions", "palette"},
    {SettingsController::Section::Accessibility, "accessibility", "Accessibility", "Experience",
        "font text size chat zoom scale compact avatars underlines readability density reduced motion", "accessibility"},
    {SettingsController::Section::System, "system", "System", "Experience",
        "system windows startup login minimized tray keybinds shortcuts mute deafen advanced developer mode copy id cache memory", "keyboard"},
    {SettingsController::Section::Language, "language", "Language & Time", "Experience", "language locale english", "languages"},
    {SettingsController::Section::ActivityPrivacy, "activity-privacy", "Activity Privacy", "Games & Apps", "activity presence share games privacy", "chart-column"},
};
struct SubsectionInfo {
    SettingsController::Section section;
    const char *id;
    const char *label;
};
constexpr SubsectionInfo subsections[] = {
    {SettingsController::Section::MessagingPermissions, "direct-messages", "Direct Messages"},
    {SettingsController::Section::MessagingPermissions, "friend-requests", "Friend Requests"},
    {SettingsController::Section::Notifications, "overview", "Overview"},
    {SettingsController::Section::Notifications, "sounds", "Sounds"},
    {SettingsController::Section::VoiceVideo, "voice", "Voice"},
    {SettingsController::Section::VoiceVideo, "streaming", "Streaming"},
    {SettingsController::Section::Appearance, "theme", "Theme"},
    {SettingsController::Section::Appearance, "custom-theme", "Styx Custom Theme"},
    {SettingsController::Section::Appearance, "messages", "Messages"},
    {SettingsController::Section::Accessibility, "text-readability", "Text Readability"},
    {SettingsController::Section::Accessibility, "visual-density", "Visual Density"},
    {SettingsController::Section::Accessibility, "reduced-motion", "Reduced Motion"},
    {SettingsController::Section::System, "general", "General"},
    {SettingsController::Section::System, "custom-keybinds", "Custom Keybinds"},
    {SettingsController::Section::System, "advanced", "Advanced"},
};
std::string lower(std::string text)
{
    for (auto &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
void description(std::string &rml, std::string_view text)
{
    rml += "<div class=\"settings-description\">" + Platform::htmlEscape(text) + "</div>";
}
}

Discord::Client *SettingsController::readyClient(Shell &shell)
{
    auto *instance = shell.currentInstance();
    return instance && instance->discord()->getMe().id.get() == instance->accountId() ? instance->discord() : nullptr;
}

void SettingsController::renderNavigation(std::string &rml)
{
    const auto query = lower(searchQuery);
    auto matches = [&query](std::string_view text) { return lower(std::string(text)).find(query) != std::string::npos; };
    std::string_view group;
    bool found = false;
    for (const auto &entry : sections) {
        std::vector<const SubsectionInfo *> links;
        for (const auto &sub : subsections)
            if (sub.section == entry.section && (query.empty() ? entry.section == currentSection : matches(sub.label)))
                links.push_back(&sub);
        const bool sectionMatch = query.empty() ? entry.listed : matches(entry.keywords) || matches(entry.label);
        if (!sectionMatch && links.empty())
            continue;
        if (entry.group != group) {
            if (found) rml += "<div class=\"settings-divider\"></div>";
            rml += "<div class=\"settings-group-header\">" + std::string(entry.group) + "</div>";
            group = entry.group;
        }
        rml += std::format("<div class=\"settings-nav-item{}\" data-section=\"{}\" role=\"button\" tabindex=\"0\">"
            "<img src=\"icon:{}\"/><span>{}</span></div>",
            currentSection == entry.section ? " active" : "", entry.id, entry.icon, Platform::htmlEscape(entry.label));
        if (!links.empty()) {
            rml += "<div class=\"settings-nav-subsections\">";
            for (const auto *sub : links) {
                const bool active = query.empty() && (currentSubsection.empty() ? sub == links.front() : currentSubsection == sub->id);
                rml += std::format("<div class=\"settings-nav-subitem{}\" data-section=\"{}\" data-subsection=\"{}\" role=\"button\" "
                    "tabindex=\"0\"><span>{}</span></div>", active ? " active" : "", entry.id, sub->id, Platform::htmlEscape(sub->label));
            }
            rml += "</div>";
        }
        found = true;
    }
    if (!found) description(rml, "No settings match your search.");
}

void SettingsController::appendSubsectionHeading(std::string &rml, std::string_view id)
{
    for (const auto &sub : subsections)
        if (id == sub.id)
            rml += std::format("<div class=\"settings-section-heading\" id=\"settings-sub-{}\" data-subsection-anchor=\"{}\">{}</div>",
                sub.id, sub.id, Platform::htmlEscape(sub.label));
}

void SettingsController::openSubsection(const std::string &sectionId, const std::string &subsectionId)
{
    // A search result link leaves search, like choosing a section does after searching.
    searchQuery.clear();
    if (auto *input = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(regionRoot->GetElementById("settings-search")))
        input->SetValue("");
    selectSection(sectionId);
    for (const auto &entry : sections)
        if (sectionId == entry.id && entry.section != currentSection)
            return; // Leaving the profile editor was declined.
    currentSubsection = subsectionId;
    refreshNavigation();
    // Scroll once the page's new layout exists.
    subsectionTimer.start(std::chrono::milliseconds(0), false, [this, subsectionId] {
        if (auto *anchor = regionRoot ? regionRoot->GetElementById("settings-sub-" + subsectionId) : nullptr) {
            anchor->ScrollIntoView(true);
            requestRender();
        }
    });
}

void SettingsController::trackVisibleSubsection()
{
    auto *scroll = regionRoot ? regionRoot->QuerySelector(".settings-content-scroll") : nullptr;
    if (!scroll || !searchQuery.empty())
        return;
    ::Rml::ElementList anchors;
    scroll->QuerySelectorAll(anchors, "[data-subsection-anchor]");
    std::string visible;
    // The last heading at or above the top of the visible page owns the reading position.
    for (auto *anchor : anchors)
        if (visible.empty() || anchor->GetAbsoluteTop() <= scroll->GetAbsoluteTop() + 32)
            visible = attribute(anchor, "data-subsection-anchor");
    // Headings near the end of a page cannot reach the top. At the bottom, a chosen heading that is
    // on screen stays selected instead of reverting to the one above it.
    const bool atBottom = scroll->GetScrollTop() + scroll->GetClientHeight() >= scroll->GetScrollHeight() - 1;
    if (atBottom && !currentSubsection.empty())
        if (auto *chosen = scroll->GetElementById("settings-sub-" + currentSubsection);
            chosen && chosen->GetAbsoluteTop() < scroll->GetAbsoluteTop() + scroll->GetClientHeight())
            visible = currentSubsection;
    if (visible == currentSubsection)
        return;
    currentSubsection = visible;
    ::Rml::ElementList links;
    regionRoot->QuerySelectorAll(links, ".settings-nav-subitem");
    for (auto *link : links)
        link->SetClass("active", attribute(link, "data-subsection") == currentSubsection);
    requestRender();
}

void SettingsController::refreshNavigation()
{
    if (auto *wrap = regionRoot->GetElementById("settings-search-wrap"))
        wrap->SetClass("has-query", !searchQuery.empty());
    if (auto *navigation = regionRoot->GetElementById("settings-navigation")) {
        std::string rml;
        renderNavigation(rml);
        navigation->SetInnerRML(rml);
    }
    requestRender();
}

void SettingsController::selectSection(const std::string &id)
{
    for (const auto &entry : sections)
        if (id == entry.id) { setSection(entry.section); return; }
}

std::string_view SettingsController::sectionTitle() const
{
    for (const auto &entry : sections)
        if (entry.section == currentSection) return entry.label;
    return {};
}

void SettingsController::appendSwitch(std::string &rml, const std::string &key, bool checked,
    const std::string &title, const std::string &detail, bool disabled)
{
    std::string id = "preference-" + key;
    for (auto &c : id)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') c = '-';
    rml += "<div class=\"settings-switch-row\"><div class=\"settings-switch-text\">";
    rml += "<div class=\"settings-switch-title\">" + Platform::htmlEscape(title) + "</div>";
    rml += "<div class=\"settings-switch-desc\">" + Platform::htmlEscape(detail) + "</div></div>";
    rml += std::format("<div id=\"{}\" class=\"settings-switch{}{}\" data-setting=\"{}\" role=\"switch\" "
        "aria-label=\"{}\" aria-checked=\"{}\" {}><div class=\"settings-switch-thumb\"></div></div></div>",
        id, checked ? " checked" : "", disabled ? " disabled" : "", Platform::htmlEscape(key),
        Platform::htmlEscape(title), checked ? "true" : "false", disabled ? "disabled=\"disabled\"" : "tabindex=\"0\"");
}

bool SettingsController::confirmProfileLeave(std::function<void()> proceed)
{
    if (!profileEditor) return true;
    if (profileEditor->isSaving()) return false;
    if (!profileEditor->hasPendingChanges()) return true;
    Platform::Guard<SettingsController> self(this);
    ConfirmOptions options;
    options.title = "Discard profile changes?";
    options.body = "Your changes have not been saved.";
    options.confirmText = "Discard changes";
    options.cancelText = "Keep editing";
    options.onConfirm = [self, proceed = std::move(proceed)] {
        if (!self) return;
        if (self->profileEditor) self->profileEditor->reset();
        proceed();
    };
    showConfirmModal(shell, options);
    return false;
}

void SettingsController::renderProfiles(std::string &rml)
{
    description(rml, "Update your Discord profile. Changes are saved to your account when you choose Save Changes.");
    rml += "<div id=\"settings-profile-editor\" class=\"settings-profile-editor\"></div>";
}

void SettingsController::renderRemoteStatus(std::string &rml)
{
    if (remoteSettingsSaving) description(rml, "Saving to Discord...");
    if (!remoteSettingsError.empty())
        rml += "<div class=\"settings-keybind-error\" role=\"alert\">" + Platform::htmlEscape(remoteSettingsError) + "</div>";
}
bool SettingsController::isRemoteSection() const
{
    return currentSection == Section::MessagingPermissions || currentSection == Section::Appearance || currentSection == Section::System ||
        currentSection == Section::ActivityPrivacy || currentSection == Section::Language;
}

void SettingsController::bindAccountSettings()
{
    accountSettingsConnection.disconnect();
    auto *instance = shell.currentInstance();
    if (!instance) return;
    const auto account = instance->accountId();
    accountSettingsConnection = instance->discord()->settingsChanged.connect([this, account] {
        if (!shell.currentInstance() || shell.currentInstance()->accountId() != account) return;
        adoptAccountTheme();
        if (shell.isSettingsOpen() && isRemoteSection() && !remoteSettingsSaving)
            rebuildTimer.start(std::chrono::milliseconds(0), false, [this] { rebuild(); });
    });
    accountSettingsConnection.setAutoDisconnect(true);
}


void SettingsController::patchRemoteSettings(Proto::PreloadedSettingsEdit edit)
{
    auto *client = readyClient(shell);
    if (!client || remoteSettingsSaving) return;
    const auto account = shell.currentInstance()->accountId();
    const auto request = ++remoteSettingsRequest;
    remoteSettingsSaving = true;
    remoteSettingsError.clear();
    render();
    Platform::Guard<SettingsController> self(this);
    client->modifyPreloadedSettings(std::move(edit), [self, account, request](const Core::Result<void> &result) {
        if (!self || self->remoteSettingsRequest != request || !self->shell.currentInstance() ||
            self->shell.currentInstance()->accountId() != account) return;
        self->remoteSettingsSaving = false;
        self->remoteSettingsError = result.success() ? "" : result.error.empty() ? "Could not save to Discord. Try again." : result.error;
        if (self->shell.isSettingsOpen() && self->isRemoteSection()) self->render();
        self->requestRender();
    });
}

bool SettingsController::handleRemoteSwitch(::Rml::Element *element)
{
    const auto key = attribute(element, "data-setting");
    if (!key.starts_with("discord/")) return false;
    auto *client = readyClient(shell);
    if (!client || remoteSettingsSaving || element->HasAttribute("disabled")) return true;
    const bool checked = !element->IsClassSet("checked");
    Proto::PreloadedSettingsEdit edit;
    if (key == "discord/default-dms") edit.defaultGuildsRestricted = !checked;
    else if (key == "discord/accessibility-detection") edit.allowAccessibilityDetection = checked;
    else if (key == "discord/activity") edit.showCurrentGame = checked;
    else if (key == "discord/developer-mode") edit.developerMode = checked;
    else if (key.starts_with("discord/friend-")) {
        const auto &privacy = client->getSettings().privacy;
        uint32_t flags = privacy ? privacy->friendSourceFlags.value_or(14u) : 14u;
        const uint32_t bit = key == "discord/friend-everyone" ? 8u : key == "discord/friend-mutual" ? 2u : 4u;
        if (checked) flags |= bit;
        else flags &= ~bit;
        if (bit == 8 && checked) flags |= 6u;
        if (bit != 8 && !checked) flags &= ~8u;
        edit.friendSourceFlags = flags;
    } else return true;
    patchRemoteSettings(std::move(edit));
    return true;
}

void SettingsController::renderPrivacy(std::string &rml)
{
    auto *client = readyClient(shell);
    if (!client) { description(rml, "Connect your account to change its privacy settings."); return; }
    description(rml, "These preferences are saved to your Discord account.");
    const auto privacy = client->getSettings().privacy.value_or(Proto::PrivacySettings{});
    appendSwitch(rml, "discord/default-dms", !privacy.defaultGuildsRestricted, "Allow direct messages from server members",
        "Applies to new servers you join. Existing server preferences are unchanged.", remoteSettingsSaving);
    appendSwitch(rml, "discord/accessibility-detection", privacy.allowAccessibilityDetection, "Allow screen reader detection",
        "Allow Discord clients to detect screen reader usage. Styx does not collect this information.", remoteSettingsSaving);
    renderRemoteStatus(rml);
}

void SettingsController::renderFriendRequests(std::string &rml)
{
    auto *client = readyClient(shell);
    if (!client) { description(rml, "Connect your account to change who can add you."); return; }
    description(rml, "Choose who can send you a friend request on Discord.");
    const auto &privacy = client->getSettings().privacy;
    const uint32_t flags = privacy ? privacy->friendSourceFlags.value_or(14u) : 14u;
    appendSwitch(rml, "discord/friend-everyone", flags & 8u, "Everyone", "Allow requests from people with no mutual friends or servers.", remoteSettingsSaving);
    appendSwitch(rml, "discord/friend-mutual", flags & 2u, "Friends of Friends", "Allow requests from people with mutual friends.", remoteSettingsSaving);
    appendSwitch(rml, "discord/friend-servers", flags & 4u, "Server Members", "Allow requests from people in your servers.", remoteSettingsSaving);
    renderRemoteStatus(rml);
}

void SettingsController::renderActivityPrivacy(std::string &rml)
{
    auto *client = readyClient(shell);
    if (!client) { description(rml, "Connect your account to change its activity visibility."); return; }
    const bool shared = client->getSettings().status ? client->getSettings().status->showCurrentGame.value_or(true) : true;
    appendSwitch(rml, "discord/activity", shared, "Share your activity", "Show activity on Discord through the account's activity-sharing preference.", remoteSettingsSaving);
    description(rml, "Styx does not detect running games or provide an in-game overlay.");
    renderRemoteStatus(rml);
}

void SettingsController::renderChat(std::string &rml)
{
    description(rml, "Message display preferences apply to this device.");
    appendSwitch(rml, Keys::ShowEmbeds, getBool(Keys::ShowEmbeds, Defaults::ShowEmbeds), "Show embeds and link previews", "Display preview cards for links in messages.");
    appendSwitch(rml, Keys::ShowReactions, getBool(Keys::ShowReactions, Defaults::ShowReactions), "Show message reactions", "Display reaction emoji and counts below messages.");
}

void SettingsController::renderAccessibility(std::string &rml)
{
    description(rml, "Changes apply to this device immediately.");
    std::string avatar = "icon:smile";
    if (auto *instance = shell.currentInstance()) {
        const auto &me = instance->discord()->getMe();
        const auto url = me.avatar.hasValue() && !me.avatar->empty()
            ? Discord::Cdn::userAvatar(instance->accountId(), me.avatar.get(), 80)
            : Discord::Cdn::defaultUserAvatar(instance->accountId());
        avatar = shell.imageSource(url, {40, 40});
    }
    rml += std::format("<div class=\"settings-message-preview\"><div class=\"message-row group-start\">"
        "<div class=\"message-avatar-clip\"><img class=\"message-avatar\" src=\"{0}\"/></div>"
        "<div class=\"message-header\"><span class=\"message-author\">You</span><span class=\"message-timestamp\">12:00</span></div>"
        "<div class=\"message-body\">This is how your messages will look when a long line wraps onto the next one. "
        "<span class=\"chat-link\">Example link</span></div></div>"
        "<div class=\"message-row\"><div class=\"message-body\">Code uses <span class=\"inline-code\">inline code</span> and blocks:"
        "<pre class=\"code-block\">const int size = 16;</pre></div></div></div>", Platform::htmlEscape(avatar));
    appendSubsectionHeading(rml, "text-readability");
    appendSteppedSlider(rml, "message-font-size", "accessibility/message-font-px", "Text size in chat",
        "Adjust the size of the chat font.", Core::Theme::chatTextSizes, Core::Theme::chatTextSize(), "px");
    appendSwitch(rml, Keys::UnderlineLinks, getBool(Keys::UnderlineLinks, Defaults::UnderlineLinks), "Always underline links", "Make links stand out in messages.");
    appendSubsectionHeading(rml, "visual-density");
    appendSwitch(rml, Keys::CompactMessages, getBool(Keys::CompactMessages, Defaults::CompactMessages), "Compact message spacing", "Use less space between message groups and hide message avatars.");
    appendSwitch(rml, Keys::ShowAvatars, getBool(Keys::ShowAvatars, Defaults::ShowAvatars), "Show message avatars", "Show profile pictures beside messages in the standard layout.");
    appendSteppedSlider(rml, "interface-zoom", Keys::InterfaceZoom, "Zoom level",
        "Adjust the size of the interface. You can also change this with Ctrl +/-.", Core::Theme::zoomLevels,
        View::interfaceZoom(), "");
    appendSubsectionHeading(rml, "reduced-motion");
    appendSwitch(rml, Keys::ReducedMotion, getBool(Keys::ReducedMotion, Defaults::ReducedMotion), "Reduce interface motion", "Turn off interface transitions and CSS animations.");
}

void SettingsController::appendSteppedSlider(std::string &rml, const std::string &id, const std::string &key,
    const std::string &title, const std::string &detail, std::span<const int> stops, int value, const std::string &unit)
{
    const int low = stops.front(), high = stops.back();
    rml += std::format("<div class=\"settings-slider-wrap\"><label class=\"settings-label\" for=\"{0}\">{1}</label>"
        "<div class=\"settings-description\">{2}</div><div class=\"settings-slider-ticks\">",
        id, Platform::htmlEscape(title), Platform::htmlEscape(detail));
    for (const int stop : stops)
        rml += std::format("<span class=\"settings-slider-tick{}{}{}\" data-value=\"{}\" style=\"left: {}%;\">{}{}</span>",
            stop == low ? " start" : "", stop == high ? " end" : "", stop == value ? " active" : "", stop,
            (stop - low) * 100.0 / (high - low), stop, unit);
    // The highlighted stop label shows the value, as in Discord, so these sliders have no floating hint.
    rml += std::format("</div><input id=\"{0}\" type=\"range\" class=\"settings-slider settings-stepped-slider\" min=\"{1}\" max=\"{2}\" "
        "step=\"1\" value=\"{3}\" data-setting=\"{4}\" aria-label=\"{5}\"/></div>",
        id, low, high, value, Platform::htmlEscape(key), Platform::htmlEscape(title));
}

void SettingsController::renderSystem(std::string &rml)
{
    if (Platform::Desktop::launchAtLoginSupported()) {
        const bool enabled = Platform::Desktop::launchAtLoginEnabled();
        appendSwitch(rml, "system/launch-at-login", enabled, "Open Styx when your computer starts", "Launch Styx when you sign into Windows.");
        appendSwitch(rml, Keys::StartMinimized, getBool(Keys::StartMinimized, Defaults::StartMinimized), "Start Styx minimized", "Applies when Windows launches Styx at sign-in.", !enabled);
    }
    appendSwitch(rml, Keys::CloseToTray, getBool(Keys::CloseToTray, Defaults::CloseToTray), "Minimize to system tray", "Closing the window keeps Styx running in the notification tray.");
    if (!remoteSettingsError.empty()) renderRemoteStatus(rml);
}

void SettingsController::renderLanguage(std::string &rml)
{
    auto *client = readyClient(shell);
    if (!client) { description(rml, "Connect your account to change its Discord language."); return; }
    description(rml, "This updates your Discord account language and the locale sent with Discord requests. Styx's interface is currently English.");
    const auto &localization = client->getSettings().localization;
    const auto selected = localization ? localization->locale.value_or("en-US") : "en-US";
    rml += "<label class=\"settings-label\" for=\"discord-locale\">Discord account language</label><select id=\"discord-locale\" class=\"settings-select\" data-setting=\"discord/locale\"";
    if (remoteSettingsSaving) rml += " disabled=\"disabled\"";
    rml += ">";
    for (const auto &[code, label] : {std::pair{"en-US", "English, US"}, {"en-GB", "English, UK"}, {"bg", "Bulgarian"},
        {"zh-CN", "Chinese, China"}, {"zh-TW", "Chinese, Taiwan"}, {"hr", "Croatian"}, {"cs", "Czech"}, {"da", "Danish"},
        {"nl", "Dutch"}, {"fi", "Finnish"}, {"fr", "French"}, {"de", "German"}, {"el", "Greek"}, {"hi", "Hindi"},
        {"hu", "Hungarian"}, {"id", "Indonesian"}, {"it", "Italian"}, {"ja", "Japanese"}, {"ko", "Korean"}, {"lt", "Lithuanian"},
        {"no", "Norwegian"}, {"pl", "Polish"}, {"pt-BR", "Portuguese, Brazil"}, {"ro", "Romanian"}, {"ru", "Russian"},
        {"es-ES", "Spanish, Spain"}, {"es-419", "Spanish, Latin America"}, {"sv-SE", "Swedish"}, {"th", "Thai"}, {"tr", "Turkish"},
        {"uk", "Ukrainian"}, {"vi", "Vietnamese"}})
        rml += std::format("<option value=\"{}\"{}>{}</option>", code, selected == code ? " selected" : "", label);
    rml += "</select>";
    renderRemoteStatus(rml);
}

} // namespace Styx::UI::Rml
