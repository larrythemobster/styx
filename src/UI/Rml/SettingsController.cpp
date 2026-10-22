#include "SettingsController.hpp"

#include "Shell.hpp"
#include "RmlRuntime.hpp"

#include "Core/AnimatedImageCache.hpp"
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

#include <QDesktopServices>
#include <QFontInfo>
#include <QRandomGenerator>
#include <QUrl>

#ifndef STYX_NO_VOICE
#include "Core/Audio/AudioBackends.hpp"
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

SettingsController::SettingsController(Shell &shell, ::Rml::Element *root, QObject *parent)
    : Controller(shell, root, parent)
{
    themeSeedColor = Core::Theme::Manager::instance().color(Core::Theme::Token::Highlight);

    rebuildTimer.setSingleShot(true);
    connect(&rebuildTimer, &QTimer::timeout, this, &SettingsController::rebuild);

    hideTransitionTimer.setSingleShot(true);
    connect(&hideTransitionTimer, &QTimer::timeout, this, [this]() {
        if (!this->shell.isSettingsOpen() && regionRoot)
            regionRoot->SetProperty("display", "none");
    });

    connect(&shell, &Shell::settingsOpenChanged, this, &SettingsController::onSettingsOpenChanged);
    connect(&shell, &Shell::instanceChanged, this, [this](Core::ClientInstance *) {
        if (this->shell.isSettingsOpen())
            rebuildTimer.start(0);
    });

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
        regionRoot->SetClass("open", false);
        hideTransitionTimer.start(160);
        requestRender(200);
    }
}

void SettingsController::setSection(Section section)
{
    if (currentSection == section)
        return;
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

    QString rml;
    renderSidebar(rml);
    renderContent(rml);

    regionRoot->SetInnerRML(rml.toUtf8().constData());
}

void SettingsController::renderSidebar(QString &rml)
{
    rml += QStringLiteral("<div class=\"settings-sidebar\">");

    // USER SETTINGS
    rml += QStringLiteral("<div class=\"settings-group-header\">User Settings</div>");
    rml += QStringLiteral("<div class=\"settings-nav-item%1\" data-section=\"my-account\">My Account</div>")
               .arg(currentSection == Section::MyAccount ? QStringLiteral(" active") : QString());

    rml += QStringLiteral("<div class=\"settings-divider\"></div>");

    // APP SETTINGS
    rml += QStringLiteral("<div class=\"settings-group-header\">App Settings</div>");
    rml += QStringLiteral("<div class=\"settings-nav-item%1\" data-section=\"appearance\">Appearance</div>")
               .arg(currentSection == Section::Appearance ? QStringLiteral(" active") : QString());
    rml += QStringLiteral("<div class=\"settings-nav-item%1\" data-section=\"chat\">Chat &amp; Accessibility</div>")
               .arg(currentSection == Section::Chat ? QStringLiteral(" active") : QString());
    rml += QStringLiteral("<div class=\"settings-nav-item%1\" data-section=\"voice-video\">Voice &amp; Video</div>")
               .arg(currentSection == Section::VoiceVideo ? QStringLiteral(" active") : QString());
    rml += QStringLiteral("<div class=\"settings-nav-item%1\" data-section=\"go-live\">Go Live</div>")
               .arg(currentSection == Section::GoLive ? QStringLiteral(" active") : QString());
    rml += QStringLiteral("<div class=\"settings-nav-item%1\" data-section=\"advanced\">Advanced</div>")
               .arg(currentSection == Section::Advanced ? QStringLiteral(" active") : QString());

    rml += QStringLiteral("<div class=\"settings-divider\"></div>");

    // Footer info
    rml += QStringLiteral("<div class=\"settings-sidebar-footer\">");
    rml += QStringLiteral("<div class=\"settings-app-version\">Styx v0.1.0</div>");
    rml += QStringLiteral("<a class=\"settings-credit-link\" data-action=\"open-upstream\">Based on Acheron by ouwou</a>");
    rml += QStringLiteral("</div>"); // .settings-sidebar-footer

    rml += QStringLiteral("</div>"); // .settings-sidebar
}

void SettingsController::renderContent(QString &rml)
{
    rml += QStringLiteral("<div class=\"settings-content-pane\">");

    // Close button top right
    rml += QStringLiteral(
        "<div class=\"settings-close-container\">"
        "  <div class=\"settings-close-btn\" data-action=\"close\" title=\"Close Settings\">"
        "    <img src=\"icon:x\"/>"
        "  </div>"
        "  <div class=\"settings-esc-label\">ESC</div>"
        "</div>");

    rml += QStringLiteral("<div class=\"settings-content-wrapper\">");

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
    case Section::VoiceVideo:
        renderVoiceVideo(rml);
        break;
    case Section::GoLive:
        renderGoLive(rml);
        break;
    case Section::Advanced:
        renderAdvanced(rml);
        break;
    }

    rml += QStringLiteral("</div>"); // .settings-content-wrapper
    rml += QStringLiteral("</div>"); // .settings-content-pane
}

