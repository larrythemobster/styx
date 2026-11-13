#include "SettingsController.hpp"

#include "ProfileEditorController.hpp"
#include "Shell.hpp"
#include "ShortcutController.hpp"
#include "ReleaseVersion.hpp"
#include "Shortcuts.hpp"
#include "RmlRuntime.hpp"
#include "UI/PushToTalk.hpp"

#include "Core/AnimatedImageCache.hpp"
#include "Core/Audio/Sounds.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/Session.hpp"
#include "Core/SettingsKeys.hpp"
#include "Core/Stream/StreamQuality.hpp"
#include "Core/Theme/Fonts.hpp"
#include "Core/Theme/Generator.hpp"
#include "Core/Theme/Manager.hpp"
#include "Core/Theme/Tokens.hpp"
#include "Core/UserManager.hpp"
#include "Discord/CdnUrls.hpp"
#include "Discord/Entities.hpp"
#include "Proto/PreloadedSettings.hpp"

#include "Platform/Notifications.hpp"
#include "Platform/Desktop.hpp"
#include <algorithm>
#include <cmath>
#include <random>
#include <sstream>
#include <vector>
#include <charconv>
#include <format>
#include <tuple>

#ifndef STYX_NO_VOICE
#include "Core/Audio/AudioBackends.hpp"
#include "Core/Audio/IAudioBackend.hpp"
#include "Core/Audio/VoiceManager.hpp"
#endif

#include <RmlUi/Core/Box.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

namespace Styx {
namespace UI {
namespace Rml {

using namespace Styx::Core::Settings;

namespace {
template<class T> std::string textValue(const T &value) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
}
template<class... Args> std::string formatRml(std::string text, const Args &...args) {
    const std::vector<std::string> values{ textValue(args)... };
    for (size_t i = 0; i < values.size(); ++i) {
        const auto marker = "%" + std::to_string(i + 1);
        size_t pos = 0;
        while ((pos = text.find(marker, pos)) != std::string::npos) {
            text.replace(pos, marker.size(), values[i]);
            pos += values[i].size();
        }
    }
    return text;
}
std::string hexEncode(const std::string &bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) { result += digits[c >> 4]; result += digits[c & 15]; }
    return result;
}
std::string hexDecode(const std::string &text) {
    std::string result;
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i + 1 < text.size(); i += 2) {
        int high = digit(text[i]), low = digit(text[i + 1]);
        if (high < 0 || low < 0) return {};
        result += static_cast<char>((high << 4) | low);
    }
    return result;
}
Platform::Color pickerColor(double hue, double saturation, double brightness)
{
    const double sector = std::fmod(hue / 60.0, 6.0);
    const double chroma = brightness * saturation;
    const double x = chroma * (1.0 - std::abs(std::fmod(sector, 2.0) - 1.0));
    const double m = brightness - chroma;
    const double r = sector < 1 ? chroma : sector < 2 ? x : sector < 4 ? 0 : sector < 5 ? x : chroma;
    const double g = sector < 1 ? x : sector < 3 ? chroma : sector < 4 ? x : 0;
    const double b = sector < 2 ? 0 : sector < 3 ? x : sector < 5 ? chroma : x;
    return Platform::Color{static_cast<uint8_t>(std::lround((r + m) * 255)),
        static_cast<uint8_t>(std::lround((g + m) * 255)),
        static_cast<uint8_t>(std::lround((b + m) * 255)), 255, true};
}
}



SettingsController::SettingsController(Shell &shell, ::Rml::Element *root)
    : Controller(shell, root)
{
    const auto appearance = Core::Theme::loadAppearance();
    themeSeedColor = appearance.seed;
    themeSchemeIndex = static_cast<int>(appearance.scheme);
    themeDarkMode = appearance.customDark;
    readPickerColor();
    track(shell.settingsOpenChanged.connect([this](bool open) { onSettingsOpenChanged(open); }));
    track(shell.instanceChanged.connect([this](Core::ClientInstance *) {
        ++remoteSettingsRequest;
        remoteSettingsSaving = false;
        remoteSettingsError.clear();
        bindAccountSettings();
        if (this->shell.isSettingsOpen())
            rebuildTimer.start(std::chrono::milliseconds(0), false, [this] { rebuild(); });
    }));
    bindAccountSettings();
    track(shell.accountReady.connect([this](Core::Snowflake account) {
        auto *instance = this->shell.currentInstance();
        if (!instance || instance->accountId() != account) return;
        adoptAccountTheme();
        if (!this->shell.isSettingsOpen()) return;
        if (profileEditor && !profileEditor->hasPendingChanges() && !profileEditor->isSaving())
            profileEditor->open();
        else if (currentSection != Section::Profiles)
            rebuildTimer.start(std::chrono::milliseconds(0), false, [this] { rebuild(); });
    }));
    track(View::systemThemeChanged().connect([this] { onDeviceThemeChanged(); }));

    setupEventListeners();
    if (regionRoot)
        regionRoot->SetProperty("display", "none");
}

SettingsController::~SettingsController() = default;

void SettingsController::onSettingsOpenChanged(bool open)
{
    if (!regionRoot)
        return;

    if (open) {
        if (auto *focused = regionRoot->GetContext()->GetFocusElement())
            previousFocus = focused->GetObserverPtr();
        regionRoot->SetProperty("display", "flex");
        render();
        regionRoot->SetClass("open", true);
        if (auto *navigation = regionRoot->QuerySelector(".settings-nav-item.active"))
            navigation->Focus();
        requestRender(200);
    } else {
        if (profileEditor)
            profileEditor->close();
        clearSliderHint();
        colorDragging = false;
        colorPickerOpen = false;
        stopRecordingKeybind();
        regionRoot->SetClass("open", false);
        regionRoot->SetProperty("display", "none");
        if (previousFocus) previousFocus->Focus();
        previousFocus.reset();
        requestRender(200);
    }
}

void SettingsController::setSection(Section section)
{
    if (currentSection == section)
        return;
    Platform::Guard<SettingsController> self(this);
    if (!confirmProfileLeave([self, section] { if (self) self->setSection(section); }))
        return;
    stopRecordingKeybind();
    colorDragging = false;
    colorPickerOpen = false;
    currentSection = section;
    render();
    requestRender(100);
}

void SettingsController::rebuild()
{
    if (shell.isSettingsOpen()) {
        render();
        requestRender(100);
    }
}

void SettingsController::render()
{
    if (!regionRoot)
        return;
    std::string focusSelector;
    if (auto *focused = regionRoot->GetContext()->GetFocusElement()) {
        for (auto *parent = focused; parent; parent = parent->GetParentNode()) {
            if (parent != regionRoot) continue;
            if (!focused->GetId().empty()) focusSelector = "#" + focused->GetId();
            else if (focused->HasAttribute("data-action")) {
                focusSelector = focused->GetTagName() + "[data-action=\"" + attribute(focused, "data-action") + "\"]";
                for (const auto *qualifier : {"data-role", "data-keybind"})
                    if (focused->HasAttribute(qualifier))
                        focusSelector += "[" + std::string(qualifier) + "=\"" + attribute(focused, qualifier) + "\"]";
            } else if (focused->IsClassSet("settings-radio-card")) {
                focusSelector = ".settings-radio-card[data-setting=\"" + attribute(focused, "data-setting") +
                                "\"][data-value=\"" + attribute(focused, "data-value") + "\"]";
            } else focusSelector = ".settings-nav-item.active";
            break;
        }
    }

    clearSliderHint();
    profileEditor.reset();
    std::string rml = "<div class=\"settings-dialog\" role=\"dialog\" aria-modal=\"true\" aria-label=\"User settings\">";
    renderSidebar(rml);
    rml += "<div class=\"settings-close-container\"><button class=\"settings-close-btn\" data-action=\"close\" "
           "title=\"Close Settings\" aria-label=\"Close Settings\"><img src=\"icon:x\"/></button></div>";
    renderContent(rml);
    rml += "</div>";

    // Native controls emit change events while their initial values are installed.
    renderingControls = true;
    regionRoot->SetInnerRML(rml.c_str());
    if (currentSection == Section::Profiles) {
        if (auto *host = regionRoot->GetElementById("settings-profile-editor")) {
            profileEditor = std::make_unique<ProfileEditorController>(shell, *host);
            profileEditor->open();
        }
    }
    renderingControls = false;
    if (!focusSelector.empty()) {
        auto *focus = regionRoot->QuerySelector(focusSelector);
        if (!focus) focus = regionRoot->QuerySelector(".settings-nav-item.active");
        if (focus) focus->Focus();
    }
}

void SettingsController::renderSidebar(std::string &rml)
{
    rml += std::string("<div class=\"settings-sidebar\">");
    if (auto *instance = shell.currentInstance()) {
        const auto &account = instance->accountInfo();
        const auto &me = instance->discord()->getMe();
        const auto name = me.globalName.valueOr(account.displayName.empty() ? account.username : account.displayName);
        const auto avatar = me.avatar.hasValue() && !me.avatar->empty()
            ? Discord::Cdn::userAvatar(instance->accountId(), me.avatar.get(), 80)
            : Discord::Cdn::defaultUserAvatar(instance->accountId());
        rml += formatRml("<button class=\"settings-btn settings-account\" data-section=\"profiles\" aria-label=\"Edit profile\">"
            "<span class=\"settings-account-avatar\"><img src=\"%1\"/></span><span class=\"settings-account-text\">"
            "<span class=\"settings-account-name\">%2</span><span class=\"settings-account-edit\">Edit profile</span></span>"
            "<img class=\"settings-account-pencil\" src=\"icon:pencil\"/></button>",
            Rml::escape(shell.imageSource(avatar, {40, 40})), Rml::escape(name));
    }

    rml += formatRml("<div id=\"settings-search-wrap\" class=\"settings-search-wrap%1\"><img class=\"settings-search-icon\" src=\"icon:search\"/>"
        "<input id=\"settings-search\" class=\"settings-search\" type=\"text\" aria-label=\"Search settings\" value=\"%2\"/>"
        "<span class=\"settings-search-placeholder\" aria-hidden=\"true\">Search settings</span>"
        "<button class=\"settings-search-clear settings-btn\" data-action=\"clear-settings-search\" aria-label=\"Clear search\"><img src=\"icon:x\"/></button></div>",
        searchQuery.empty() ? "" : " has-query", Rml::escape(searchQuery));
    rml += "<div id=\"settings-navigation\">";
    renderNavigation(rml);
    rml += "</div>";

    // Footer info
    rml += std::string("<div class=\"settings-sidebar-footer\">");
    rml += std::string("<div class=\"settings-app-version\">Styx v") + std::string(currentVersion) + "</div>";
    rml += std::string("<div class=\"settings-credit\">Based on Acheron by ouwou</div>");
    rml += std::string("</div>"); // .settings-sidebar-footer

    rml += std::string("</div>"); // .settings-sidebar
}

