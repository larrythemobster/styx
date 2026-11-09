#include "SettingsController.hpp"

#include "ProfileEditorController.hpp"
#include "Shell.hpp"
#include "ShortcutController.hpp"
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

#include "Platform/Desktop.hpp"
#include "Platform/Notifications.hpp"
#include <algorithm>
#include <cmath>
#include <random>
#include <sstream>
#include <vector>
#include <charconv>

#ifndef STYX_NO_VOICE
#include "Core/Audio/AudioBackends.hpp"
#include "Core/Audio/IAudioBackend.hpp"
#include "Core/Audio/VoiceManager.hpp"
#endif

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
}



SettingsController::SettingsController(Shell &shell, ::Rml::Element *root)
    : Controller(shell, root)
{
    themeSeedColor = Core::Theme::Manager::instance().color(Core::Theme::Token::Highlight);
    track(shell.settingsOpenChanged.connect([this](bool open) { onSettingsOpenChanged(open); }));
    track(shell.instanceChanged.connect([this](Core::ClientInstance *) {
        if (this->shell.isSettingsOpen())
            rebuildTimer.start(std::chrono::milliseconds(0), false, [this] { rebuild(); });
    }));

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
        hideTransitionTimer.stop();
        regionRoot->SetProperty("display", "flex");
        render();
        regionRoot->SetClass("open", true);
        requestRender(200);
    } else {
        if (profileEditor)
            profileEditor->close();
        stopRecordingKeybind();
        regionRoot->SetClass("open", false);
        hideTransitionTimer.start(std::chrono::milliseconds(160), false, [this] {
            if (!shell.isSettingsOpen() && regionRoot)
                regionRoot->SetProperty("display", "none");
        });
        requestRender(200);
    }
}

void SettingsController::setSection(Section section)
{
    if (currentSection == section)
        return;
    stopRecordingKeybind();
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

    std::string rml;
    renderSidebar(rml);
    renderContent(rml);

    regionRoot->SetInnerRML(rml.c_str());
}

void SettingsController::renderSidebar(std::string &rml)
{
    rml += std::string("<div class=\"settings-sidebar\">");

    // USER SETTINGS
    rml += std::string("<div class=\"settings-group-header\">User Settings</div>");
    rml += formatRml("<div class=\"settings-nav-item%1\" data-section=\"my-account\">My Account</div>", currentSection == Section::MyAccount ? std::string(" active") : std::string());

    rml += std::string("<div class=\"settings-divider\"></div>");

    // APP SETTINGS
    rml += std::string("<div class=\"settings-group-header\">App Settings</div>");
    rml += formatRml("<div class=\"settings-nav-item%1\" data-section=\"appearance\">Appearance</div>", currentSection == Section::Appearance ? std::string(" active") : std::string());
    rml += formatRml("<div class=\"settings-nav-item%1\" data-section=\"chat\">Chat &amp; Accessibility</div>", currentSection == Section::Chat ? std::string(" active") : std::string());
    rml += formatRml("<div class=\"settings-nav-item%1\" data-section=\"notifications\">Notifications</div>", currentSection == Section::Notifications ? std::string(" active") : std::string());
    rml += formatRml("<div class=\"settings-nav-item%1\" data-section=\"voice-video\">Voice &amp; Video</div>", currentSection == Section::VoiceVideo ? std::string(" active") : std::string());
    rml += formatRml("<div class=\"settings-nav-item%1\" data-section=\"keybinds\">Keybinds</div>", currentSection == Section::Keybinds ? std::string(" active") : std::string());
    rml += formatRml("<div class=\"settings-nav-item%1\" data-section=\"go-live\">Go Live</div>", currentSection == Section::GoLive ? std::string(" active") : std::string());
    rml += formatRml("<div class=\"settings-nav-item%1\" data-section=\"advanced\">Advanced</div>", currentSection == Section::Advanced ? std::string(" active") : std::string());

    rml += std::string("<div class=\"settings-divider\"></div>");

    // Footer info
    rml += std::string("<div class=\"settings-sidebar-footer\">");
    rml += std::string("<div class=\"settings-app-version\">Styx v0.1.0</div>");
    rml += std::string("<a class=\"settings-credit-link\" data-action=\"open-acheron\">Based on Acheron by ouwou</a>");
    rml += std::string("</div>"); // .settings-sidebar-footer

    rml += std::string("</div>"); // .settings-sidebar
}