void SettingsController::renderMyAccount(QString &rml)
{
    rml += QStringLiteral("<div class=\"settings-section-title\">My Account</div>");

    auto *inst = shell.currentInstance();
    QString displayName = QStringLiteral("Not logged in");
    QString username = QStringLiteral("none");
    QString accountIdStr = QStringLiteral("-");
    QString emailStr = QStringLiteral("Not provided");
    QString avatarSrc;

    if (inst) {
        const auto &info = inst->accountInfo();
        const auto accountId = inst->accountId();
        accountIdStr = accountId.toString();

        auto *userMgr = inst->users();
        std::optional<Discord::User> me;
        if (userMgr)
            me = userMgr->getUser(accountId);

        displayName = (me && me->globalName.hasValue() && !me->globalName->isEmpty())
                          ? *me->globalName
                          : (info.displayName.isEmpty() ? info.username : info.displayName);
        if (displayName.isEmpty())
            displayName = info.username;

        username = me ? me->username.get() : info.username;

        QString avatarHash = (me && me->avatar.hasValue()) ? *me->avatar : info.avatar;
        QUrl avatarUrl = !avatarHash.isEmpty()
                             ? Discord::Cdn::userAvatar(accountId, avatarHash, 80)
                             : Discord::Cdn::defaultUserAvatar(accountId);
        avatarSrc = shell.imageSource(avatarUrl, QSize(80, 80));
    }

    rml += QStringLiteral("<div class=\"my-account-card\">");
    rml += QStringLiteral("<div class=\"my-account-profile-header\">");
    if (!avatarSrc.isEmpty()) {
        rml += QStringLiteral("<img class=\"my-account-avatar\" src=\"%1\"/>").arg(Rml::escape(avatarSrc));
    }
    rml += QStringLiteral("<div class=\"my-account-names\">");
    rml += QStringLiteral("<div class=\"my-account-display-name\">%1</div>").arg(Rml::escape(displayName));
    rml += QStringLiteral("<div class=\"my-account-username\">@%1</div>").arg(Rml::escape(username));
    rml += QStringLiteral("</div>"); // .my-account-names
    rml += QStringLiteral("</div>"); // .my-account-profile-header

    rml += QStringLiteral("<div class=\"my-account-field-card\">");
    rml += QStringLiteral("<div class=\"my-account-field-label\">Display Name</div>");
    rml += QStringLiteral("<div class=\"my-account-field-value\">%1</div>").arg(Rml::escape(displayName));
    rml += QStringLiteral("</div>");

    rml += QStringLiteral("<div class=\"my-account-field-card\">");
    rml += QStringLiteral("<div class=\"my-account-field-label\">Username</div>");
    rml += QStringLiteral("<div class=\"my-account-field-value\">@%1</div>").arg(Rml::escape(username));
    rml += QStringLiteral("</div>");

    rml += QStringLiteral("<div class=\"my-account-field-card\">");
    rml += QStringLiteral("<div class=\"my-account-field-label\">User ID</div>");
    rml += QStringLiteral("<div class=\"my-account-field-value\">%1</div>").arg(Rml::escape(accountIdStr));
    rml += QStringLiteral("</div>");

    rml += QStringLiteral("<div class=\"my-account-field-card\">");
    rml += QStringLiteral("<div class=\"my-account-field-label\">Email</div>");
    rml += QStringLiteral("<div class=\"my-account-field-value\">%1</div>").arg(Rml::escape(emailStr));
    rml += QStringLiteral("</div>");

    rml += QStringLiteral(
        "<button class=\"settings-btn settings-btn-primary\" data-action=\"manage-accounts\">"
        "Switch / Manage Accounts"
        "</button>");
    rml += QStringLiteral("</div>"); // .my-account-card
}