void SettingsController::renderContent(std::string &rml)
{
    rml += "<div class=\"settings-content-pane\"><div class=\"settings-page-header\"><div class=\"settings-section-title\">" +
        Platform::htmlEscape(sectionTitle()) + "</div></div>";
    rml += formatRml("<div class=\"settings-content-scroll%1\"><div class=\"settings-content-wrapper\">",
        currentSection == Section::Profiles ? " settings-profiles-page" : "");

    switch (currentSection) {
    case Section::MyAccount:
        renderMyAccount(rml);
        break;
    case Section::Appearance:
        renderAppearance(rml);
        break;
    case Section::Chat:
        renderChat(rml);
        break;
    case Section::Notifications:
        renderNotifications(rml);
        break;
    case Section::VoiceVideo:
        renderVoiceVideo(rml);
        break;
    case Section::Keybinds:
        renderKeybinds(rml);
        break;
    case Section::GoLive:
        renderGoLive(rml);
        break;
    case Section::Advanced:
        renderAdvanced(rml);
        break;
    case Section::Profiles: renderProfiles(rml); break;
    case Section::Privacy: renderPrivacy(rml); break;
    case Section::FriendRequests: renderFriendRequests(rml); break;
    case Section::Accessibility: renderAccessibility(rml); break;
    case Section::ActivityPrivacy: renderActivityPrivacy(rml); break;
    case Section::System: renderSystem(rml); break;
    case Section::Language: renderLanguage(rml); break;
    }

    rml += std::string("</div>"); // .settings-content-wrapper
    rml += "</div>"; // .settings-content-scroll
    rml += std::string("</div>"); // .settings-content-pane
}

void SettingsController::renderMyAccount(std::string &rml)
{

    auto *inst = shell.currentInstance();
    std::string displayName = std::string("Not logged in");
    std::string username = std::string("none");
    std::string accountIdStr = std::string("-");
    std::string securityStatus = "Not available";
    std::string avatarSrc;

    if (inst) {
        const auto &info = inst->accountInfo();
        const auto accountId = inst->accountId();
        accountIdStr = accountId.toString();

        auto *userMgr = inst->users();
        std::optional<Discord::User> me;
        if (userMgr)
            me = userMgr->getUser(accountId);

        displayName = (me && me->globalName.hasValue() && !me->globalName->empty())
                          ? *me->globalName
                          : (info.displayName.empty() ? info.username : info.displayName);
        if (displayName.empty())
            displayName = info.username;

        username = me ? me->username.get() : info.username;
        const auto &self = inst->discord()->getMe();
        if (self.id.get() == accountId && self.mfaEnabled.hasValue())
            securityStatus = self.mfaEnabled.get() ? "Enabled" : "Not enabled";

        std::string avatarHash = (me && me->avatar.hasValue()) ? *me->avatar : info.avatar;
        std::string avatarUrl = !avatarHash.empty()
                             ? Discord::Cdn::userAvatar(accountId, avatarHash, 80)
                             : Discord::Cdn::defaultUserAvatar(accountId);
        avatarSrc = shell.imageSource(avatarUrl, Platform::Size{80, 80});
    }

    rml += std::string("<div class=\"my-account-card\">");
    rml += std::string("<div class=\"my-account-profile-header\">");
    if (!avatarSrc.empty()) {
        rml += formatRml("<img class=\"my-account-avatar\" src=\"%1\"/>", Rml::escape(avatarSrc));
    }
    rml += std::string("<div class=\"my-account-names\">");
    rml += formatRml("<div class=\"my-account-display-name\">%1</div>", Rml::escape(displayName));
    rml += formatRml("<div class=\"my-account-username\">@%1</div>", Rml::escape(username));
    rml += std::string("</div>"); // .my-account-names
    rml += std::string("</div>"); // .my-account-profile-header

    rml += std::string("<div class=\"my-account-field-card\">");
    rml += std::string("<div class=\"my-account-field-label\">Display Name</div>");
    rml += formatRml("<div class=\"my-account-field-value\">%1</div>", Rml::escape(displayName));
    rml += std::string("</div>");

    rml += std::string("<div class=\"my-account-field-card\">");
    rml += std::string("<div class=\"my-account-field-label\">Username</div>");
    rml += formatRml("<div class=\"my-account-field-value\">@%1</div>", Rml::escape(username));
    rml += std::string("</div>");

    rml += std::string("<div class=\"my-account-field-card\">");
    rml += std::string("<div class=\"my-account-field-label\">User ID</div>");
    rml += formatRml("<div class=\"my-account-field-value\">%1</div>", Rml::escape(accountIdStr));
    rml += std::string("</div>");

    rml += std::string("<div class=\"my-account-field-card\">");
    rml += std::string("<div class=\"my-account-field-label\">Two-Factor Authentication</div>");
    rml += formatRml("<div class=\"my-account-field-value\">%1</div>", Rml::escape(securityStatus));
    rml += std::string("</div>");
    rml += "<div class=\"settings-description\">Email, password and security changes are not available in Styx.</div>";

    rml += std::string(
        "<div class=\"settings-button-group\">"
        "<button class=\"settings-btn settings-btn-primary\" data-action=\"edit-profile\">"
        "<span class=\"settings-btn-label\">Edit Profile</span>"
        "</button>"
        "<button class=\"settings-btn settings-btn-secondary\" data-action=\"manage-accounts\">"
        "<span class=\"settings-btn-label\">Switch / Manage Accounts</span>"
        "</button>"
        "</div>");
    rml += std::string("</div>"); // .my-account-card
}