void SettingsController::renderContent(std::string &rml)
{
    rml += std::string("<div class=\"settings-content-pane\">");

    // Close button top right
    rml += std::string(
        "<div class=\"settings-close-container\">"
        "  <div class=\"settings-close-btn\" data-action=\"close\" title=\"Close Settings\">"
        "    <img src=\"icon:x\"/>"
        "  </div>"
        "  <div class=\"settings-esc-label\">ESC</div>"
        "</div>");

    rml += std::string("<div class=\"settings-content-wrapper\">");

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
    }

    rml += std::string("</div>"); // .settings-content-wrapper
    rml += std::string("</div>"); // .settings-content-pane
}

void SettingsController::renderMyAccount(std::string &rml)
{
    rml += std::string("<div class=\"settings-section-title\">My Account</div>");

    auto *inst = shell.currentInstance();
    std::string displayName = std::string("Not logged in");
    std::string username = std::string("none");
    std::string accountIdStr = std::string("-");
    std::string emailStr = std::string("Not provided");
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
    rml += std::string("<div class=\"my-account-field-label\">Email</div>");
    rml += formatRml("<div class=\"my-account-field-value\">%1</div>", Rml::escape(emailStr));
    rml += std::string("</div>");

    rml += std::string(
        "<div class=\"settings-button-group\">"
        "<button class=\"settings-btn settings-btn-primary\" data-action=\"edit-profile\" style=\"tab-index: auto;\">"
        "Edit Profile"
        "</button>"
        "<button class=\"settings-btn settings-btn-secondary\" data-action=\"manage-accounts\" style=\"tab-index: auto;\">"
        "Switch / Manage Accounts"
        "</button>"
        "</div>");
    rml += std::string("</div>"); // .my-account-card
}