void SettingsController::renderAppearance(QString &rml)
{
    rml += QStringLiteral("<div class=\"settings-section-title\">Appearance</div>");

    // Theme Generator card
    rml += QStringLiteral("<div class=\"settings-card\">");
    rml += QStringLiteral("<div class=\"settings-label\">Theme Generator</div>");
    rml += QStringLiteral("<div class=\"settings-description\">Generate Discord-style themes from an accent seed color.</div>");

    rml += QStringLiteral("<div class=\"settings-color-row\">");
    rml += QStringLiteral("<div class=\"settings-color-swatch\" style=\"background-color: %1;\"></div>")
               .arg(themeSeedColor.name(QColor::HexRgb));
    rml += QStringLiteral("<input type=\"text\" class=\"settings-text-input\" id=\"seed-color-hex\" value=\"%1\" style=\"width: 120dp;\"/>")
               .arg(themeSeedColor.name(QColor::HexRgb));
    rml += QStringLiteral("</div>");

    rml += QStringLiteral("<div class=\"settings-row\">");
    rml += QStringLiteral("<div class=\"settings-label\">Palette Scheme</div>");
    rml += QStringLiteral("<select class=\"settings-select\" id=\"theme-scheme-select\">");
    for (int i = 0; i < Core::Theme::schemeCount; ++i) {
        auto s = static_cast<Core::Theme::Scheme>(i);
        QString selected = (i == themeSchemeIndex) ? QStringLiteral(" selected") : QString();
        rml += QStringLiteral("<option value=\"%1\"%2>%3</option>").arg(i).arg(selected, Core::Theme::schemeName(s));
    }
    rml += QStringLiteral("</select>");
    rml += QStringLiteral("</div>");

    rml += QStringLiteral("<div class=\"settings-row\">");
    rml += QStringLiteral("<div class=\"settings-label\">Mode</div>");
    rml += QStringLiteral("<select class=\"settings-select\" id=\"theme-mode-select\">");
    rml += QStringLiteral("<option value=\"dark\"%1>Dark</option>").arg(themeDarkMode ? QStringLiteral(" selected") : QString());
    rml += QStringLiteral("<option value=\"light\"%1>Light</option>").arg(!themeDarkMode ? QStringLiteral(" selected") : QString());
    rml += QStringLiteral("</select>");
    rml += QStringLiteral("</div>");

    rml += QStringLiteral("<div class=\"settings-button-group\">");
    rml += QStringLiteral("<button class=\"settings-btn settings-btn-primary\" data-action=\"generate-theme\">Generate</button>");
    rml += QStringLiteral("<button class=\"settings-btn settings-btn-secondary\" data-action=\"random-theme\">Randomize</button>");
    rml += QStringLiteral("<button class=\"settings-btn settings-btn-danger\" data-action=\"reset-theme\">Reset Theme</button>");
    rml += QStringLiteral("</div>");
    rml += QStringLiteral("</div>"); // .settings-card

    // System theme switches
    rml += QStringLiteral("<div class=\"settings-card\">");
    const bool useSysColors = getBool(Keys::ThemeUseSystemColors, Defaults::ThemeUseSystemColors);
    rml += QStringLiteral("<div class=\"settings-switch-row\">");
    rml += QStringLiteral("<div class=\"settings-switch-text\">");
    rml += QStringLiteral("<div class=\"settings-switch-title\">Use system colors</div>");
    rml += QStringLiteral("<div class=\"settings-switch-desc\">Follow operating system color scheme overrides.</div>");
    rml += QStringLiteral("</div>");
    rml += QStringLiteral("<div class=\"settings-switch%1\" data-setting=\"%2\" tabindex=\"0\"><div class=\"settings-switch-thumb\"></div></div>")
               .arg(useSysColors ? QStringLiteral(" checked") : QString(), Keys::ThemeUseSystemColors);
    rml += QStringLiteral("</div>");

    const bool useSysStyle = getBool(Keys::ThemeUseSystemStyle, Defaults::ThemeUseSystemStyle);
    rml += QStringLiteral("<div class=\"settings-switch-row\">");
    rml += QStringLiteral("<div class=\"settings-switch-text\">");
    rml += QStringLiteral("<div class=\"settings-switch-title\">Use system widget style</div>");
    rml += QStringLiteral("<div class=\"settings-switch-desc\">Apply native OS widget style instead of Fusion.</div>");
    rml += QStringLiteral("<div class=\"settings-note\">* Applies after restart</div>");
    rml += QStringLiteral("</div>");
    rml += QStringLiteral("<div class=\"settings-switch%1\" data-setting=\"%2\" tabindex=\"0\"><div class=\"settings-switch-thumb\"></div></div>")
               .arg(useSysStyle ? QStringLiteral(" checked") : QString(), Keys::ThemeUseSystemStyle);
    rml += QStringLiteral("</div>");
    rml += QStringLiteral("</div>"); // .settings-card

    // Font overrides
    rml += QStringLiteral("<div class=\"settings-card\">");
    rml += QStringLiteral("<div class=\"settings-label\">Font Overrides</div>");
    rml += QStringLiteral("<div class=\"settings-description\">Tune typography sizes across Styx interface roles.</div>");

    for (const auto &fd : Core::Theme::fontRegistry()) {
        const auto role = fd.role;
        const QFont currentFont = Core::Theme::Manager::instance().font(role);
        const int ptSize = QFontInfo(currentFont).pointSize();

        rml += QStringLiteral("<div class=\"settings-font-row\">");
        rml += QStringLiteral("<span class=\"settings-font-name\">%1</span>").arg(QString::fromUtf8(fd.label));
        rml += QStringLiteral("<span class=\"settings-font-size\">%1 pt</span>").arg(ptSize);
        rml += QStringLiteral("<button class=\"settings-btn settings-btn-secondary\" data-action=\"font-size-dec\" data-role=\"%1\" style=\"padding: 4dp 10dp; margin-right: 6dp;\">-</button>").arg(static_cast<int>(role));
        rml += QStringLiteral("<button class=\"settings-btn settings-btn-secondary\" data-action=\"font-size-inc\" data-role=\"%1\" style=\"padding: 4dp 10dp; margin-right: 12dp;\">+</button>").arg(static_cast<int>(role));
        rml += QStringLiteral("<button class=\"settings-btn settings-btn-secondary\" data-action=\"reset-font\" data-role=\"%1\" style=\"padding: 4dp 12dp;\">Reset</button>").arg(static_cast<int>(role));
        rml += QStringLiteral("</div>");
    }
    rml += QStringLiteral("</div>"); // .settings-card
}