void SettingsController::renderAppearance(std::string &rml)
{
    using Core::Theme::ThemeChoice;
    const auto appearance = Core::Theme::loadAppearance();
    const auto device = View::systemTheme();
    const auto shown = Core::Theme::effectiveChoice(appearance, device);

    // Discord's Theme section: device following, default themes and account sync.
    rml += std::string("<div class=\"settings-card\">");
    rml += std::string("<div class=\"settings-theme-heading\">Theme</div>");
    if (!appearanceSaveError.empty())
        rml += "<div class=\"settings-keybind-error\" role=\"alert\">" + Rml::escape(appearanceSaveError) + "</div>";
    appendSwitch(rml, Keys::ThemeFollowDevice, appearance.followDevice && device != Core::Theme::DeviceTheme::Unknown,
        "Same as Device Theme", device == Core::Theme::DeviceTheme::Unknown
            ? "Your system does not report a light or dark preference."
            : "Match your device's light or dark mode.",
        device == Core::Theme::DeviceTheme::Unknown);
    rml += std::string("<div class=\"settings-label\">Default Themes</div>");
    rml += std::string("<div class=\"settings-theme-swatches\" role=\"radiogroup\" aria-label=\"Default themes\">");
    for (const auto &[choice, label, preview] : {std::tuple{ThemeChoice::Light, "Light", "#ffffff"},
             std::tuple{ThemeChoice::Ash, "Ash", "#323339"}, std::tuple{ThemeChoice::Dark, "Dark", "#1a1a1e"},
             std::tuple{ThemeChoice::Onyx, "Onyx", "#000000"}}) {
        const bool selected = shown == choice;
        rml += std::format("<button class=\"settings-theme-swatch{}\" data-action=\"select-theme\" data-theme=\"{}\" "
            "role=\"radio\" aria-checked=\"{}\" aria-label=\"{}\" title=\"{}\" style=\"background-color: {};\">"
            "<span class=\"settings-theme-check\">&#10003;</span></button>",
            selected ? " selected" : "", Core::Theme::choiceId(choice), selected ? "true" : "false", label, label, preview);
    }
    rml += std::string("</div>");
    if (appearance.followDevice && device != Core::Theme::DeviceTheme::Unknown)
        rml += std::string("<div class=\"settings-description\">Your device chooses light or dark. Picking a theme stops following it.</div>");
    rml += std::string("<div class=\"settings-description\">Discord's Nitro color themes are not available in Styx. "
        "Use the Styx custom theme below to build your own palette.</div>");
    appendSwitch(rml, Keys::ThemeSync, appearance.syncTheme, "Sync theme across devices",
        readyClient(shell) ? "Save the default theme to your Discord account and follow changes made on other devices."
                           : "Uses your Discord account's theme once an account is connected.");
    if (appearance.syncTheme)
        renderRemoteStatus(rml);
    rml += std::string("</div>"); // .settings-card

    // Styx custom theme, generated locally and never synced
    rml += std::string("<div class=\"settings-card\">");
    rml += std::string("<div class=\"settings-label\">Styx Custom Theme</div>");
    rml += formatRml("<div class=\"settings-description\">%1</div>", appearance.choice == ThemeChoice::Custom
        ? std::string("Your custom theme is active on this device.")
        : std::string("Build a theme for this device from an accent color. Applying it replaces the default theme."));
    rml += std::string("<div class=\"settings-color-row\">");
    rml += std::string("<div class=\"settings-color-field\">");
    rml += std::string("<label class=\"settings-color-label\" for=\"seed-color-hex\">Accent color</label>");
    rml += std::string("<div class=\"settings-color-input-row\">");
    rml += formatRml("<button id=\"seed-color-toggle\" class=\"settings-color-swatch settings-btn\" "
        "data-action=\"toggle-color-picker\" title=\"Choose accent color\" aria-label=\"Choose accent color\" "
        "aria-controls=\"seed-color-picker\" aria-expanded=\"%2\" style=\"background-color: %1;\"></button>",
        themeSeedColor.css(), colorPickerOpen ? "true" : "false");
    rml += formatRml("<input type=\"text\" class=\"settings-text-input\" id=\"seed-color-hex\" value=\"%1\"/>", themeSeedColor.css());
    rml += std::string("</div>");
    rml += formatRml("<div id=\"seed-color-picker\" class=\"settings-color-picker\" style=\"display: %1;\">",
        colorPickerOpen ? "block" : "none");
    rml += formatRml("<div id=\"seed-color-plane\" class=\"settings-color-plane\" tabindex=\"0\" role=\"slider\" "
        "aria-label=\"Color saturation and brightness. Use arrow keys to adjust.\" style=\"background-color: %1;\">",
        pickerColor(pickerHue, 1, 1).css());
    rml += "<div class=\"settings-color-layer\" style=\"decorator: linear-gradient(to right, #ffffff, #ffffff00);\"></div>"
           "<div class=\"settings-color-layer\" style=\"decorator: linear-gradient(to bottom, #00000000, #000000);\"></div>";
    rml += formatRml("<div id=\"seed-color-cursor\" class=\"settings-color-cursor\" style=\"left: %1%; top: %2%;\">"
        "<div class=\"settings-color-cursor-inner\"></div></div></div>", pickerSaturation * 100, (1 - pickerBrightness) * 100);
    rml += "<label class=\"settings-color-label\" for=\"seed-color-hue\">Hue</label>";
    rml += formatRml("<input type=\"range\" id=\"seed-color-hue\" class=\"settings-color-hue\" "
        "min=\"0\" max=\"360\" step=\"1\" value=\"%1\" aria-label=\"Color hue\"/>", int(std::lround(pickerHue)));
    rml += "<div class=\"settings-description settings-color-help\">Drag to choose a color, then apply the custom theme.</div></div>";
    rml += std::string("</div>");
    rml += std::string("</div>");

    rml += std::string("<div class=\"settings-row\">");
    rml += std::string("<label class=\"settings-label\" for=\"theme-scheme-select\">Palette Scheme</label>");
    rml += std::string("<select class=\"settings-select\" id=\"theme-scheme-select\">");
    for (int i = 0; i < Core::Theme::schemeCount; ++i) {
        auto s = static_cast<Core::Theme::Scheme>(i);
        std::string selected = (i == themeSchemeIndex) ? std::string(" selected") : std::string();
        rml += formatRml("<option value=\"%1\"%2>%3</option>", i, selected, Core::Theme::schemeName(s));
    }
    rml += std::string("</select>");
    rml += std::string("</div>");

    const bool modeFollowsDevice = appearance.followDevice && device != Core::Theme::DeviceTheme::Unknown;
    rml += std::string("<div class=\"settings-row\">");
    rml += std::string("<label class=\"settings-label\" for=\"theme-mode-select\">Mode</label>");
    rml += formatRml("<select class=\"settings-select\" id=\"theme-mode-select\"%1>", modeFollowsDevice ? std::string(" disabled=\"disabled\"") : std::string());
    const bool dark = Core::Theme::effectiveCustomDark(appearance, device);
    rml += formatRml("<option value=\"dark\"%1>Dark</option>", dark ? std::string(" selected") : std::string());
    rml += formatRml("<option value=\"light\"%1>Light</option>", !dark ? std::string(" selected") : std::string());
    rml += std::string("</select>");
    if (modeFollowsDevice)
        rml += std::string("<div class=\"settings-description\">Following your device's light or dark mode.</div>");
    rml += std::string("</div>");

    rml += std::string("<div class=\"settings-button-group\">");
    rml += std::string("<button class=\"settings-btn settings-btn-primary\" data-action=\"generate-theme\"><span class=\"settings-btn-label\">Apply custom theme</span></button>");
    rml += std::string("<button class=\"settings-btn settings-btn-secondary\" data-action=\"random-theme\"><span class=\"settings-btn-label\">Randomize</span></button>");
    rml += std::string("<button class=\"settings-btn settings-btn-secondary\" data-action=\"reset-theme\"><span class=\"settings-btn-label\">Reset to default theme</span></button>");
    rml += std::string("</div>");
    rml += formatRml("<div id=\"theme-error\" class=\"settings-keybind-error\" style=\"display: %1;\">%2</div>",
                     themeError.empty() ? "none" : "block", Rml::escape(themeError));
    rml += std::string("</div>"); // .settings-card

#ifdef _WIN32
    // Windows contrast colors override every palette above while enabled.
    rml += std::string("<div class=\"settings-card\">");
    appendSwitch(rml, Keys::ThemeUseSystemColors, getBool(Keys::ThemeUseSystemColors, Defaults::ThemeUseSystemColors),
        "Use Windows system colors", "Use Windows color settings, such as contrast themes, instead of the theme above.");
    rml += std::string("</div>"); // .settings-card
#endif

    // Font overrides
    rml += std::string("<div class=\"settings-card\">");
    rml += std::string("<div class=\"settings-label\">Font Overrides</div>");
    rml += std::string("<div class=\"settings-description\">Tune typography sizes across Styx interface roles.</div>");

    for (const auto &fd : Core::Theme::fontRegistry()) {
        const auto role = fd.role;
        const auto currentFont = Core::Theme::Manager::instance().font(role);
        const int ptSize = static_cast<int>(currentFont.pointSize);

        rml += std::string("<div class=\"settings-font-row\">");
        rml += formatRml("<span class=\"settings-font-name\">%1</span>", std::string(fd.label));
        rml += formatRml("<span class=\"settings-font-size\">%1 pt</span>", ptSize);
        rml += std::string("<div class=\"settings-font-controls\">");
        rml += formatRml("<button class=\"settings-btn settings-btn-secondary settings-font-adjust\" data-action=\"font-size-dec\" data-role=\"%1\" title=\"Decrease %2 font size\" aria-label=\"Decrease %2 font size\"><img class=\"settings-btn-icon\" src=\"icon:minus\"/></button>", static_cast<int>(role), std::string(fd.label));
        rml += formatRml("<button class=\"settings-btn settings-btn-secondary settings-font-adjust\" data-action=\"font-size-inc\" data-role=\"%1\" title=\"Increase %2 font size\" aria-label=\"Increase %2 font size\"><img class=\"settings-btn-icon\" src=\"icon:plus\"/></button>", static_cast<int>(role), std::string(fd.label));
        rml += formatRml("<button class=\"settings-btn settings-btn-secondary settings-font-reset\" data-action=\"reset-font\" data-role=\"%1\"><span class=\"settings-btn-label\">Reset</span></button>", static_cast<int>(role));
        rml += std::string("</div>");
        rml += std::string("</div>");
    }
    rml += std::string("</div>"); // .settings-card
}

void SettingsController::readPickerColor()
{
    const double r = themeSeedColor.r / 255.0;
    const double g = themeSeedColor.g / 255.0;
    const double b = themeSeedColor.b / 255.0;
    const double high = std::max({r, g, b});
    const double delta = high - std::min({r, g, b});
    pickerBrightness = high;
    if (high > 0)
        pickerSaturation = delta / high;
    if (delta > 0) {
        const double sector = high == r ? std::fmod((g - b) / delta + 6.0, 6.0) :
            high == g ? (b - r) / delta + 2.0 : (r - g) / delta + 4.0;
        pickerHue = sector * 60.0;
    }
}

void SettingsController::refreshColorPicker(bool updateHex)
{
    if (!regionRoot)
        return;
    const bool wasRendering = renderingControls;
    renderingControls = true;
    if (auto *swatch = regionRoot->GetElementById("seed-color-toggle"))
        swatch->SetProperty("background-color", themeSeedColor.css());
    if (auto *plane = regionRoot->GetElementById("seed-color-plane")) {
        plane->SetProperty("background-color", pickerColor(pickerHue, 1, 1).css());
        plane->SetAttribute("aria-valuetext", "Saturation " + std::to_string(int(pickerSaturation * 100)) +
            "%, brightness " + std::to_string(int(pickerBrightness * 100)) + "%");
    }
    if (auto *cursor = regionRoot->GetElementById("seed-color-cursor")) {
        cursor->SetProperty("left", std::to_string(pickerSaturation * 100) + "%");
        cursor->SetProperty("top", std::to_string((1 - pickerBrightness) * 100) + "%");
    }
    if (auto *hue = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(regionRoot->GetElementById("seed-color-hue")))
        hue->SetValue(std::to_string(int(std::lround(pickerHue))));
    if (updateHex)
        if (auto *hex = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(regionRoot->GetElementById("seed-color-hex")))
            hex->SetValue(themeSeedColor.css());
    renderingControls = wasRendering;
    requestRender();
}

void SettingsController::applyPickerColor()
{
    themeSeedColor = pickerColor(pickerHue, pickerSaturation, pickerBrightness);
    themeError.clear();
    if (auto *error = regionRoot->GetElementById("theme-error"))
        error->SetProperty("display", "none");
    refreshColorPicker();
}

void SettingsController::pickColorAt(float x, float y)
{
    auto *plane = regionRoot->GetElementById("seed-color-plane");
    if (!plane || plane->GetOffsetWidth() <= 0 || plane->GetOffsetHeight() <= 0)
        return;
    pickerSaturation = std::clamp(double(x - plane->GetAbsoluteLeft()) / plane->GetOffsetWidth(), 0.0, 1.0);
    pickerBrightness = 1.0 - std::clamp(double(y - plane->GetAbsoluteTop()) / plane->GetOffsetHeight(), 0.0, 1.0);
    applyPickerColor();
}

void SettingsController::applyGeneratedTheme()
{
    if (regionRoot) {
        if (auto *input = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(regionRoot->GetElementById("seed-color-hex"))) {
            const auto color = Platform::Color::parse(input->GetValue());
            if (!color.valid) {
                themeError = "Enter a valid hex color, such as #5865f2.";
                if (auto *error = regionRoot->GetElementById("theme-error")) {
                    error->SetInnerRML(Rml::escape(themeError));
                    error->SetProperty("display", "block");
                }
                requestRender();
                return;
            }
            themeSeedColor = color;
            readPickerColor();
        }
    }
    auto appearance = Core::Theme::loadAppearance();
    appearance.choice = Core::Theme::ThemeChoice::Custom;
    appearance.seed = themeSeedColor;
    appearance.scheme = static_cast<Core::Theme::Scheme>(std::clamp(themeSchemeIndex, 0, Core::Theme::schemeCount - 1));
    appearance.customDark = themeDarkMode;
    themeError.clear();
    applyAppearanceChoice(appearance, true);
}