void SettingsController::renderAppearance(std::string &rml)
{
    rml += std::string("<div class=\"settings-section-title\">Appearance</div>");

    // Theme Generator card
    rml += std::string("<div class=\"settings-card\">");
    rml += std::string("<div class=\"settings-label\">Theme Generator</div>");
    rml += std::string("<div class=\"settings-description\">Generate Discord-style themes from an accent seed color.</div>");

    rml += std::string("<div class=\"settings-color-row\">");
    rml += formatRml("<div class=\"settings-color-swatch\" style=\"background-color: %1;\"></div>", themeSeedColor.css());
    rml += formatRml("<input type=\"text\" class=\"settings-text-input\" id=\"seed-color-hex\" value=\"%1\" style=\"width: 120dp;\"/>", themeSeedColor.css());
    rml += std::string("</div>");

    rml += std::string("<div class=\"settings-row\">");
    rml += std::string("<div class=\"settings-label\">Palette Scheme</div>");
    rml += std::string("<select class=\"settings-select\" id=\"theme-scheme-select\">");
    for (int i = 0; i < Core::Theme::schemeCount; ++i) {
        auto s = static_cast<Core::Theme::Scheme>(i);
        std::string selected = (i == themeSchemeIndex) ? std::string(" selected") : std::string();
        rml += formatRml("<option value=\"%1\"%2>%3</option>", i, selected, Core::Theme::schemeName(s));
    }
    rml += std::string("</select>");
    rml += std::string("</div>");

    rml += std::string("<div class=\"settings-row\">");
    rml += std::string("<div class=\"settings-label\">Mode</div>");
    rml += std::string("<select class=\"settings-select\" id=\"theme-mode-select\">");
    rml += formatRml("<option value=\"dark\"%1>Dark</option>", themeDarkMode ? std::string(" selected") : std::string());
    rml += formatRml("<option value=\"light\"%1>Light</option>", !themeDarkMode ? std::string(" selected") : std::string());
    rml += std::string("</select>");
    rml += std::string("</div>");

    rml += std::string("<div class=\"settings-button-group\">");
    rml += std::string("<button class=\"settings-btn settings-btn-primary\" data-action=\"generate-theme\">Generate</button>");
    rml += std::string("<button class=\"settings-btn settings-btn-secondary\" data-action=\"random-theme\">Randomize</button>");
    rml += std::string("<button class=\"settings-btn settings-btn-danger\" data-action=\"reset-theme\">Reset Theme</button>");
    rml += std::string("</div>");
    rml += std::string("</div>"); // .settings-card

    // System theme switches
    rml += std::string("<div class=\"settings-card\">");
    const bool useSysColors = getBool(Keys::ThemeUseSystemColors, Defaults::ThemeUseSystemColors);
    rml += std::string("<div class=\"settings-switch-row\">");
    rml += std::string("<div class=\"settings-switch-text\">");
    rml += std::string("<div class=\"settings-switch-title\">Use system colors</div>");
    rml += std::string("<div class=\"settings-switch-desc\">Follow operating system color scheme overrides.</div>");
    rml += std::string("</div>");
    rml += formatRml("<div class=\"settings-switch%1\" data-setting=\"%2\" tabindex=\"0\"><div class=\"settings-switch-thumb\"></div></div>", useSysColors ? std::string(" checked") : std::string(), Keys::ThemeUseSystemColors);
    rml += std::string("</div>");

    rml += std::string("</div>"); // .settings-card

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
        rml += formatRml("<button class=\"settings-btn settings-btn-secondary\" data-action=\"font-size-dec\" data-role=\"%1\" style=\"padding: 4dp 10dp; margin-right: 6dp;\">-</button>", static_cast<int>(role));
        rml += formatRml("<button class=\"settings-btn settings-btn-secondary\" data-action=\"font-size-inc\" data-role=\"%1\" style=\"padding: 4dp 10dp; margin-right: 12dp;\">+</button>", static_cast<int>(role));
        rml += formatRml("<button class=\"settings-btn settings-btn-secondary\" data-action=\"reset-font\" data-role=\"%1\" style=\"padding: 4dp 12dp;\">Reset</button>", static_cast<int>(role));
        rml += std::string("</div>");
    }
    rml += std::string("</div>"); // .settings-card
}