void SettingsController::renderChat(QString &rml)
{
    rml += QStringLiteral("<div class=\"settings-section-title\">Chat &amp; Accessibility</div>");

    // Animation toggles
    rml += QStringLiteral("<div class=\"settings-card\">");
    const bool animEmoji = getBool(Keys::AnimateEmoji, Defaults::AnimateEmoji);
    rml += QStringLiteral("<div class=\"settings-switch-row\" data-setting=\"%1\" style=\"cursor: pointer;\">").arg(Keys::AnimateEmoji);
    rml += QStringLiteral("<div class=\"settings-switch-text\">");
    rml += QStringLiteral("<div class=\"settings-switch-title\">Animate emoji</div>");
    rml += QStringLiteral("<div class=\"settings-switch-desc\">Play animated emoji reactions and messages continuously.</div>");
    rml += QStringLiteral("</div>");
    rml += QStringLiteral("<div class=\"settings-switch%1\" data-setting=\"%2\" tabindex=\"0\"><div class=\"settings-switch-thumb\"></div></div>")
               .arg(animEmoji ? QStringLiteral(" checked") : QString(), Keys::AnimateEmoji);
    rml += QStringLiteral("</div>");

    const bool animStickers = getBool(Keys::AnimateStickers, Defaults::AnimateStickers);
    rml += QStringLiteral("<div class=\"settings-switch-row\">");
    rml += QStringLiteral("<div class=\"settings-switch-text\">");
    rml += QStringLiteral("<div class=\"settings-switch-title\">Animate stickers</div>");
    rml += QStringLiteral("<div class=\"settings-switch-desc\">Play animated APNG and Lottie stickers in chat.</div>");
    rml += QStringLiteral("</div>");
    rml += QStringLiteral("<div class=\"settings-switch%1\" data-setting=\"%2\" tabindex=\"0\"><div class=\"settings-switch-thumb\"></div></div>")
               .arg(animStickers ? QStringLiteral(" checked") : QString(), Keys::AnimateStickers);
    rml += QStringLiteral("</div>");
    rml += QStringLiteral("</div>"); // .settings-card

    // GIF playback mode (Radio cards)
    rml += QStringLiteral("<div class=\"settings-card\">");
    rml += QStringLiteral("<div class=\"settings-label\">GIF Playback Mode</div>");
    const QString gifMode = getString(Keys::GifPlayback, Defaults::GifPlayback);

    auto renderRadioCard = [&](const QString &val, const QString &title, const QString &desc) {
        const bool active = (gifMode == val);
        rml += QStringLiteral("<div class=\"settings-radio-card%1\" data-setting=\"%2\" data-value=\"%3\" tabindex=\"0\">")
                   .arg(active ? QStringLiteral(" active") : QString(), Keys::GifPlayback, val);
        rml += QStringLiteral("<div class=\"settings-radio-indicator\"></div>");
        rml += QStringLiteral("<div class=\"settings-radio-info\">");
        rml += QStringLiteral("<div class=\"settings-radio-title\">%1</div>").arg(title);
        rml += QStringLiteral("<div class=\"settings-radio-desc\">%1</div>").arg(desc);
        rml += QStringLiteral("</div></div>");
    };

    renderRadioCard(QStringLiteral("focused"), QStringLiteral("When Styx is focused"),
                    QStringLiteral("Play animated GIFs automatically when the window has focus."));
    renderRadioCard(QStringLiteral("hover"), QStringLiteral("On hover"),
                    QStringLiteral("Only play animated GIFs when hovering cursor over them."));
    renderRadioCard(QStringLiteral("never"), QStringLiteral("Never play"),
                    QStringLiteral("Keep animated GIFs frozen as still pictures."));

    // GIF max playing slider
    const int maxPlaying = getInt(Keys::GifMaxPlaying, Defaults::GifMaxPlaying);
    rml += QStringLiteral("<div class=\"settings-slider-wrap\" style=\"margin-top: 16dp;\">");
    rml += QStringLiteral("<div class=\"settings-slider-header\">");
    rml += QStringLiteral("<span class=\"settings-label\">Max simultaneous playing GIFs</span>");
    rml += QStringLiteral("<span class=\"settings-slider-val\" id=\"val-gif-max-playing\">up to %1 at once</span>").arg(maxPlaying);
    rml += QStringLiteral("</div>");
    rml += QStringLiteral("<input type=\"range\" class=\"settings-slider\" min=\"1\" max=\"16\" step=\"1\" value=\"%1\" data-setting=\"%2\" data-display=\"val-gif-max-playing\" data-prefix=\"up to \" data-unit=\" at once\"/>")
               .arg(maxPlaying).arg(Keys::GifMaxPlaying);
    rml += QStringLiteral("</div>");

    // GIF max memory slider
    const int maxGifMb = getInt(Keys::GifMaxMb, Defaults::GifMaxMb);
    rml += QStringLiteral("<div class=\"settings-slider-wrap\">");
    rml += QStringLiteral("<div class=\"settings-slider-header\">");
    rml += QStringLiteral("<span class=\"settings-label\">Largest GIF memory</span>");
    rml += QStringLiteral("<span class=\"settings-slider-val\" id=\"val-gif-max-mb\">%1 MB</span>").arg(maxGifMb);
    rml += QStringLiteral("</div>");
    rml += QStringLiteral("<input type=\"range\" class=\"settings-slider\" min=\"8\" max=\"256\" step=\"8\" value=\"%1\" data-setting=\"%2\" data-display=\"val-gif-max-mb\" data-unit=\" MB\"/>")
               .arg(maxGifMb).arg(Keys::GifMaxMb);
    rml += QStringLiteral("<div class=\"settings-description\">Oversized GIFs exceeding this memory threshold stay as still pictures.</div>");
    rml += QStringLiteral("</div>");

    rml += QStringLiteral("</div>"); // .settings-card

    // Animation Cache Limits
    rml += QStringLiteral("<div class=\"settings-card\">");
    rml += QStringLiteral("<div class=\"settings-label\">Animation Memory Limits</div>");

    const int cacheMb = getInt(Keys::AnimationCacheMb, Defaults::AnimationCacheMb);
    rml += QStringLiteral("<div class=\"settings-slider-wrap\">");
    rml += QStringLiteral("<div class=\"settings-slider-header\">");
    rml += QStringLiteral("<span class=\"settings-label\">Memory for animations</span>");
    rml += QStringLiteral("<span class=\"settings-slider-val\" id=\"val-anim-cache-mb\">%1 MB</span>").arg(cacheMb);
    rml += QStringLiteral("</div>");
    rml += QStringLiteral("<input type=\"range\" class=\"settings-slider\" min=\"16\" max=\"1024\" step=\"16\" value=\"%1\" data-setting=\"%2\" data-display=\"val-anim-cache-mb\" data-unit=\" MB\"/>")
               .arg(cacheMb).arg(Keys::AnimationCacheMb);
    rml += QStringLiteral("<div class=\"settings-description\">Total unpacked RAM buffer for animated stickers and emoji frames.</div>");
    rml += QStringLiteral("</div>");

    const int maxAnimMb = getInt(Keys::AnimationMaxMb, Defaults::AnimationMaxMb);
    rml += QStringLiteral("<div class=\"settings-slider-wrap\">");
    rml += QStringLiteral("<div class=\"settings-slider-header\">");
    rml += QStringLiteral("<span class=\"settings-label\">Largest single animation</span>");
    rml += QStringLiteral("<span class=\"settings-slider-val\" id=\"val-anim-max-mb\">%1 MB</span>").arg(maxAnimMb);
    rml += QStringLiteral("</div>");
    rml += QStringLiteral("<input type=\"range\" class=\"settings-slider\" min=\"4\" max=\"128\" step=\"4\" value=\"%1\" data-setting=\"%2\" data-display=\"val-anim-max-mb\" data-unit=\" MB\"/>")
               .arg(maxAnimMb).arg(Keys::AnimationMaxMb);
    rml += QStringLiteral("<div class=\"settings-description\">Animations that exceed this limit are downscaled or shown static.</div>");
    rml += QStringLiteral("</div>");

    rml += QStringLiteral("</div>"); // .settings-card
}