void SettingsController::applyAppearanceChoice(Core::Theme::Appearance appearance, bool explicitChoice)
{
    // A deliberate theme choice replaces Windows contrast colors; device or account updates do not.
    if (explicitChoice)
        Core::Theme::Manager::instance().setUseSystemColors(false);
    const bool stored = Core::Theme::storeAppearance(appearance);
    const bool saved = Core::Theme::applyAppearance(appearance, View::systemTheme());
    appearanceSaveError = stored && saved ? std::string() : std::string("Theme applied, but it could not be saved.");
    if (!shell.isSettingsOpen() || currentSection != Section::Appearance)
        return;
    rebuildTimer.start(std::chrono::milliseconds(0), false, [this] {
        render();
        requestRender(100);
    });
}

void SettingsController::selectDefaultTheme(Core::Theme::ThemeChoice choice)
{
    auto appearance = Core::Theme::loadAppearance();
    appearance.choice = choice;
    appearance.followDevice = false;
    applyAppearanceChoice(appearance, true);
    if (appearance.syncTheme)
        pushThemeToAccount(choice);
}

void SettingsController::pushThemeToAccount(Core::Theme::ThemeChoice choice)
{
    auto *client = readyClient(shell);
    const auto value = Core::Theme::protoFromChoice(choice);
    if (!client || !value)
        return;
    const auto &remote = client->getSettings().appearance;
    if (remote && Core::Theme::choiceFromProto(remote->theme) == choice)
        return;
    Proto::PreloadedSettingsEdit edit;
    edit.theme = *value;
    patchRemoteSettings(std::move(edit));
}

void SettingsController::adoptAccountTheme()
{
    auto *client = readyClient(shell);
    // While a write is pending, an older account snapshot must not undo the local choice.
    if (!client || remoteSettingsSaving)
        return;
    const auto &remote = client->getSettings().appearance;
    auto appearance = Core::Theme::loadAppearance();
    if (!remote || !appearance.syncTheme || appearance.choice == Core::Theme::ThemeChoice::Custom)
        return;
    const auto choice = Core::Theme::choiceFromProto(remote->theme);
    if (!choice || *choice == appearance.choice)
        return;
    appearance.choice = *choice;
    applyAppearanceChoice(appearance, false);
}

void SettingsController::onDeviceThemeChanged()
{
    const auto appearance = Core::Theme::loadAppearance();
    if (appearance.followDevice)
        applyAppearanceChoice(appearance, false);
}


void SettingsController::renderNotifications(std::string &rml)
{
    using Core::Sounds::Sound;
    auto renderSwitch = [&rml](const std::string &key, bool checked, const std::string &title, const std::string &desc) {
        rml += std::string("<div class=\"settings-switch-row\">");
        rml += std::string("<div class=\"settings-switch-text\">");
        rml += formatRml("<div class=\"settings-switch-title\">%1</div>", title);
        rml += formatRml("<div class=\"settings-switch-desc\">%1</div>", desc);
        rml += std::string("</div>");
        rml += formatRml("<div class=\"settings-switch%1\" data-setting=\"%2\" tabindex=\"0\"><div class=\"settings-switch-thumb\"></div></div>", checked ? std::string(" checked") : std::string(), key);
        rml += std::string("</div>");
    };
    auto renderSoundSwitch = [&rml, &renderSwitch](Sound sound, const std::string &title, const std::string &desc) {
        const std::string key(Core::Sounds::settingKey(sound));
        rml += "<div class=\"settings-sound-row\">";
        renderSwitch(key, getBool(key, true), title, desc);
#ifdef STYX_HAVE_MINIAUDIO
        rml += formatRml("<button class=\"settings-btn settings-btn-secondary\" data-action=\"preview-sound\" "
            "data-sound=\"%1\" title=\"Preview %2 sound\"%3><span class=\"settings-btn-label\">Preview</span></button>",
            static_cast<int>(sound), Rml::escape(title), getBool(Keys::SoundsEnabled, true) ? "" : " disabled=\"disabled\"");
#endif
        rml += "</div>";
    };


    rml += std::string("<div class=\"settings-card\">");
    renderSwitch(Keys::DesktopNotifications, getBool(Keys::DesktopNotifications, Defaults::DesktopNotifications),
                 "Desktop notifications",
                 "Show a notification for new messages your server and channel settings allow, unless you're looking at that channel.");
    if (!Platform::Notifications::available())
        rml += std::string("<div class=\"settings-note\">This system isn't accepting notifications from Styx right now.</div>");
    renderSwitch(Keys::FlashTaskbar, getBool(Keys::FlashTaskbar, Defaults::FlashTaskbar),
                 "Flash the taskbar button",
                 "Flash Styx's taskbar button when a notification arrives and the window isn't focused.");
    renderSwitch(Keys::TaskbarBadge, getBool(Keys::TaskbarBadge, Defaults::TaskbarBadge),
                 "Show the notification dot on the taskbar",
                 "Show a red-and-white dot on Styx's Windows icon while you have unread mentions.");
    rml += std::string("<div class=\"settings-button-group\">");
    rml += std::string("<button class=\"settings-btn settings-btn-secondary\" data-action=\"test-notification\"><span class=\"settings-btn-label\">Send a test notification</span></button>");
    rml += std::string("</div>");
    rml += std::string("</div>"); // .settings-card

    rml += std::string("<div class=\"settings-card\">");
    rml += std::string("<div class=\"settings-label\">Sounds</div>");
#ifdef STYX_HAVE_MINIAUDIO
    appendSwitch(rml, Keys::SoundsEnabled, getBool(Keys::SoundsEnabled, true), "Enable notification sounds",
        "Allow message, mention, incoming-call and voice join/leave sounds.");
#endif
    renderSoundSwitch(Sound::Message, "Message", "A new message that notifies.");
    renderSoundSwitch(Sound::Mention, "Mention", "A message that mentions you, your role or @everyone.");
    renderSoundSwitch(Sound::Ring, "Incoming call", "Rings while someone is calling you in a DM.");
    renderSoundSwitch(Sound::Join, "Voice join", "You or someone else joins your voice channel.");
    renderSoundSwitch(Sound::Leave, "Voice leave", "You or someone else leaves your voice channel.");
    rml += std::string("</div>"); // .settings-card
}