void SettingsController::renderChat(std::string &rml)
{
    rml += std::string("<div class=\"settings-section-title\">Chat &amp; Accessibility</div>");

    // Animation toggles
    rml += std::string("<div class=\"settings-card\">");
    const bool animEmoji = getBool(Keys::AnimateEmoji, Defaults::AnimateEmoji);
    rml += formatRml("<div class=\"settings-switch-row\" data-setting=\"%1\" style=\"cursor: pointer;\">", Keys::AnimateEmoji);
    rml += std::string("<div class=\"settings-switch-text\">");
    rml += std::string("<div class=\"settings-switch-title\">Animate emoji</div>");
    rml += std::string("<div class=\"settings-switch-desc\">Play animated emoji reactions and messages continuously.</div>");
    rml += std::string("</div>");
    rml += formatRml("<div class=\"settings-switch%1\" data-setting=\"%2\" tabindex=\"0\"><div class=\"settings-switch-thumb\"></div></div>", animEmoji ? std::string(" checked") : std::string(), Keys::AnimateEmoji);
    rml += std::string("</div>");

    const bool animStickers = getBool(Keys::AnimateStickers, Defaults::AnimateStickers);
    rml += std::string("<div class=\"settings-switch-row\">");
    rml += std::string("<div class=\"settings-switch-text\">");
    rml += std::string("<div class=\"settings-switch-title\">Animate stickers</div>");
    rml += std::string("<div class=\"settings-switch-desc\">Play animated APNG and Lottie stickers in chat.</div>");
    rml += std::string("</div>");
    rml += formatRml("<div class=\"settings-switch%1\" data-setting=\"%2\" tabindex=\"0\"><div class=\"settings-switch-thumb\"></div></div>", animStickers ? std::string(" checked") : std::string(), Keys::AnimateStickers);
    rml += std::string("</div>");
    rml += std::string("</div>"); // .settings-card

    // GIF playback mode (Radio cards)
    rml += std::string("<div class=\"settings-card\">");
    rml += std::string("<div class=\"settings-label\">GIF Playback Mode</div>");
    const std::string gifMode = getString(Keys::GifPlayback, Defaults::GifPlayback);

    auto renderRadioCard = [&](const std::string &val, const std::string &title, const std::string &desc) {
        const bool active = (gifMode == val);
        rml += formatRml("<div class=\"settings-radio-card%1\" data-setting=\"%2\" data-value=\"%3\" tabindex=\"0\">", active ? std::string(" active") : std::string(), Keys::GifPlayback, val);
        rml += std::string("<div class=\"settings-radio-indicator\"></div>");
        rml += std::string("<div class=\"settings-radio-info\">");
        rml += formatRml("<div class=\"settings-radio-title\">%1</div>", title);
        rml += formatRml("<div class=\"settings-radio-desc\">%1</div>", desc);
        rml += std::string("</div></div>");
    };

    renderRadioCard(std::string("focused"), std::string("When Styx is focused"),
                    std::string("Play animated GIFs automatically when the window has focus."));
    renderRadioCard(std::string("hover"), std::string("On hover"),
                    std::string("Only play animated GIFs when hovering cursor over them."));
    renderRadioCard(std::string("never"), std::string("Never play"),
                    std::string("Keep animated GIFs frozen as still pictures."));

    // GIF max playing slider
    const int maxPlaying = getInt(Keys::GifMaxPlaying, Defaults::GifMaxPlaying);
    rml += std::string("<div class=\"settings-slider-wrap\" style=\"margin-top: 16dp;\">");
    rml += std::string("<div class=\"settings-slider-header\">");
    rml += std::string("<span class=\"settings-label\">Max simultaneous playing GIFs</span>");
    rml += formatRml("<span class=\"settings-slider-val\" id=\"val-gif-max-playing\">up to %1 at once</span>", maxPlaying);
    rml += std::string("</div>");
    rml += formatRml("<input type=\"range\" class=\"settings-slider\" min=\"1\" max=\"16\" step=\"1\" value=\"%1\" data-setting=\"%2\" data-display=\"val-gif-max-playing\" data-prefix=\"up to \" data-unit=\" at once\"/>", maxPlaying, Keys::GifMaxPlaying);
    rml += std::string("</div>");

    // GIF max memory slider
    const int maxGifMb = getInt(Keys::GifMaxMb, Defaults::GifMaxMb);
    rml += std::string("<div class=\"settings-slider-wrap\">");
    rml += std::string("<div class=\"settings-slider-header\">");
    rml += std::string("<span class=\"settings-label\">Largest GIF memory</span>");
    rml += formatRml("<span class=\"settings-slider-val\" id=\"val-gif-max-mb\">%1 MB</span>", maxGifMb);
    rml += std::string("</div>");
    rml += formatRml("<input type=\"range\" class=\"settings-slider\" min=\"8\" max=\"256\" step=\"8\" value=\"%1\" data-setting=\"%2\" data-display=\"val-gif-max-mb\" data-unit=\" MB\"/>", maxGifMb, Keys::GifMaxMb);
    rml += std::string("<div class=\"settings-description\">Oversized GIFs exceeding this memory threshold stay as still pictures.</div>");
    rml += std::string("</div>");

    rml += std::string("</div>"); // .settings-card

    // Animation Cache Limits
    rml += std::string("<div class=\"settings-card\">");
    rml += std::string("<div class=\"settings-label\">Animation Memory Limits</div>");

    const int cacheMb = getInt(Keys::AnimationCacheMb, Defaults::AnimationCacheMb);
    rml += std::string("<div class=\"settings-slider-wrap\">");
    rml += std::string("<div class=\"settings-slider-header\">");
    rml += std::string("<span class=\"settings-label\">Memory for animations</span>");
    rml += formatRml("<span class=\"settings-slider-val\" id=\"val-anim-cache-mb\">%1 MB</span>", cacheMb);
    rml += std::string("</div>");
    rml += formatRml("<input type=\"range\" class=\"settings-slider\" min=\"16\" max=\"1024\" step=\"16\" value=\"%1\" data-setting=\"%2\" data-display=\"val-anim-cache-mb\" data-unit=\" MB\"/>", cacheMb, Keys::AnimationCacheMb);
    rml += std::string("<div class=\"settings-description\">Total unpacked RAM buffer for animated stickers and emoji frames.</div>");
    rml += std::string("</div>");

    const int maxAnimMb = getInt(Keys::AnimationMaxMb, Defaults::AnimationMaxMb);
    rml += std::string("<div class=\"settings-slider-wrap\">");
    rml += std::string("<div class=\"settings-slider-header\">");
    rml += std::string("<span class=\"settings-label\">Largest single animation</span>");
    rml += formatRml("<span class=\"settings-slider-val\" id=\"val-anim-max-mb\">%1 MB</span>", maxAnimMb);
    rml += std::string("</div>");
    rml += formatRml("<input type=\"range\" class=\"settings-slider\" min=\"4\" max=\"128\" step=\"4\" value=\"%1\" data-setting=\"%2\" data-display=\"val-anim-max-mb\" data-unit=\" MB\"/>", maxAnimMb, Keys::AnimationMaxMb);
    rml += std::string("<div class=\"settings-description\">Animations that exceed this limit are downscaled or shown static.</div>");
    rml += std::string("</div>");

    rml += std::string("</div>"); // .settings-card
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
    auto renderSoundSwitch = [&renderSwitch](Sound sound, const std::string &title, const std::string &desc) {
        const std::string key(Core::Sounds::settingKey(sound));
        renderSwitch(key, getBool(key, true), title, desc);
    };

    rml += std::string("<div class=\"settings-section-title\">Notifications</div>");

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
                 "Show unread mentions on the taskbar",
                 "Put your mention count on Styx's taskbar or dock icon.");
    rml += std::string("<div class=\"settings-button-group\">");
    rml += std::string("<button class=\"settings-btn settings-btn-secondary\" data-action=\"test-notification\">Send a test notification</button>");
    rml += std::string("</div>");
    rml += std::string("</div>"); // .settings-card

    rml += std::string("<div class=\"settings-card\">");
    rml += std::string("<div class=\"settings-label\">Sounds</div>");
    renderSoundSwitch(Sound::Message, "Message", "A new message that notifies.");
    renderSoundSwitch(Sound::Mention, "Mention", "A message that mentions you, your role or @everyone.");
    renderSoundSwitch(Sound::Ring, "Incoming call", "Rings while someone is calling you in a DM.");
    renderSoundSwitch(Sound::Join, "Voice join", "You or someone else joins your voice channel.");
    renderSoundSwitch(Sound::Leave, "Voice leave", "You or someone else leaves your voice channel.");
    rml += std::string("</div>"); // .settings-card
}