void SettingsController::renderVoiceVideo(QString &rml)
{
    rml += QStringLiteral("<div class=\"settings-section-title\">Voice &amp; Video</div>");

    // Audio Devices & Backend card
    rml += QStringLiteral("<div class=\"settings-card\">");
    rml += QStringLiteral("<div class=\"settings-label\">Audio Devices</div>");

    // Backend
    const QString currentBackend = getString(Keys::AudioBackend, Defaults::AudioBackend);
    rml += QStringLiteral("<div class=\"settings-row\">");
    rml += QStringLiteral("<div class=\"settings-label\">Audio Backend</div>");
    rml += QStringLiteral("<select class=\"settings-select\" data-setting=\"%1\">").arg(Keys::AudioBackend);
    rml += QStringLiteral("<option value=\"\"%1>Automatic</option>").arg(currentBackend.isEmpty() ? QStringLiteral(" selected") : QString());
#ifndef STYX_NO_VOICE
    for (const QString &name : Core::Audio::supportedAudioBackends()) {
        QString sel = (currentBackend == name) ? QStringLiteral(" selected") : QString();
        rml += QStringLiteral("<option value=\"%1\"%2>%3</option>").arg(name, sel, name);
    }
#endif
    rml += QStringLiteral("</select>");
    rml += QStringLiteral("<div class=\"settings-note\">* Takes effect after restart</div>");
    rml += QStringLiteral("</div>");

#ifndef STYX_NO_VOICE
    auto *inst = shell.currentInstance();
    auto *vm = inst ? inst->voice() : nullptr;

    // Input Device
    const QString currentInput = getString(Keys::VoiceInputDevice, Defaults::VoiceInputDevice);
    rml += QStringLiteral("<div class=\"settings-row\">");
    rml += QStringLiteral("<div class=\"settings-label\">Input Device</div>");
    rml += QStringLiteral("<select class=\"settings-select\" data-setting=\"%1\">").arg(Keys::VoiceInputDevice);
    rml += QStringLiteral("<option value=\"\"%1>Default Communication Device</option>").arg(currentInput.isEmpty() ? QStringLiteral(" selected") : QString());
    if (vm) {
        for (const auto &dev : vm->availableInputDevices()) {
            QString devId = QString::fromUtf8(dev.id);
            QString sel = (currentInput == devId || (currentInput.isEmpty() && dev.isDefault)) ? QStringLiteral(" selected") : QString();
            rml += QStringLiteral("<option value=\"%1\"%2>%3</option>").arg(Rml::escape(devId), sel, Rml::escape(dev.description));
        }
    }
    rml += QStringLiteral("</select>");
    rml += QStringLiteral("</div>");

    // Output Device
    const QString currentOutput = getString(Keys::VoiceOutputDevice, Defaults::VoiceOutputDevice);
    rml += QStringLiteral("<div class=\"settings-row\">");
    rml += QStringLiteral("<div class=\"settings-label\">Output Device</div>");
    rml += QStringLiteral("<select class=\"settings-select\" data-setting=\"%1\">").arg(Keys::VoiceOutputDevice);
    rml += QStringLiteral("<option value=\"\"%1>Default Communication Device</option>").arg(currentOutput.isEmpty() ? QStringLiteral(" selected") : QString());
    if (vm) {
        for (const auto &dev : vm->availableOutputDevices()) {
            QString devId = QString::fromUtf8(dev.id);
            QString sel = (currentOutput == devId || (currentOutput.isEmpty() && dev.isDefault)) ? QStringLiteral(" selected") : QString();
            rml += QStringLiteral("<option value=\"%1\"%2>%3</option>").arg(Rml::escape(devId), sel, Rml::escape(dev.description));
        }
    }
    rml += QStringLiteral("</select>");
    rml += QStringLiteral("</div>");
#endif
    rml += QStringLiteral("</div>"); // .settings-card

}