void SettingsController::renderVoiceVideo(std::string &rml)
{

    // Voice Settings card
    rml += std::string("<div class=\"settings-card\">");
    rml += std::string("<div class=\"settings-label\">Voice Settings</div>");

#ifndef STYX_NO_VOICE
    auto *inst = shell.currentInstance();
    auto *vm = inst ? inst->voice() : nullptr;

    std::vector<Core::Audio::AudioDeviceInfo> inputDevices;
    std::vector<Core::Audio::AudioDeviceInfo> outputDevices;
    if (vm) {
        inputDevices = vm->availableInputDevices();
        outputDevices = vm->availableOutputDevices();
    } else {
        auto tempBackend = Core::Audio::IAudioBackend::create();
        if (tempBackend) {
            inputDevices = tempBackend->availableInputDevices();
            outputDevices = tempBackend->availableOutputDevices();
        }
    }

    const std::string currentInput = getString(Keys::VoiceInputDevice, Defaults::VoiceInputDevice);
    const std::string currentOutput = getString(Keys::VoiceOutputDevice, Defaults::VoiceOutputDevice);

    // Side-by-side Input Device & Output Device
    rml += std::string("<div class=\"settings-grid-2col\">");

    // Input Device Column
    rml += std::string("<div class=\"settings-grid-col settings-grid-col-left\">");
    rml += std::string("<div class=\"settings-label\">Input Device</div>");
    rml += formatRml("<select class=\"settings-select\" data-setting=\"%1\">", Keys::VoiceInputDevice);
    rml += formatRml("<option value=\"\"%1>Default Communication Device</option>", currentInput.empty() ? std::string(" selected") : std::string());
    for (const auto &dev : inputDevices) {
        std::string devHex = hexEncode(dev.id);
        std::string sel = (currentInput == devHex || (currentInput.empty() && dev.isDefault)) ? std::string(" selected") : std::string();
        rml += formatRml("<option value=\"%1\"%2>%3</option>", devHex, sel, Rml::escape(dev.description));
    }
    rml += std::string("</select>");
    rml += std::string("</div>");

    // Output Device Column
    rml += std::string("<div class=\"settings-grid-col settings-grid-col-right\">");
    rml += std::string("<div class=\"settings-label\">Output Device</div>");
    rml += formatRml("<select class=\"settings-select\" data-setting=\"%1\">", Keys::VoiceOutputDevice);
    rml += formatRml("<option value=\"\"%1>Default Communication Device</option>", currentOutput.empty() ? std::string(" selected") : std::string());
    for (const auto &dev : outputDevices) {
        std::string devHex = hexEncode(dev.id);
        std::string sel = (currentOutput == devHex || (currentOutput.empty() && dev.isDefault)) ? std::string(" selected") : std::string();
        rml += formatRml("<option value=\"%1\"%2>%3</option>", devHex, sel, Rml::escape(dev.description));
    }
    rml += std::string("</select>");
    rml += std::string("</div>");

    rml += std::string("</div>"); // .settings-grid-2col

    // Side-by-side Input Volume & Output Volume
    const int inputVolume = getInt(Keys::VoiceInputVolume, Defaults::VoiceInputVolume);
    const int outputVolume = getInt(Keys::VoiceOutputVolume, Defaults::VoiceOutputVolume);

    rml += std::string("<div class=\"settings-grid-2col\">");

    // Input Volume Column
    rml += std::string("<div class=\"settings-grid-col settings-grid-col-left\">");
    rml += std::string("<div class=\"settings-slider-wrap\">");
    rml += std::string("<div class=\"settings-slider-header\">");
    rml += std::string("<span class=\"settings-label\">Input Volume</span>");
    rml += std::string("<span class=\"settings-slider-val\" id=\"val-voice-input-vol\">") + std::to_string(inputVolume) + std::string("%</span>");
    rml += std::string("</div>");
    rml += std::string("<input type=\"range\" class=\"settings-slider\" min=\"0\" max=\"100\" step=\"1\" value=\"")
           + std::to_string(inputVolume)
           + std::string("\" data-setting=\"") + std::string(Keys::VoiceInputVolume)
           + std::string("\" data-display=\"val-voice-input-vol\" data-unit=\"%\"/>");
    rml += formatRml("<div class=\"settings-slider-hint\">%1%</div>", inputVolume);
    rml += std::string("</div>"); // .settings-slider-wrap
    rml += std::string("</div>"); // .settings-grid-col
    // Output Volume Column
    rml += std::string("<div class=\"settings-grid-col settings-grid-col-right\">");
    rml += std::string("<div class=\"settings-slider-wrap\">");
    rml += std::string("<div class=\"settings-slider-header\">");
    rml += std::string("<span class=\"settings-label\">Output Volume</span>");
    rml += std::string("<span class=\"settings-slider-val\" id=\"val-voice-output-vol\">") + std::to_string(outputVolume) + std::string("%</span>");
    rml += std::string("</div>");
    rml += std::string("<input type=\"range\" class=\"settings-slider\" min=\"0\" max=\"200\" step=\"1\" value=\"")
           + std::to_string(outputVolume)
           + std::string("\" data-setting=\"") + std::string(Keys::VoiceOutputVolume)
           + std::string("\" data-display=\"val-voice-output-vol\" data-unit=\"%\"/>");
    rml += formatRml("<div class=\"settings-slider-hint\">%1%</div>", outputVolume);
    rml += std::string("</div>"); // .settings-slider-wrap
    rml += std::string("</div>"); // .settings-grid-col
    rml += std::string("</div>"); // .settings-grid-2col

    rml += std::string("<div class=\"settings-description\">Device and volume changes take effect immediately during a call, or on your next voice connection.</div>");
#endif
    rml += std::string("</div>"); // .settings-card (Voice Settings)

    // Input Mode card
    rml += std::string("<div class=\"settings-card\">");
    rml += std::string("<div class=\"settings-label\">Input Mode</div>");
    const std::string inputMode = getString(Keys::VoiceInputMode, Defaults::VoiceInputMode);

    auto renderInputModeRadio = [&](const std::string &val, const std::string &title) {
        const bool active = (inputMode == val);
        rml += formatRml("<div class=\"settings-radio-card%1\" data-setting=\"%2\" data-value=\"%3\" tabindex=\"0\">", active ? std::string(" active") : std::string(), Keys::VoiceInputMode, val);
        rml += std::string("<div class=\"settings-radio-indicator\"><span class=\"settings-radio-dot\"></span></div>");
        rml += std::string("<div class=\"settings-radio-info\">");
        rml += formatRml("<div class=\"settings-radio-title\">%1</div>", title);
        rml += std::string("</div></div>");
    };

    renderInputModeRadio(std::string("voice_activity"), std::string("Voice Activity"));
    renderInputModeRadio(std::string("push_to_talk"), std::string("Push to Talk"));

    if (inputMode == std::string("push_to_talk")) {
        rml += std::string("<div class=\"settings-label\">Shortcut</div>");
        renderKeybindRow(rml, KeybindAction::PushToTalk);

        const auto *ptt = shell.pushToTalk();
        const std::string scopeDesc = ptt ? ptt->shortcutScopeDescription() : std::string("Push to talk works while Styx is the focused window.");
        rml += formatRml("<div class=\"settings-description\">%1</div>", Rml::escape(scopeDesc));

        const int pttDelay = getInt(Keys::VoicePushToTalkReleaseDelay, Defaults::VoicePushToTalkReleaseDelay);
        rml += std::string("<div class=\"settings-slider-wrap\">");
        rml += std::string("<div class=\"settings-slider-header\">");
        rml += std::string("<span class=\"settings-label\">Push to Talk Release Delay</span>");
        rml += std::string("<span class=\"settings-slider-val\" id=\"val-ptt-release-delay\">") + std::to_string(pttDelay) + std::string("ms</span>");
        rml += std::string("</div>");
        rml += std::string("<input type=\"range\" class=\"settings-slider\" min=\"0\" max=\"2000\" step=\"20\" value=\"")
               + std::to_string(pttDelay)
               + std::string("\" data-setting=\"") + std::string(Keys::VoicePushToTalkReleaseDelay)
               + std::string("\" data-display=\"val-ptt-release-delay\" data-unit=\"ms\"/>");
        rml += formatRml("<div class=\"settings-slider-hint\">%1ms</div>", pttDelay);
        rml += std::string("</div>"); // .settings-slider-wrap
    }

    rml += std::string("</div>"); // .settings-card

    // Advanced Subheading & Audio Backend card
    rml += std::string("<div class=\"settings-subheading\">Advanced</div>");
    rml += std::string("<div class=\"settings-card\">");

    const std::string currentBackend = getString(Keys::AudioBackend, Defaults::AudioBackend);
    rml += std::string("<div class=\"settings-row\">");
    rml += std::string("<div class=\"settings-label\">Audio Backend</div>");
    rml += formatRml("<select class=\"settings-select\" data-setting=\"%1\">", Keys::AudioBackend);
    rml += formatRml("<option value=\"\"%1>Automatic</option>", currentBackend.empty() ? std::string(" selected") : std::string());
#ifndef STYX_NO_VOICE
    for (const std::string &name : Core::Audio::supportedAudioBackends()) {
        std::string sel = (currentBackend == name) ? std::string(" selected") : std::string();
        rml += formatRml("<option value=\"%1\"%2>%3</option>", name, sel, name);
    }
#endif
    rml += std::string("</select>");
    rml += std::string("<div class=\"settings-note\">* Takes effect after restart</div>");
    rml += std::string("</div>"); // .settings-row
    rml += std::string("</div>"); // .settings-card
}


void SettingsController::renderKeybinds(std::string &rml)
{
    rml += std::string("<div class=\"settings-description\">Record a shortcut for each action. Mute and deafen shortcuts work while Styx is focused.</div>");
    rml += std::string("<div class=\"settings-card\">");

    rml += formatRml("<div class=\"settings-label\">%1</div>", Rml::escape(keybindName(KeybindAction::PushToTalk)));
    renderKeybindRow(rml, KeybindAction::PushToTalk);
    if (const auto *ptt = shell.pushToTalk()) {
        const std::string scopeDesc = ptt->shortcutScopeDescription();
        if (!scopeDesc.empty())
            rml += formatRml("<div class=\"settings-description\">%1</div>", Rml::escape(scopeDesc));
    }

    rml += formatRml("<div class=\"settings-label\">%1</div>", Rml::escape(keybindName(KeybindAction::ToggleMute)));
    renderKeybindRow(rml, KeybindAction::ToggleMute);

    rml += formatRml("<div class=\"settings-label\">%1</div>", Rml::escape(keybindName(KeybindAction::ToggleDeafen)));
    renderKeybindRow(rml, KeybindAction::ToggleDeafen);

    rml += std::string("</div>"); // .settings-card
}

void SettingsController::renderKeybindRow(std::string &rml, KeybindAction action)
{
    const auto &configured = ShortcutController::keybinds();
    const auto &binding = configured.get(action);
    const int actionIdx = static_cast<int>(action);

    const bool isRecording = recordingKeybind.has_value() && *recordingKeybind == action;

    std::string label;
    if (isRecording) {
        label = "Press a key…";
    } else {
        label = keyBindingLabel(binding);
        if (label.empty())
            label = "No Keybind Set";
    }

    const std::string btnClass = isRecording ? "settings-btn settings-btn-danger" : "settings-btn settings-btn-secondary";
    const std::string btnText = isRecording ? "Stop Recording" : "Record Keybind";

    rml += std::string("<div class=\"settings-keybind\">");
    rml += formatRml("<span class=\"settings-keybind-label\">%1</span>", Rml::escape(label));
    rml += std::string("<div class=\"settings-keybind-actions\">");
    rml += formatRml("<button class=\"%1\" id=\"record-keybind-%2\" data-action=\"record-keybind\" data-keybind=\"%2\"><span class=\"settings-btn-label\">%3</span></button>",
                     btnClass, actionIdx, btnText);
    if (binding.valid()) {
        rml += formatRml("<button class=\"settings-btn settings-btn-danger\" data-action=\"remove-keybind\" data-keybind=\"%1\"><span class=\"settings-btn-label\">Remove</span></button>",
                         actionIdx);
    }
    rml += std::string("</div>"); // .settings-keybind-actions
    rml += std::string("</div>"); // .settings-keybind

    const auto errorText = isRecording ? keybindError : keybindConflictText(action, binding, configured);
    if (!errorText.empty())
        rml += formatRml("<div class=\"settings-keybind-error\">%1</div>", Rml::escape(errorText));
}

void SettingsController::renderGoLive(std::string &rml)
{

    rml += std::string("<div class=\"settings-card\">");
    rml += std::string("<div class=\"settings-label\">Screenshare Quality</div>");
    rml += std::string("<div class=\"settings-description\">Default resolution and frame rate when sharing a screen or application window.</div>");

    const std::string currentPreset = getString(Keys::StreamQuality, Defaults::StreamQuality);
    rml += std::string("<div class=\"settings-row\">");
    rml += formatRml("<select class=\"settings-select\" data-setting=\"%1\">", Keys::StreamQuality);
    for (const auto &preset : Core::Stream::StreamQuality::presets()) {
        std::string sel = (preset.id == currentPreset) ? std::string(" selected") : std::string();
        rml += formatRml("<option value=\"%1\"%2>%3</option>", preset.id, sel, preset.label());
    }
    rml += std::string("</select>");
    rml += std::string("</div>");

    const bool shareAudio = getBool(Keys::StreamAudio, Defaults::StreamAudio);
    rml += std::string("<div class=\"settings-switch-row\">");
    rml += std::string("<div class=\"settings-switch-text\">");
    rml += std::string("<div class=\"settings-switch-title\">Share system audio</div>");
    rml += std::string("<div class=\"settings-switch-desc\">Capture application sound when sharing a window, or system audio when sharing screen.</div>");
    rml += std::string("</div>");
    rml += formatRml("<div class=\"settings-switch%1\" data-setting=\"%2\" tabindex=\"0\"><div class=\"settings-switch-thumb\"></div></div>", shareAudio ? std::string(" checked") : std::string(), Keys::StreamAudio);
    rml += std::string("</div>");

    rml += std::string("</div>"); // .settings-card
}