void SettingsController::renderVoiceVideo(std::string &rml)
{
    rml += std::string("<div class=\"settings-section-title\">Voice &amp; Video</div>");

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

    rml += std::string("<div class=\"settings-grid-2col\" style=\"margin-bottom: 8dp;\">");

    // Input Volume Column
    rml += std::string("<div class=\"settings-grid-col settings-grid-col-left\">");
    rml += std::string("<div class=\"settings-slider-wrap\" style=\"margin-bottom: 0;\">");
    rml += std::string("<div class=\"settings-slider-header\">");
    rml += std::string("<span class=\"settings-label\" style=\"margin-bottom: 0;\">Input Volume</span>");
    rml += std::string("<span class=\"settings-slider-val\" id=\"val-voice-input-vol\">") + std::to_string(inputVolume) + std::string("%</span>");
    rml += std::string("</div>");
    rml += std::string("<input type=\"range\" class=\"settings-slider\" min=\"0\" max=\"100\" step=\"1\" value=\"")
           + std::to_string(inputVolume)
           + std::string("\" data-setting=\"") + std::string(Keys::VoiceInputVolume)
           + std::string("\" data-display=\"val-voice-input-vol\" data-unit=\"%\"/>");
    rml += std::string("</div>"); // .settings-slider-wrap
    rml += std::string("</div>"); // .settings-grid-col
    // Output Volume Column
    rml += std::string("<div class=\"settings-grid-col settings-grid-col-right\">");
    rml += std::string("<div class=\"settings-slider-wrap\" style=\"margin-bottom: 0;\">");
    rml += std::string("<div class=\"settings-slider-header\">");
    rml += std::string("<span class=\"settings-label\" style=\"margin-bottom: 0;\">Output Volume</span>");
    rml += std::string("<span class=\"settings-slider-val\" id=\"val-voice-output-vol\">") + std::to_string(outputVolume) + std::string("%</span>");
    rml += std::string("</div>");
    rml += std::string("<input type=\"range\" class=\"settings-slider\" min=\"0\" max=\"200\" step=\"1\" value=\"")
           + std::to_string(outputVolume)
           + std::string("\" data-setting=\"") + std::string(Keys::VoiceOutputVolume)
           + std::string("\" data-display=\"val-voice-output-vol\" data-unit=\"%\"/>");
    rml += std::string("</div>"); // .settings-slider-wrap
    rml += std::string("</div>"); // .settings-grid-col
    rml += std::string("</div>"); // .settings-grid-2col

    rml += std::string("<div class=\"settings-description\" style=\"margin-top: 8dp; margin-bottom: 0;\">Device and volume changes take effect immediately during a call, or on your next voice connection.</div>");
#endif
    rml += std::string("</div>"); // .settings-card (Voice Settings)

    // Input Mode card
    rml += std::string("<div class=\"settings-card\">");
    rml += std::string("<div class=\"settings-label\">Input Mode</div>");
    const std::string inputMode = getString(Keys::VoiceInputMode, Defaults::VoiceInputMode);

    auto renderInputModeRadio = [&](const std::string &val, const std::string &title) {
        const bool active = (inputMode == val);
        rml += formatRml("<div class=\"settings-radio-card%1\" data-setting=\"%2\" data-value=\"%3\" tabindex=\"0\">", active ? std::string(" active") : std::string(), Keys::VoiceInputMode, val);
        rml += std::string("<div class=\"settings-radio-indicator\"></div>");
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
        rml += std::string("<div class=\"settings-slider-wrap\" style=\"margin-bottom: 0;\">");
        rml += std::string("<div class=\"settings-slider-header\">");
        rml += std::string("<span class=\"settings-label\" style=\"margin-bottom: 0;\">Push to Talk Release Delay</span>");
        rml += std::string("<span class=\"settings-slider-val\" id=\"val-ptt-release-delay\">") + std::to_string(pttDelay) + std::string("ms</span>");
        rml += std::string("</div>");
        rml += std::string("<input type=\"range\" class=\"settings-slider\" min=\"0\" max=\"2000\" step=\"20\" value=\"")
               + std::to_string(pttDelay)
               + std::string("\" data-setting=\"") + std::string(Keys::VoicePushToTalkReleaseDelay)
               + std::string("\" data-display=\"val-ptt-release-delay\" data-unit=\"ms\"/>");
        rml += std::string("</div>"); // .settings-slider-wrap
    }

    rml += std::string("</div>"); // .settings-card

    // Advanced Subheading & Audio Backend card
    rml += std::string("<div class=\"settings-subheading\">Advanced</div>");
    rml += std::string("<div class=\"settings-card\">");

    const std::string currentBackend = getString(Keys::AudioBackend, Defaults::AudioBackend);
    rml += std::string("<div class=\"settings-row\" style=\"margin-bottom: 0;\">");
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
    rml += std::string("<div class=\"settings-section-title\">Keybinds</div>");
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
    rml += formatRml("<button class=\"%1\" id=\"record-keybind-%2\" data-action=\"record-keybind\" data-keybind=\"%2\">%3</button>",
                     btnClass, actionIdx, btnText);
    if (binding.valid()) {
        rml += formatRml("<button class=\"settings-btn settings-btn-danger\" data-action=\"remove-keybind\" data-keybind=\"%1\">Remove</button>",
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
    rml += std::string("<div class=\"settings-section-title\">Go Live</div>");

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
    rml += std::string("<div class=\"settings-section-title\">Advanced</div>");

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

    // Close to tray
    const bool closeToTray = getBool(Keys::CloseToTray, Defaults::CloseToTray);
    rml += std::string("<div class=\"settings-switch-row\">");
    rml += std::string("<div class=\"settings-switch-text\">");
    rml += std::string("<div class=\"settings-switch-title\">Keep running in system tray</div>");
    rml += std::string("<div class=\"settings-switch-desc\">Minimize Styx to the notification tray instead of exiting when the window is closed.</div>");
    rml += std::string("</div>");
    rml += formatRml("<div class=\"settings-switch%1\" data-setting=\"%2\" tabindex=\"0\"><div class=\"settings-switch-thumb\"></div></div>", closeToTray ? std::string(" checked") : std::string(), Keys::CloseToTray);
    rml += std::string("</div>");


    rml += std::string("</div>"); // .settings-card
}

void SettingsController::setupEventListeners()
{
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
            std::string sec = attribute(item, "data-section");
            if (sec == std::string("my-account"))
                setSection(Section::MyAccount);
            else if (sec == std::string("appearance"))
                setSection(Section::Appearance);
            else if (sec == std::string("chat"))
                setSection(Section::Chat);
            else if (sec == std::string("notifications"))
                setSection(Section::Notifications);
            else if (sec == std::string("voice-video"))
                setSection(Section::VoiceVideo);
            else if (sec == std::string("keybinds"))
                setSection(Section::Keybinds);
            else if (sec == std::string("go-live"))
                setSection(Section::GoLive);
            else if (sec == std::string("advanced"))
                setSection(Section::Advanced);
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
        auto *target = event.GetTargetElement();
        if (!target)
            return;

        if (target->GetTagName() == "input") {
            handleSliderChange(target);
        } else if (target->GetTagName() == "select") {
            handleSelectChange(target);
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
            shell.closeSettings();
            event.StopPropagation();
            return;
        }

        if (key == ::Rml::Input::KeyIdentifier::KI_SPACE || key == ::Rml::Input::KeyIdentifier::KI_RETURN) {
            auto *target = event.GetTargetElement();
            if (!target)
                return;
            if (target->IsClassSet("settings-switch")) {
                handleSwitchClick(target);
                event.StopPropagation();
            } else if (target->IsClassSet("settings-radio-card")) {
                handleRadioClick(target);
                event.StopPropagation();
            }
        }
    }, regionRoot);
}