void SettingsController::renderGoLive(QString &rml)
{
    rml += QStringLiteral("<div class=\"settings-section-title\">Go Live</div>");

    rml += QStringLiteral("<div class=\"settings-card\">");
    rml += QStringLiteral("<div class=\"settings-label\">Screenshare Quality</div>");
    rml += QStringLiteral("<div class=\"settings-description\">Default resolution and frame rate when sharing a screen or application window.</div>");

    const QString currentPreset = getString(Keys::StreamQuality, Defaults::StreamQuality);
    rml += QStringLiteral("<div class=\"settings-row\">");
    rml += QStringLiteral("<select class=\"settings-select\" data-setting=\"%1\">").arg(Keys::StreamQuality);
    for (const auto &preset : Core::Stream::StreamQuality::presets()) {
        QString sel = (preset.id == currentPreset) ? QStringLiteral(" selected") : QString();
        rml += QStringLiteral("<option value=\"%1\"%2>%3</option>").arg(preset.id, sel, preset.label());
    }
    rml += QStringLiteral("</select>");
    rml += QStringLiteral("</div>");

    const bool shareAudio = getBool(Keys::StreamAudio, Defaults::StreamAudio);
    rml += QStringLiteral("<div class=\"settings-switch-row\">");
    rml += QStringLiteral("<div class=\"settings-switch-text\">");
    rml += QStringLiteral("<div class=\"settings-switch-title\">Share system audio</div>");
    rml += QStringLiteral("<div class=\"settings-switch-desc\">Capture application sound when sharing a window, or system audio when sharing screen.</div>");
    rml += QStringLiteral("</div>");
    rml += QStringLiteral("<div class=\"settings-switch%1\" data-setting=\"%2\" tabindex=\"0\"><div class=\"settings-switch-thumb\"></div></div>")
               .arg(shareAudio ? QStringLiteral(" checked") : QString(), Keys::StreamAudio);
    rml += QStringLiteral("</div>");

    rml += QStringLiteral("</div>"); // .settings-card
}