void SettingsController::renderAdvanced(std::string &rml)
{
    if (auto *instance = shell.currentInstance(); instance && instance->discord()->getMe().id.get() == instance->accountId()) {
        const auto &appearance = instance->discord()->getSettings().appearance;
        appendSwitch(rml, "discord/developer-mode", appearance && appearance->developerMode, "Developer Mode",
            "Show Copy ID actions in message, user, channel and server menus.", remoteSettingsSaving);
        renderRemoteStatus(rml);
    }

    rml += std::string("<div class=\"settings-card\">");

    // In-memory cache
    const bool inMemory = getBool(Keys::InMemoryCache, Defaults::InMemoryCache);
    rml += std::string("<div class=\"settings-switch-row\">");
    rml += std::string("<div class=\"settings-switch-text\">");
    rml += std::string("<div class=\"settings-switch-title\">In-memory cache database</div>");
    rml += std::string("<div class=\"settings-switch-desc\">Store temporary cache in RAM (:memory:) instead of sqlite file on disk.</div>");
    rml += std::string("<div class=\"settings-note\">* Requires restart</div>");
    rml += std::string("</div>");
    rml += formatRml("<div class=\"settings-switch%1\" data-setting=\"%2\" tabindex=\"0\"><div class=\"settings-switch-thumb\"></div></div>", inMemory ? std::string(" checked") : std::string(), Keys::InMemoryCache);
    rml += std::string("</div>");
    rml += "<div class=\"settings-label\">Animation memory</div>";
    auto renderLimit = [&](const char *key, const char *id, const char *title, const Core::MiBRange &range) {
        const int value = std::clamp(getInt(key, range.fallback), range.min, range.max);
        rml += formatRml("<div class=\"settings-slider-wrap\"><div class=\"settings-slider-header\">"
            "<span class=\"settings-label\">%1</span><span class=\"settings-slider-val\" id=\"%2\">%3 MiB</span></div>"
            "<input type=\"range\" class=\"settings-slider\" min=\"%4\" max=\"%5\" step=\"4\" value=\"%3\" "
            "data-setting=\"%6\" data-display=\"%2\" data-unit=\" MiB\"/><div class=\"settings-slider-hint\">%3 MiB</div></div>",
            title, id, value, range.min, range.max, key);
    };
    renderLimit(Keys::AnimationCacheMb, "val-anim-cache-mb", "Memory for cached animations", Core::AnimatedImageCache::CacheLimit);
    renderLimit(Keys::AnimationMaxMb, "val-anim-max-mb", "Largest decoded animation", Core::AnimatedImageCache::AnimationLimit);



    rml += std::string("</div>"); // .settings-card
}

void SettingsController::setupEventListeners()
{
    listen("mousedown", [this](::Rml::Event &event) {
        if (event.GetParameter<int>("button", 0) != 0)
            return;
        auto *plane = regionRoot->GetElementById("seed-color-plane");
        for (auto *target = event.GetTargetElement(); target; target = target->GetParentNode()) {
            if (target != plane)
                continue;
            colorDragging = true;
            plane->Focus();
            pickColorAt(event.GetParameter<float>("mouse_x", 0), event.GetParameter<float>("mouse_y", 0));
            event.StopPropagation();
            break;
        }
    }, regionRoot);
    listen("mousemove", [this](::Rml::Event &event) {
        if (colorDragging)
            pickColorAt(event.GetParameter<float>("mouse_x", 0), event.GetParameter<float>("mouse_y", 0));
    }, regionRoot);
    listen("mouseup", [this](::Rml::Event &) { colorDragging = false; }, regionRoot);
    listen("click", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        if (!target)
            return;

        // Close button
        if (auto *btn = closest(target, "data-action")) {
            std::string action = attribute(btn, "data-action");
            handleAction(action, btn);
            return;
        }

        // Section tab item
        if (auto *item = closest(target, "data-section")) {
            selectSection(attribute(item, "data-section"));
            return;
        }

        // Switch toggle
        if (auto *switchEl = closest(target, "data-setting")) {
            ::Rml::Element *sw = switchEl->IsClassSet("settings-switch") ? switchEl : nullptr;
            if (!sw) {
                ::Rml::ElementList list;
                switchEl->GetElementsByClassName(list, "settings-switch");
                if (!list.empty())
                    sw = list[0];
            }
            if (sw) {
                handleSwitchClick(sw);
                return;
            }
        }

        // Radio card
        if (auto *radioEl = closest(target, "data-value")) {
            if (radioEl->IsClassSet("settings-radio-card")) {
                handleRadioClick(radioEl);
                return;
            }
        }
    }, regionRoot);

    listen("change", [this](::Rml::Event &event) {
        if (renderingControls)
            return;
        auto *target = event.GetTargetElement();
        if (!target)
            return;

        if (target->GetId() == "settings-search") {
            if (auto *input = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(target))
                searchQuery = input->GetValue();
            refreshNavigation();
            return;
        }
        if (target->GetId() == "seed-color-hex") {
            auto *input = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(target);
            const auto color = input ? Platform::Color::parse(input->GetValue()) : Platform::Color{};
            if (color.valid) {
                themeSeedColor = color;
                readPickerColor();
                refreshColorPicker(false);
            }
            return;
        }
        if (target->GetId() == "seed-color-hue") {
            auto *input = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(target);
            float hue = 0;
            const auto value = input ? input->GetValue() : std::string{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), hue);
            if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && std::isfinite(hue)) {
                pickerHue = std::clamp(double(hue), 0.0, 360.0);
                applyPickerColor();
            }
            return;
        }
        if (target->GetTagName() == "input") {
            handleSliderChange(target);
        } else if (target->GetTagName() == "select") {
            handleSelectChange(target);
        }
    }, regionRoot);
    auto sliderTarget = [](auto *target) -> ::Rml::Element * {
        for (auto *el = target; el; el = el->GetParentNode()) {
            if (el->IsClassSet("settings-slider"))
                return el;
        }
        return nullptr;
    };
    listen("mouseover", [this, sliderTarget](::Rml::Event &event) {
        auto *slider = sliderTarget(event.GetTargetElement());
        if (slider && (!sliderDragging || hintSlider == slider))
            showSliderHint(slider);
    }, regionRoot);
    listen("mouseout", [this](::Rml::Event &event) {
        if (!sliderDragging && event.GetTargetElement() == hintSlider)
            clearSliderHint();
    }, regionRoot);
    listen("focus", [this, sliderTarget](::Rml::Event &event) {
        if (auto *slider = sliderTarget(event.GetTargetElement()))
            showSliderHint(slider);
    }, regionRoot, true);
    listen("blur", [this](::Rml::Event &event) {
        if (!sliderDragging && event.GetTargetElement() == hintSlider)
            clearSliderHint();
    }, regionRoot, true);
    listen("dragstart", [this, sliderTarget](::Rml::Event &event) {
        if (auto *slider = sliderTarget(event.GetTargetElement())) {
            sliderDragging = true;
            showSliderHint(slider);
        }
    }, regionRoot);
    listen("dragend", [this](::Rml::Event &event) {
        if (!sliderDragging)
            return;
        sliderDragging = false;
        if (hintSlider) {
            const float x = event.GetParameter<float>("mouse_x", -1);
            const float y = event.GetParameter<float>("mouse_y", -1);
            if (x < hintSlider->GetAbsoluteLeft() || x > hintSlider->GetAbsoluteLeft() + hintSlider->GetOffsetWidth() ||
                y < hintSlider->GetAbsoluteTop() || y > hintSlider->GetAbsoluteTop() + hintSlider->GetOffsetHeight())
                clearSliderHint();
        }
    }, regionRoot);

    listen("keyup", [this](::Rml::Event &event) {
        const int key = event.GetParameter<int>("key_identifier", 0);
        if (recordingKeybind && isModifierKey(key))
            recordConfiguredKey(key, ShortcutController::eventModifiers(event));
    }, regionRoot);

    listen("keydown", [this](::Rml::Event &event) {
        if (recordingKeybind) {
            const int key = event.GetParameter<int>("key_identifier", 0);
            if (key == ::Rml::Input::KeyIdentifier::KI_ESCAPE) {
                stopRecordingKeybind();
                refreshKeybinds();
            } else if (!isModifierKey(key)) {
                recordConfiguredKey(key, ShortcutController::eventModifiers(event));
            }
            event.StopPropagation();
            return;
        }

        const int key = event.GetParameter("key_identifier", 0);
        if (key == ::Rml::Input::KeyIdentifier::KI_ESCAPE) {
            for (auto *target = event.GetTargetElement(); target && target != regionRoot; target = target->GetParentNode())
                if (auto *select = rmlui_dynamic_cast<::Rml::ElementFormControlSelect *>(target); select && select->IsSelectBoxVisible())
                    return;
        }
        if (event.GetTargetElement()->GetId() == "settings-search" &&
            (key == ::Rml::Input::KeyIdentifier::KI_RETURN || key == ::Rml::Input::KeyIdentifier::KI_NUMPADENTER)) {
            event.StopPropagation();
            if (auto *first = regionRoot->QuerySelector("#settings-navigation .settings-nav-item"))
                selectSection(attribute(first, "data-section"));
            return;
        }
        if (event.GetTargetElement()->GetId() == "seed-color-plane") {
            const double step = event.GetParameter<bool>("shift_key", false) ? 0.1 : 0.01;
            bool adjusted = true;
            if (key == ::Rml::Input::KeyIdentifier::KI_LEFT) pickerSaturation -= step;
            else if (key == ::Rml::Input::KeyIdentifier::KI_RIGHT) pickerSaturation += step;
            else if (key == ::Rml::Input::KeyIdentifier::KI_UP) pickerBrightness += step;
            else if (key == ::Rml::Input::KeyIdentifier::KI_DOWN) pickerBrightness -= step;
            else adjusted = false;
            if (adjusted) {
                pickerSaturation = std::clamp(pickerSaturation, 0.0, 1.0);
                pickerBrightness = std::clamp(pickerBrightness, 0.0, 1.0);
                applyPickerColor();
                event.StopPropagation();
                return;
            }
        }
        if (key == ::Rml::Input::KeyIdentifier::KI_ESCAPE && colorPickerOpen) {
            handleAction("toggle-color-picker", nullptr);
            if (auto *swatch = regionRoot->GetElementById("seed-color-toggle")) swatch->Focus();
            event.StopPropagation();
            return;
        }
        if (key == ::Rml::Input::KeyIdentifier::KI_ESCAPE) {
            handleAction("close", nullptr);
            event.StopPropagation();
            return;
        }
        if (key == ::Rml::Input::KeyIdentifier::KI_TAB) {
            ::Rml::ElementList controls;
            regionRoot->QuerySelectorAll(controls, ".settings-nav-item, .settings-switch, .settings-radio-card, #seed-color-plane, button, input, select, textarea");
            std::erase_if(controls, [](auto *element) { return !element->IsVisible(true) || element->HasAttribute("disabled"); });
            if (controls.empty()) return;
            auto position = std::find(controls.begin(), controls.end(), regionRoot->GetContext()->GetFocusElement());
            const bool backwards = event.GetParameter<bool>("shift_key", false);
            const int count = static_cast<int>(controls.size());
            const int index = position == controls.end() ? (backwards ? 0 : count - 1) : static_cast<int>(position - controls.begin());
            auto *next = controls[(index + (backwards ? -1 : 1) + count) % count];
            next->Focus();
            next->ScrollIntoView();
            event.StopPropagation();
            return;
        }

        if (key == ::Rml::Input::KeyIdentifier::KI_SPACE || key == ::Rml::Input::KeyIdentifier::KI_RETURN ||
            key == ::Rml::Input::KeyIdentifier::KI_NUMPADENTER) {
            auto *target = event.GetTargetElement();
            if (!target)
                return;
            if (target->HasAttribute("disabled")) return;
            if (target->IsClassSet("settings-switch") || target->IsClassSet("settings-radio-card") ||
                target->IsClassSet("settings-nav-item") || target->IsClassSet("settings-close-btn") ||
                target->IsClassSet("settings-btn")) {
                event.StopPropagation();
                target->Click();
            }
        }
    }, regionRoot, true);
}