void SettingsController::handleSwitchClick(::Rml::Element *switchEl)
{
    const std::string key = attribute(switchEl, "data-setting");
    if (key.empty())
        return;

    const bool wasChecked = switchEl->IsClassSet("checked");
    const bool nowChecked = !wasChecked;
    switchEl->SetClass("checked", nowChecked);

    setBool(key, nowChecked);

    // Immediate action application
    if (key == std::string(Keys::CloseToTray)) {
        shell.applyCloseToTray();
    } else if (key == std::string(Keys::TaskbarBadge)) {
        shell.refreshTaskbarBadge();
    } else if (key == std::string(Keys::ThemeUseSystemColors)) {
        Core::Theme::Manager::instance().setUseSystemColors(nowChecked);
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

void SettingsController::handleSliderChange(::Rml::Element *sliderEl)
{
    const std::string key = attribute(sliderEl, "data-setting");
    if (key.empty())
        return;

    auto *input = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(sliderEl);
    if (!input)
        return;

    int val = 0;
    const auto value = input->GetValue();
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), val);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) val = 0;
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

    // Apply immediate settings
    if (key == std::string(Keys::AnimationCacheMb)) {
        shell.session()->getAnimatedImageCache()->setCacheLimitMiB(val);
    } else if (key == std::string(Keys::AnimationMaxMb)) {
        shell.session()->getAnimatedImageCache()->setAnimationLimitMiB(val);
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

    // Local theme generator selects
    if (selectEl->GetId() == "theme-scheme-select") {
        themeSchemeIndex = std::stoi(val);
    } else if (selectEl->GetId() == "theme-mode-select") {
        themeDarkMode = (val == std::string("dark"));
    }

    requestRender(50);
}