void SettingsController::renderAdvanced(QString &rml)
{
    rml += QStringLiteral("<div class=\"settings-section-title\">Advanced</div>");

    rml += QStringLiteral("<div class=\"settings-card\">");

    // In-memory cache
    const bool inMemory = getBool(Keys::InMemoryCache, Defaults::InMemoryCache);
    rml += QStringLiteral("<div class=\"settings-switch-row\">");
    rml += QStringLiteral("<div class=\"settings-switch-text\">");
    rml += QStringLiteral("<div class=\"settings-switch-title\">In-memory cache database</div>");
    rml += QStringLiteral("<div class=\"settings-switch-desc\">Store temporary cache in RAM (:memory:) instead of sqlite file on disk.</div>");
    rml += QStringLiteral("<div class=\"settings-note\">* Requires restart</div>");
    rml += QStringLiteral("</div>");
    rml += QStringLiteral("<div class=\"settings-switch%1\" data-setting=\"%2\" tabindex=\"0\"><div class=\"settings-switch-thumb\"></div></div>")
               .arg(inMemory ? QStringLiteral(" checked") : QString(), Keys::InMemoryCache);
    rml += QStringLiteral("</div>");

    // Close to tray
    const bool closeToTray = getBool(Keys::CloseToTray, Defaults::CloseToTray);
    rml += QStringLiteral("<div class=\"settings-switch-row\">");
    rml += QStringLiteral("<div class=\"settings-switch-text\">");
    rml += QStringLiteral("<div class=\"settings-switch-title\">Keep running in system tray</div>");
    rml += QStringLiteral("<div class=\"settings-switch-desc\">Minimize Styx to the notification tray instead of exiting when the window is closed.</div>");
    rml += QStringLiteral("</div>");
    rml += QStringLiteral("<div class=\"settings-switch%1\" data-setting=\"%2\" tabindex=\"0\"><div class=\"settings-switch-thumb\"></div></div>")
               .arg(closeToTray ? QStringLiteral(" checked") : QString(), Keys::CloseToTray);
    rml += QStringLiteral("</div>");

#ifdef _WIN32
    // Font Engine
    const QString fontEngine = getString(Keys::FontEngine, Defaults::FontEngine);
    rml += QStringLiteral("<div class=\"settings-row\" style=\"margin-top: 16dp;\">");
    rml += QStringLiteral("<div class=\"settings-label\">Font Engine</div>");
    rml += QStringLiteral("<select class=\"settings-select\" data-setting=\"%1\">").arg(Keys::FontEngine);
    rml += QStringLiteral("<option value=\"directwrite\"%1>DirectWrite (Default)</option>").arg(fontEngine == QLatin1String("directwrite") ? QStringLiteral(" selected") : QString());
    rml += QStringLiteral("<option value=\"freetype\"%1>FreeType</option>").arg(fontEngine == QLatin1String("freetype") ? QStringLiteral(" selected") : QString());
    rml += QStringLiteral("</select>");
    rml += QStringLiteral("<div class=\"settings-note\">* Windows font rasterization engine. Requires restart.</div>");
    rml += QStringLiteral("</div>");
#endif

    rml += QStringLiteral("</div>"); // .settings-card
}