void SettingsController::handleSwitchClick(::Rml::Element *switchEl)
{
    if (switchEl->HasAttribute("disabled") || handleRemoteSwitch(switchEl))
        return;
    const std::string key = attribute(switchEl, "data-setting");
    if (key.empty())
        return;

    const bool wasChecked = switchEl->IsClassSet("checked");
    const bool nowChecked = !wasChecked;
    switchEl->SetClass("checked", nowChecked);
    if (key == "system/launch-at-login" || key == Keys::StartMinimized) {
        const bool enabled = key == "system/launch-at-login" ? nowChecked : Platform::Desktop::launchAtLoginEnabled();
        const bool minimized = key == Keys::StartMinimized ? nowChecked : getBool(Keys::StartMinimized, false);
        if (!Platform::Desktop::configureLaunchAtLogin(enabled, minimized))
            remoteSettingsError = "Could not update the Windows startup entry.";
        else {
            remoteSettingsError.clear();
            if (key == Keys::StartMinimized) setBool(key, minimized);
        }
        render();
        requestRender();
        return;
    }

    setBool(key, nowChecked);
    switchEl->SetAttribute("aria-checked", nowChecked ? "true" : "false");
#ifdef STYX_HAVE_MINIAUDIO
    if (key == Keys::SoundsEnabled) {
        if (!nowChecked) Core::Sounds::stopAll();
        render();
        requestRender();
        return;
    }
#endif

    // Immediate action application
    if (key == std::string(Keys::CloseToTray)) {
        shell.applyCloseToTray();
    } else if (key == std::string(Keys::TaskbarBadge)) {
        shell.refreshTaskbarBadge();
    } else if (key == std::string(Keys::ThemeUseSystemColors)) {
        Core::Theme::Manager::instance().setUseSystemColors(nowChecked);
        Core::Theme::Manager::instance().apply();
    } else if (key == std::string(Keys::ThemeFollowDevice)) {
        applyAppearanceChoice(Core::Theme::loadAppearance(), false);
    } else if (key == std::string(Keys::ThemeSync)) {
        // Turning sync on publishes this device's default theme to the account.
        const auto appearance = Core::Theme::loadAppearance();
        if (!Core::Theme::storeAppearance(appearance))
            appearanceSaveError = "Theme sync changed, but it could not be saved.";
        if (nowChecked)
            pushThemeToAccount(appearance.choice);
        render();
    }

    requestRender(100);
}

void SettingsController::handleRadioClick(::Rml::Element *radioEl)
{
    const std::string key = attribute(radioEl, "data-setting");
    const std::string val = attribute(radioEl, "data-value");
    if (key.empty() || val.empty())
        return;

    setString(key, val);

    // Update sibling active classes
    if (auto *parent = radioEl->GetParentNode()) {
        for (int i = 0; i < parent->GetNumChildren(); ++i) {
            auto *child = parent->GetChild(i);
            if (child->IsClassSet("settings-radio-card"))
                child->SetClass("active", child == radioEl);
        }
    }

    if (key == std::string(Keys::VoiceInputMode)) {
        applyPushToTalk();
        render();
    }

    requestRender(100);
}

void SettingsController::clearSliderHint()
{
    if (hintSlider && hintSlider->GetParentNode())
        hintSlider->GetParentNode()->SetClass("hint-visible", false);
    hintSlider = nullptr;
    sliderDragging = false;
    requestRender();
}

void SettingsController::showSliderHint(::Rml::Element *sliderEl)
{
    auto *input = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(sliderEl);
    auto *wrap = sliderEl ? sliderEl->GetParentNode() : nullptr;
    if (!input || !wrap)
        return;

    ::Rml::ElementList hints;
    wrap->GetElementsByClassName(hints, "settings-slider-hint");
    if (hints.empty())
        return;

    float value = 0;
    const auto text = input->GetValue();
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value))
        return;

    if (hintSlider && hintSlider != sliderEl && hintSlider->GetParentNode())
        hintSlider->GetParentNode()->SetClass("hint-visible", false);
    hintSlider = sliderEl;
    auto *hint = hints[0];
    const auto label = attribute(sliderEl, "data-prefix") + std::to_string(static_cast<int>(std::lround(value))) +
                       attribute(sliderEl, "data-unit");
    hint->SetInnerRML(Platform::htmlEscape(label));
    const float textWidth = static_cast<float>(::Rml::ElementUtilities::GetStringWidth(hint, label));
    hint->SetProperty(::Rml::PropertyId::Width, ::Rml::Property(textWidth, ::Rml::Unit::PX));
    wrap->SetClass("hint-visible", true);

    ::Rml::Element *track = nullptr;
    ::Rml::Element *bar = nullptr;
    for (int i = 0; i < sliderEl->GetNumChildren(true); ++i) {
        auto *child = sliderEl->GetChild(i);
        if (child->GetTagName() == "slidertrack")
            track = child;
        else if (child->GetTagName() == "sliderbar")
            bar = child;
    }
    if (track && bar) {
        const float min = sliderEl->GetAttribute<float>("min", 0);
        const float max = sliderEl->GetAttribute<float>("max", 100);
        const float fraction = max > min ? std::clamp((value - min) / (max - min), 0.0f, 1.0f) : 0;
        const float leftEdge = bar->GetBox().GetEdge(::Rml::BoxArea::Margin, ::Rml::BoxEdge::Left);
        const float rightEdge = bar->GetBox().GetEdge(::Rml::BoxArea::Margin, ::Rml::BoxEdge::Right);
        const float barWidth = bar->GetBox().GetSize(::Rml::BoxArea::Border).x;
        const float trackWidth = track->GetBox().GetSize(::Rml::BoxArea::Content).x;
        const float center = track->GetAbsoluteLeft() - wrap->GetAbsoluteLeft() + leftEdge +
                             (trackWidth - barWidth - leftEdge - rightEdge) * fraction + barWidth / 2;
        const float width = textWidth + hint->GetBox().GetFrameSize(::Rml::BoxArea::Border).x +
                            hint->GetBox().GetFrameSize(::Rml::BoxArea::Padding).x;
        const float left = std::clamp(center - width / 2, 0.0f, std::max(0.0f, wrap->GetClientWidth() - width));
        hint->SetProperty(::Rml::PropertyId::Left, ::Rml::Property(std::floor(left), ::Rml::Unit::PX));
    }
    requestRender();
}

void SettingsController::handleSliderChange(::Rml::Element *sliderEl)
{
    const std::string key = attribute(sliderEl, "data-setting");
    if (key.empty())
        return;

    auto *input = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(sliderEl);
    if (!input)
        return;

    float value = 0;
    const auto text = input->GetValue();
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value))
        return;
    const int val = static_cast<int>(std::lround(value));
    setInt(key, val);

    // Update display label
    const std::string dispId = attribute(sliderEl, "data-display");
    if (!dispId.empty() && regionRoot) {
        if (auto *labelEl = regionRoot->GetElementById(dispId)) {
            const std::string prefix = attribute(sliderEl, "data-prefix");
            const std::string unit = attribute(sliderEl, "data-unit");
            labelEl->SetInnerRML((prefix + std::to_string(val) + unit).c_str());
        }
    }
    if (hintSlider == sliderEl)
        showSliderHint(sliderEl);

    // Apply immediate settings
    if (key == Keys::AnimationCacheMb) {
        shell.session()->getAnimatedImageCache()->setCacheLimitMiB(val);
    } else if (key == Keys::AnimationMaxMb) {
        shell.session()->getAnimatedImageCache()->setAnimationLimitMiB(val);
    } else if (key == "accessibility/message-font-px") {
        auto &theme = Core::Theme::Manager::instance();
        auto font = theme.font(Core::Theme::FontRole::Message);
        font.pointSize = std::clamp(val, 12, 24) * Core::Theme::fontDescriptor(Core::Theme::FontRole::Message).defaultFont.pointSize / 16.0;
        theme.setFontOverride(Core::Theme::FontRole::Message, font);
        theme.applyFonts();
        theme.save();
    } else if (key == std::string(Keys::VoicePushToTalkReleaseDelay)) {
        applyPushToTalk();
#ifndef STYX_NO_VOICE
    } else if (key == std::string(Keys::VoiceInputVolume)) {
        const float gain = static_cast<float>(val) / 100.0f;
        for (const auto &client : shell.session()->getClients()) {
            if (client && client->voice())
                client->voice()->setInputGain(gain);
        }
    } else if (key == std::string(Keys::VoiceOutputVolume)) {
        const float vol = static_cast<float>(val) / 100.0f;
        for (const auto &client : shell.session()->getClients()) {
            if (client && client->voice())
                client->voice()->setOutputVolume(vol);
        }
#endif
    }
    requestRender(50);
}