void SettingsController::handleAction(const std::string &action, ::Rml::Element *target)
{
    if (action == std::string("close")) {
        shell.closeSettings();
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
        if (!profileEditor) {
            profileEditor = std::make_unique<ProfileEditorController>(shell);
            track(profileEditor->closed.connect([this] {
                if (this->shell.isSettingsOpen()) {
                    render();
                    requestRender();
                }
            }));
        }
        profileEditor->open();
    } else if (action == std::string("manage-accounts")) {
        shell.openAccounts();
    } else if (action == std::string("open-acheron")) {
        Platform::Desktop::openUrl("https://github.com/ouwou/acheron");
    } else if (action == std::string("test-notification")) {
        Platform::Notifications::Notification notification;
        notification.title = "Styx";
        notification.body = "This is how notifications will look.";
        Platform::Notifications::show(notification, {});
        Core::Sounds::play(Core::Sounds::Sound::Message);
    } else if (action == std::string("generate-theme")) {
        // Read seed color hex
        if (regionRoot) {
            if (auto *hexEl = regionRoot->GetElementById("seed-color-hex")) {
                if (auto *inp = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(hexEl)) {
                    std::string hex = inp->GetValue();
                    auto c = Platform::Color::parse(hex);
                    if (c.valid)
                        themeSeedColor = c;
                }
            }
        }
        const auto scheme = static_cast<Core::Theme::Scheme>(std::clamp(themeSchemeIndex, 0, Core::Theme::schemeCount - 1));
        Core::Theme::Manager::instance().setOverrides(Core::Theme::generate(themeSeedColor, scheme, themeDarkMode));
        Core::Theme::Manager::instance().apply();
        Core::Theme::Manager::instance().save();
        render();
        requestRender(100);
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
        themeSchemeIndex = std::uniform_int_distribution<int>(0, Core::Theme::schemeCount - 1)(rng);
        const auto scheme = static_cast<Core::Theme::Scheme>(std::clamp(themeSchemeIndex, 0, Core::Theme::schemeCount - 1));
        Core::Theme::Manager::instance().setOverrides(Core::Theme::generate(themeSeedColor, scheme, themeDarkMode));
        Core::Theme::Manager::instance().apply();
        Core::Theme::Manager::instance().save();
        render();
        requestRender(100);
    } else if (action == std::string("reset-theme")) {
        Core::Theme::Manager::instance().resetAll();
        Core::Theme::Manager::instance().apply();
        Core::Theme::Manager::instance().applyFonts();
        Core::Theme::Manager::instance().save();
        themeSeedColor = Core::Theme::Manager::instance().color(Core::Theme::Token::Highlight);
        render();
        requestRender(100);
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