void SettingsController::setupEventListeners()
{
    listen("click", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        if (!target)
            return;

        // Close button
        if (auto *btn = closest(target, "data-action")) {
            QString action = attribute(btn, "data-action");
            handleAction(action, btn);
            return;
        }

        // Section tab item
        if (auto *item = closest(target, "data-section")) {
            QString sec = attribute(item, "data-section");
            if (sec == QLatin1String("my-account"))
                setSection(Section::MyAccount);
            else if (sec == QLatin1String("appearance"))
                setSection(Section::Appearance);
            else if (sec == QLatin1String("chat"))
                setSection(Section::Chat);
            else if (sec == QLatin1String("voice-video"))
                setSection(Section::VoiceVideo);
            else if (sec == QLatin1String("go-live"))
                setSection(Section::GoLive);
            else if (sec == QLatin1String("advanced"))
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

    listen("keydown", [this](::Rml::Event &event) {
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
    const QString key = attribute(switchEl, "data-setting");
    if (key.isEmpty())
        return;

    const bool wasChecked = switchEl->IsClassSet("checked");
    const bool nowChecked = !wasChecked;
    switchEl->SetClass("checked", nowChecked);

    setBool(key, nowChecked);

    // Immediate action application
    if (key == QLatin1String(Keys::CloseToTray)) {
        shell.applyCloseToTray();
    } else if (key == QLatin1String(Keys::ThemeUseSystemColors)) {
        Core::Theme::Manager::instance().setUseSystemColors(nowChecked);
    } else if (key == QLatin1String(Keys::ThemeUseSystemStyle)) {
        Core::Theme::Manager::setSystemStyleEnabled(nowChecked);
    }

    requestRender(100);
}

void SettingsController::handleRadioClick(::Rml::Element *radioEl)
{
    const QString key = attribute(radioEl, "data-setting");
    const QString val = attribute(radioEl, "data-value");
    if (key.isEmpty() || val.isEmpty())
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

    requestRender(100);
}

void SettingsController::handleSliderChange(::Rml::Element *sliderEl)
{
    const QString key = attribute(sliderEl, "data-setting");
    if (key.isEmpty())
        return;

    auto *input = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(sliderEl);
    if (!input)
        return;

    const int val = QString::fromStdString(input->GetValue()).toInt();
    setInt(key, val);

    // Update display label
    const QString dispId = attribute(sliderEl, "data-display");
    if (!dispId.isEmpty() && regionRoot) {
        if (auto *labelEl = regionRoot->GetElementById(dispId.toStdString())) {
            const QString prefix = attribute(sliderEl, "data-prefix");
            const QString unit = attribute(sliderEl, "data-unit");
            labelEl->SetInnerRML((prefix + QString::number(val) + unit).toUtf8().constData());
        }
    }

    // Apply immediate settings
    if (key == QLatin1String(Keys::AnimationCacheMb)) {
        shell.session()->getAnimatedImageCache()->setCacheLimitMiB(val);
    } else if (key == QLatin1String(Keys::AnimationMaxMb)) {
        shell.session()->getAnimatedImageCache()->setAnimationLimitMiB(val);
    }

    requestRender(50);
}

void SettingsController::handleSelectChange(::Rml::Element *selectEl)
{
    const QString key = attribute(selectEl, "data-setting");
    auto *sel = rmlui_dynamic_cast<::Rml::ElementFormControlSelect *>(selectEl);
    if (!sel)
        return;

    const QString val = QString::fromStdString(sel->GetValue());

    if (!key.isEmpty()) {
            setString(key, val);
#ifndef STYX_NO_VOICE
            if (key == QLatin1String(Keys::VoiceInputDevice)) {
                if (auto *inst = shell.currentInstance())
                    if (auto *vm = inst->voice())
                        vm->setInputDevice(val.toUtf8());
            } else if (key == QLatin1String(Keys::VoiceOutputDevice)) {
                if (auto *inst = shell.currentInstance())
                    if (auto *vm = inst->voice())
                        vm->setOutputDevice(val.toUtf8());
            }
#endif
        }

    // Local theme generator selects
    if (selectEl->GetId() == "theme-scheme-select") {
        themeSchemeIndex = val.toInt();
    } else if (selectEl->GetId() == "theme-mode-select") {
        themeDarkMode = (val == QLatin1String("dark"));
    }

    requestRender(50);
}

void SettingsController::handleAction(const QString &action, ::Rml::Element *target)
{
    if (action == QLatin1String("close")) {
        shell.closeSettings();
    } else if (action == QLatin1String("manage-accounts")) {
        shell.openAccounts();
    } else if (action == QLatin1String("open-upstream")) {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/ouwou/acheron")));
    } else if (action == QLatin1String("generate-theme")) {
        // Read seed color hex
        if (regionRoot) {
            if (auto *hexEl = regionRoot->GetElementById("seed-color-hex")) {
                if (auto *inp = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(hexEl)) {
                    QString hex = QString::fromStdString(inp->GetValue());
                    QColor c(hex);
                    if (c.isValid())
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
    } else if (action == QLatin1String("random-theme")) {
        auto *rng = QRandomGenerator::global();
        const qreal h = rng->generateDouble() * 360.0;
        const qreal s = 0.55 + rng->generateDouble() * 0.40;
        const qreal l = 0.45 + rng->generateDouble() * 0.20;
        themeSeedColor = QColor::fromHslF(static_cast<float>(h / 360.0), static_cast<float>(s), static_cast<float>(l));
        themeSchemeIndex = static_cast<int>(rng->generateDouble() * Core::Theme::schemeCount);
        const auto scheme = static_cast<Core::Theme::Scheme>(std::clamp(themeSchemeIndex, 0, Core::Theme::schemeCount - 1));
        Core::Theme::Manager::instance().setOverrides(Core::Theme::generate(themeSeedColor, scheme, themeDarkMode));
        Core::Theme::Manager::instance().apply();
        Core::Theme::Manager::instance().save();
        render();
        requestRender(100);
    } else if (action == QLatin1String("reset-theme")) {
        Core::Theme::Manager::instance().resetAll();
        Core::Theme::Manager::instance().apply();
        Core::Theme::Manager::instance().applyFonts();
        Core::Theme::Manager::instance().save();
        themeSeedColor = Core::Theme::Manager::instance().color(Core::Theme::Token::Highlight);
        render();
        requestRender(100);
    } else if (action == QLatin1String("font-size-inc") || action == QLatin1String("font-size-dec")) {
        int roleInt = attribute(target, "data-role").toInt();
        auto role = static_cast<Core::Theme::FontRole>(roleInt);
        QFont f = Core::Theme::Manager::instance().font(role);
        int curPt = QFontInfo(f).pointSize();
        int newPt = (action == QLatin1String("font-size-inc")) ? curPt + 1 : std::max(6, curPt - 1);
        f.setPointSize(newPt);
        Core::Theme::Manager::instance().setFontOverride(role, f);
        Core::Theme::Manager::instance().applyFonts();
        Core::Theme::Manager::instance().save();
        render();
        requestRender(100);
    } else if (action == QLatin1String("reset-font")) {
        int roleInt = attribute(target, "data-role").toInt();
        auto role = static_cast<Core::Theme::FontRole>(roleInt);
        Core::Theme::Manager::instance().clearFontOverride(role);
        Core::Theme::Manager::instance().applyFonts();
        Core::Theme::Manager::instance().save();
        render();
        requestRender(100);
    }
}

} // namespace Rml
} // namespace UI
} // namespace Styx