void SettingsController::handleSelectChange(::Rml::Element *selectEl)
{
    const std::string key = attribute(selectEl, "data-setting");
    auto *sel = rmlui_dynamic_cast<::Rml::ElementFormControlSelect *>(selectEl);
    if (!sel)
        return;

    const std::string val = sel->GetValue();

    if (key == "discord/locale") {
        Proto::PreloadedSettingsEdit edit;
        edit.locale = val;
        patchRemoteSettings(std::move(edit));
        return;
    }
    if (!key.empty()) {
        setString(key, val);
#ifndef STYX_NO_VOICE
        if (key == std::string(Keys::VoiceInputDevice)) {
            std::string devBytes = hexDecode(val);
            for (const auto &client : shell.session()->getClients()) {
                if (client && client->voice())
                    client->voice()->setInputDevice(devBytes);
            }
        } else if (key == std::string(Keys::VoiceOutputDevice)) {
            std::string devBytes = hexDecode(val);
            for (const auto &client : shell.session()->getClients()) {
                if (client && client->voice())
                    client->voice()->setOutputDevice(devBytes);
            }
        } else if (key == std::string(Keys::AudioBackend)) {
            Core::Audio::setConfiguredAudioBackend(val);
        }
#endif
    }

    // Custom theme draft: an active custom theme updates live; otherwise Apply custom theme uses the draft.
    const bool customActive = Core::Theme::loadAppearance().choice == Core::Theme::ThemeChoice::Custom;
    if (selectEl->GetId() == "theme-scheme-select") {
        const int scheme = std::stoi(val);
        if (themeSchemeIndex != scheme) {
            themeSchemeIndex = scheme;
            if (customActive)
                applyGeneratedTheme();
        }
    } else if (selectEl->GetId() == "theme-mode-select") {
        const bool dark = (val == "dark");
        if (themeDarkMode != dark) {
            themeDarkMode = dark;
            if (customActive)
                applyGeneratedTheme();
        }
    }

    requestRender(50);
}

void SettingsController::handleAction(const std::string &action, ::Rml::Element *target)
{
    if (action == "clear-settings-search") {
        searchQuery.clear();
        refreshNavigation();
        if (auto *input = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(regionRoot->GetElementById("settings-search"))) {
            input->SetValue("");
            input->Focus();
        }
        return;
    }
    if (action == "toggle-color-picker") {
        colorPickerOpen = !colorPickerOpen;
        colorDragging = false;
        if (auto *picker = regionRoot->GetElementById("seed-color-picker"))
            picker->SetProperty("display", colorPickerOpen ? "block" : "none");
        if (auto *swatch = regionRoot->GetElementById("seed-color-toggle"))
            swatch->SetAttribute("aria-expanded", colorPickerOpen ? "true" : "false");
        if (colorPickerOpen)
            if (auto *plane = regionRoot->GetElementById("seed-color-plane")) plane->Focus();
        requestRender();
    } else if (action == std::string("close")) {
        Platform::Guard<SettingsController> self(this);
        if (confirmProfileLeave([self] { if (self) self->shell.closeSettings(); }))
            shell.closeSettings();
#ifdef STYX_HAVE_MINIAUDIO
    } else if (action == "preview-sound") {
        const int sound = target ? target->GetAttribute<int>("data-sound", -1) : -1;
        if (sound >= 0 && sound <= static_cast<int>(Core::Sounds::Sound::Leave))
            Core::Sounds::preview(static_cast<Core::Sounds::Sound>(sound));
#endif
    } else if (action == std::string("record-keybind")) {
        const int rawAction = target ? target->GetAttribute<int>("data-keybind", -1) : -1;
        if (rawAction < 0 || rawAction > 2)
            return;
        const auto act = static_cast<KeybindAction>(rawAction);
        if (recordingKeybind && *recordingKeybind == act) {
            stopRecordingKeybind();
        } else {
            recordingKeybind = act;
            keybindError.clear();
            if (auto *ptt = shell.pushToTalk())
                ptt->setSuspended(true);
        }
        refreshKeybinds();
    } else if (action == std::string("remove-keybind")) {
        const int rawAction = target ? target->GetAttribute<int>("data-keybind", -1) : -1;
        if (rawAction < 0 || rawAction > 2)
            return;
        const auto act = static_cast<KeybindAction>(rawAction);
        if (auto *ptt = shell.pushToTalk())
            ptt->setSuspended(true);
        storeKeybind(act, KeyBinding{});
        applyPushToTalk();
        stopRecordingKeybind();
        refreshKeybinds();
    } else if (action == std::string("edit-profile")) {
        setSection(Section::Profiles);
    } else if (action == std::string("manage-accounts")) {
        shell.openAccounts();
    } else if (action == std::string("test-notification")) {
        Platform::Notifications::Notification notification;
        notification.title = "Styx";
        notification.body = "This is how notifications will look.";
        Platform::Notifications::show(notification, {});
        Core::Sounds::play(Core::Sounds::Sound::Message);
    } else if (action == std::string("generate-theme")) {
        applyGeneratedTheme();
    } else if (action == std::string("select-theme")) {
        const auto choice = Core::Theme::choiceFromId(attribute(target, "data-theme"));
        if (choice && *choice != Core::Theme::ThemeChoice::Custom)
            selectDefaultTheme(*choice);
    } else if (action == std::string("random-theme")) {
        std::random_device rng;
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        const double h = unit(rng) * 6.0;
        const double saturation = 0.55 + unit(rng) * 0.40;
        const double lightness = 0.45 + unit(rng) * 0.20;
        const double chroma = (1.0 - std::abs(2.0 * lightness - 1.0)) * saturation;
        const double x = chroma * (1.0 - std::abs(std::fmod(h, 2.0) - 1.0));
        const double m = lightness - chroma / 2.0;
        const double r = h < 1 ? chroma : h < 2 ? x : h < 4 ? 0 : h < 5 ? x : chroma;
        const double g = h < 1 ? x : h < 3 ? chroma : h < 4 ? x : 0;
        const double b = h < 2 ? 0 : h < 3 ? x : h < 5 ? chroma : x;
        themeSeedColor = Platform::Color{static_cast<uint8_t>((r + m) * 255), static_cast<uint8_t>((g + m) * 255), static_cast<uint8_t>((b + m) * 255), 255, true};
        readPickerColor();
        themeSchemeIndex = std::uniform_int_distribution<int>(0, Core::Theme::schemeCount - 1)(rng);
        if (regionRoot) {
            if (auto *input = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(regionRoot->GetElementById("seed-color-hex")))
                input->SetValue(themeSeedColor.css());
        }
        applyGeneratedTheme();
    } else if (action == std::string("reset-theme")) {
        // Clears the custom draft and returns to the default theme: the account's when synced, otherwise Onyx.
        themeSeedColor = Platform::Color::rgb(0x5865f2);
        readPickerColor();
        themeDarkMode = true;
        themeSchemeIndex = 0;
        themeError.clear();
        Core::Theme::Appearance appearance;
        appearance.syncTheme = Core::Theme::loadAppearance().syncTheme;
        appearance.seed = themeSeedColor;
        if (auto *client = readyClient(shell); client && appearance.syncTheme && client->getSettings().appearance)
            appearance.choice = Core::Theme::choiceFromProto(client->getSettings().appearance->theme).value_or(appearance.choice);
        applyAppearanceChoice(appearance, true);
    } else if (action == std::string("font-size-inc") || action == std::string("font-size-dec")) {
        int roleInt = std::stoi(attribute(target, "data-role"));
        auto role = static_cast<Core::Theme::FontRole>(roleInt);
        auto f = Core::Theme::Manager::instance().font(role);
        int curPt = static_cast<int>(f.pointSize);
        int newPt = (action == std::string("font-size-inc")) ? curPt + 1 : std::max(6, curPt - 1);
        f.pointSize = newPt;
        Core::Theme::Manager::instance().setFontOverride(role, f);
        Core::Theme::Manager::instance().applyFonts();
        Core::Theme::Manager::instance().save();
        render();
        requestRender(100);
    } else if (action == std::string("reset-font")) {
        int roleInt = std::stoi(attribute(target, "data-role"));
        auto role = static_cast<Core::Theme::FontRole>(roleInt);
        Core::Theme::Manager::instance().clearFontOverride(role);
        Core::Theme::Manager::instance().applyFonts();
        Core::Theme::Manager::instance().save();
        render();
        requestRender(100);
    }
}

void SettingsController::refreshKeybinds()
{
    render();
    if (recordingKeybind && regionRoot) {
        const std::string btnId = "record-keybind-" + std::to_string(static_cast<int>(*recordingKeybind));
        if (auto *btn = regionRoot->GetElementById(btnId))
            btn->Focus();
    }
    requestRender(100);
}

void SettingsController::recordConfiguredKey(int key, int rmlModifiers)
{
    if (!recordingKeybind)
        return;

    const auto binding = recordKeyBinding(key, rmlModifiers);
    if (!binding.valid())
        return;

    const auto action = *recordingKeybind;
    const auto configured = loadKeybinds();
    const auto conflict = keybindConflictText(action, binding, configured);
    if (!conflict.empty()) {
        keybindError = conflict;
        refreshKeybinds();
        return;
    }

    storeKeybind(action, binding);
    applyPushToTalk();
    stopRecordingKeybind();
    refreshKeybinds();
}

void SettingsController::stopRecordingKeybind()
{
    recordingKeybind.reset();
    keybindError.clear();
    if (auto *ptt = shell.pushToTalk(); ptt && ptt->isSuspended())
        ptt->setSuspended(false);
}

void SettingsController::applyPushToTalk()
{
    ShortcutController::reloadKeybinds();

    if (auto *ptt = shell.pushToTalk())
        ptt->reload();

#ifndef STYX_NO_VOICE
    const std::string mode = getString(Keys::VoiceInputMode, Defaults::VoiceInputMode);
    const int delay = getInt(Keys::VoicePushToTalkReleaseDelay, Defaults::VoicePushToTalkReleaseDelay);
    const bool pttEnabled = (mode == "push_to_talk");
    if (auto *sess = shell.session()) {
        for (const auto &client : sess->getClients()) {
            if (client && client->voice())
                client->voice()->setPushToTalk(pttEnabled, delay);
        }
    }
#endif
}

} // namespace Rml
} // namespace UI
} // namespace Styx
