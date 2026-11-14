#include "SettingsController.hpp"

#include "ConfirmModal.hpp"
#include "ProfileEditorController.hpp"
#include "Shell.hpp"
#include "ShortcutController.hpp"
#include "ReleaseVersion.hpp"
#include "Shortcuts.hpp"
#include "QrImage.hpp"
#include "RmlRuntime.hpp"
#include "UI/PushToTalk.hpp"

#include "Core/AccountDetails.hpp"
#include "Core/AnimatedImageCache.hpp"
#include "Core/Audio/Sounds.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/Session.hpp"
#include "Core/SettingsKeys.hpp"
#include "Core/Stream/StreamQuality.hpp"
#ifdef STYX_HAVE_SCREENSHARE
#include "Core/Stream/LoopbackCapture.hpp"
#endif
#include "Core/Theme/Fonts.hpp"
#include "Core/Theme/Generator.hpp"
#include "Core/Logging.hpp"
#include "Core/Theme/Manager.hpp"
#include "Core/Theme/Scale.hpp"
#include "Core/Theme/Tokens.hpp"
#include "Core/UserManager.hpp"
#include "Core/TimeUtils.hpp"
#include "Core/TokenStore.hpp"
#include "Discord/CdnUrls.hpp"
#include "Discord/Entities.hpp"
#include "Proto/PreloadedSettings.hpp"
#include "Storage/AccountRepository.hpp"

#include "Platform/Badge.hpp"
#include "Platform/Notifications.hpp"
#include "Platform/Desktop.hpp"
#include "Platform/Dispatcher.hpp"
#include <algorithm>
#include <cctype>
#include <functional>
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
        emailRevealed = phoneRevealed = false;
        if (!passwordSaving) passwordFormOpen = false;
        // Backup codes stay up across the reconnect that follows enabling the authenticator.
        if (!mfaBusy && mfaStep != MfaStep::BackupCodes) clearMfaFlow();
        hideDevices();
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
    track(shell.interfaceZoomChanged.connect([this](int zoom) {
        // Ctrl +/- while the slider is visible keeps it in step with the applied zoom.
        if (regionRoot && this->shell.isSettingsOpen())
            if (auto *slider = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(regionRoot->GetElementById("interface-zoom")))
                showSteppedValue(slider, zoom);
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
        emailRevealed = phoneRevealed = false;
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
        if (!mfaBusy) clearMfaFlow();
        hideDevices();
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
    emailRevealed = phoneRevealed = false;
    if (!passwordSaving) passwordFormOpen = false;
    if (!mfaBusy) clearMfaFlow();
    hideDevices();
    securityNotice.clear();
    currentSubsection.clear();
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
        rml += formatRml("<button class=\"settings-btn settings-account\" data-section=\"profiles\" aria-label=\"Edit profiles\">"
            "<span class=\"settings-account-avatar\"><img src=\"%1\"/></span><span class=\"settings-account-text\">"
            "<span class=\"settings-account-name\">%2</span><span class=\"settings-account-edit\"><span>Edit Profiles</span>"
            "<img class=\"settings-account-pencil\" src=\"icon:pencil\"/></span></span></button>",
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
    case Section::MyAccount: renderMyAccount(rml); break;
    case Section::Profiles: renderProfiles(rml); break;
    case Section::MessagingPermissions:
        appendSubsectionHeading(rml, "spam-filters");
        renderSpamFilters(rml);
        appendSubsectionHeading(rml, "direct-messages");
        renderPrivacy(rml);
        appendSubsectionHeading(rml, "friend-requests");
        renderFriendRequests(rml);
        break;
    case Section::Notifications: renderNotifications(rml); break;
    case Section::VoiceVideo:
        appendSubsectionHeading(rml, "voice");
        renderVoiceVideo(rml);
        appendSubsectionHeading(rml, "streaming");
        renderGoLive(rml);
        break;
    case Section::Appearance:
        renderAppearance(rml);
        appendSubsectionHeading(rml, "messages");
        renderChat(rml);
        break;
    case Section::Accessibility: renderAccessibility(rml); break;
    case Section::System:
        appendSubsectionHeading(rml, "general");
        renderSystem(rml);
        appendSubsectionHeading(rml, "custom-keybinds");
        renderKeybinds(rml);
        appendSubsectionHeading(rml, "advanced");
        renderAdvanced(rml);
        break;
    case Section::Language: renderLanguage(rml); break;
    case Section::ActivityPrivacy: renderActivityPrivacy(rml); break;
    }

    rml += std::string("</div>"); // .settings-content-wrapper
    rml += "</div>"; // .settings-content-scroll
    rml += std::string("</div>"); // .settings-content-pane
}

void SettingsController::renderMyAccount(std::string &rml)
{
    auto *inst = shell.currentInstance();
    auto row = [&rml](std::string_view label, const std::string &valueRml, const std::string &actionRml = {}) {
        rml += formatRml("<div class=\"account-info-row\"><div class=\"account-info-label\">%1</div>"
            "<div class=\"account-info-value\">%2</div>%3</div>",
            Rml::escape(std::string(label)), valueRml, actionRml);
    };
    // Reveal and Hide only change what this page shows; the values come from the signed-in account.
    auto revealButton = [](const char *action, bool revealed, const char *what) {
        return formatRml("<button class=\"settings-btn account-info-reveal\" data-action=\"%1\" aria-label=\"%2 %3\">"
            "<span class=\"settings-btn-label\">%2</span></button>",
            std::string(action), std::string(revealed ? "Hide" : "Reveal"), std::string(what));
    };
    auto muted = [](std::string_view text) {
        return "<span class=\"account-info-muted\">" + Rml::escape(std::string(text)) + "</span>";
    };

    appendSubsectionHeading(rml, "account-info");
    if (!inst) {
        rml += "<div class=\"settings-description\">Sign in to an account to see its details.</div>";
    } else {
        const auto &self = inst->discord()->getMe();
        const bool ready = self.id.get() == inst->accountId();
        const auto username = ready ? self.username.get() : inst->accountInfo().username;
        row("Username", Rml::escape(username));
        if (!ready) {
            row("Email", muted("Loading..."));
            row("Phone Number", muted("Loading..."));
        } else {
            if (self.email.hasValue() && !self.email->empty()) {
                std::string value = Rml::escape(emailRevealed ? self.email.get() : Core::maskEmail(self.email.get()));
                if (self.verified.hasValue() && !self.verified.get())
                    value += " " + muted("(unverified)");
                row("Email", value, revealButton("reveal-email", emailRevealed, "email"));
            } else {
                row("Email", muted("No email address"));
            }
            if (self.phone.hasValue() && !self.phone->empty())
                row("Phone Number", Rml::escape(phoneRevealed ? self.phone.get() : Core::maskPhone(self.phone.get())),
                    revealButton("reveal-phone", phoneRevealed, "phone number"));
            else
                row("Phone Number", muted("No phone number"));
        }

        rml += "<div class=\"account-info-divider\"></div>";
        appendSubsectionHeading(rml, "password-security");
        if (!securityNotice.empty() && securityNoticeAccount == inst->accountId())
            rml += "<div class=\"account-password-notice\" role=\"status\">" + Rml::escape(securityNotice) + "</div>";
        if (!ready) {
            row("Password", muted("Loading..."));
        } else if (!passwordFormOpen) {
            // Reference 007 shows Password with an Edit button; the form opens in place below it.
            row("Password", std::string(), "<button class=\"settings-btn settings-btn-secondary\" data-action=\"open-password-form\" "
                "aria-label=\"Change password\"><span class=\"settings-btn-label\">Edit</span></button>");
        } else {
            rml += "<div id=\"account-password-form\" class=\"account-password-form\">";
            for (const auto &[id, label] : {std::pair{"account-current-password", "Current Password"},
                     std::pair{"account-new-password", "New Password"}, std::pair{"account-confirm-password", "Confirm New Password"}})
                rml += formatRml("<label class=\"settings-label\" for=\"%1\">%2</label>"
                    "<input id=\"%1\" class=\"account-password-input\" type=\"password\" maxlength=\"128\"/>",
                    std::string(id), std::string(label));
            // Shown once Discord asks this account to confirm the change with its second factor.
            rml += formatRml("<div id=\"account-password-mfa\" class=\"account-password-mfa%1\">"
                "<div class=\"settings-description\">Discord needs a code from your authenticator app, or a backup code, "
                "to change this account's password.</div><label class=\"settings-label\" for=\"account-password-code\">"
                "Authentication or backup code</label><input id=\"account-password-code\" class=\"account-password-input\" "
                "type=\"text\" maxlength=\"16\"/></div>", std::string(passwordNeedsCode ? " open" : ""));
            rml += "<div id=\"account-password-error\" class=\"settings-keybind-error\" role=\"alert\"></div>"
                   "<div class=\"settings-button-group\">"
                   "<button class=\"settings-btn settings-btn-secondary\" data-action=\"cancel-password\"><span class=\"settings-btn-label\">Cancel</span></button>"
                   "<button id=\"account-password-submit\" class=\"settings-btn settings-btn-primary\" data-action=\"submit-password\">"
                   "<span class=\"settings-btn-label\">" + std::string(passwordNeedsCode ? "Confirm" : "Done") + "</span></button></div>";
            rml += "</div>";
        }
        // Authenticator app: setup in three steps (password, key and code, backup codes) or removal with a code.
        auto mfaForm = [&rml, this](const std::string &body) {
            rml += "<div class=\"account-password-form account-mfa-form\">" + body +
                   "<div id=\"account-mfa-error\" class=\"settings-keybind-error\" role=\"alert\"></div><div class=\"settings-button-group\">"
                   "<button class=\"settings-btn settings-btn-secondary\" data-action=\"mfa-cancel\"><span class=\"settings-btn-label\">Cancel</span></button>"
                   "<button id=\"account-mfa-submit\" class=\"settings-btn settings-btn-primary\" data-action=\"mfa-submit\"><span class=\"settings-btn-label\">" +
                   std::string(mfaSubmitLabel()) + "</span></button></div></div>";
        };
        auto input = [](const char *id, const char *label, const char *type) {
            return formatRml("<label class=\"settings-label\" for=\"%1\">%2</label><input id=\"%1\" class=\"account-password-input\" type=\"%3\" maxlength=\"128\"/>",
                std::string(id), std::string(label), std::string(type));
        };
        // Codes Discord just returned; used ones are crossed out. They live only in memory while shown.
        auto codeList = [&rml, this](const char *description, const std::string &buttons) {
            rml += std::string("<div class=\"account-password-form\"><div class=\"settings-description\">") + description +
                   "</div><div class=\"account-mfa-codes\">";
            for (const auto &code : mfaBackupCodes)
                rml += code.consumed ? "<span class=\"account-mfa-code consumed\" title=\"Used\">" + Rml::escape(code.code) + "</span>"
                                     : "<span class=\"account-mfa-code\">" + Rml::escape(code.code) + "</span>";
            rml += "</div><div id=\"account-mfa-error\" class=\"settings-keybind-error\" role=\"alert\"></div><div class=\"settings-button-group\">"
                   "<button class=\"settings-btn settings-btn-secondary\" data-action=\"mfa-copy-codes\"><span class=\"settings-btn-label\">Copy Codes</span></button>" +
                   buttons + "<button class=\"settings-btn settings-btn-primary\" data-action=\"mfa-done\"><span class=\"settings-btn-label\">Done</span></button></div></div>";
        };
        const bool mfaOn = ready && self.mfaEnabled.valueOr(false);
        if (!ready) {
            row("Multi-Factor Authentication", muted("Loading..."));
        } else if (mfaStep == MfaStep::Password) {
            row("Multi-Factor Authentication", muted("Not enabled"));
            mfaForm("<div class=\"settings-description\">Confirm your password to set up an authenticator app.</div>" +
                input("account-mfa-password", "Password", "password"));
        } else if (mfaStep == MfaStep::Setup) {
            row("Multi-Factor Authentication", muted("Not enabled"));
            mfaForm(formatRml("<div class=\"settings-description\">Scan this code with an authenticator app such as Google Authenticator, "
                "Microsoft Authenticator or Authy, or enter the key yourself. Then type the 6-digit code it shows.</div>"
                "<div class=\"account-mfa-setup\"><img class=\"account-mfa-qr\" src=\"frame:account-mfa-qr\" alt=\"Authenticator setup code\"/>"
                "<div class=\"account-mfa-key\"><div class=\"settings-label\">Key</div><div class=\"account-mfa-secret\">%1</div>"
                "<div class=\"settings-button-group\"><button class=\"settings-btn settings-btn-secondary\" data-action=\"mfa-copy-key\">"
                "<span class=\"settings-btn-label\">Copy Key</span></button></div></div></div>", Rml::escape(Discord::groupedTotpSecret(mfaSecret))) +
                input("account-mfa-code", "6-digit code", "text"));
        } else if (mfaStep == MfaStep::BackupCodes && securityNoticeAccount == inst->accountId()) {
            row("Multi-Factor Authentication", Rml::escape(std::string("Enabled")));
            codeList("Save these backup codes somewhere safe. Each one signs you in once if you lose your authenticator.", std::string());
        } else if (mfaStep == MfaStep::Remove) {
            row("Multi-Factor Authentication", Rml::escape(std::string("Enabled")));
            mfaForm("<div class=\"settings-description\">Enter a code from your authenticator app, or a backup code, to remove it.</div>" +
                input("account-mfa-code", "Authentication or backup code", "text"));
        } else if (mfaOn) {
            row("Multi-Factor Authentication", Rml::escape(std::string("Enabled")), "<button class=\"settings-btn settings-btn-secondary\" "
                "data-action=\"mfa-remove\" aria-label=\"Remove authenticator app\"><span class=\"settings-btn-label\">Remove</span></button>");
        } else if (self.mfaEnabled.hasValue()) {
            row("Multi-Factor Authentication", muted("Not enabled"), "<button class=\"settings-btn settings-btn-secondary\" "
                "data-action=\"mfa-enable\" aria-label=\"Enable authenticator app\"><span class=\"settings-btn-label\">Enable</span></button>");
        } else {
            row("Multi-Factor Authentication", muted("Unknown"));
        }
        // Text message codes (reference 012) need the authenticator and a verified phone number.
        if (mfaOn && mfaStep != MfaStep::BackupCodes) {
            const bool smsOn = (self.flags.valueOr(0) & Discord::MfaSmsFlag) != 0;
            const bool hasPhone = self.phone.hasValue() && !self.phone->empty();
            if (mfaStep == MfaStep::SmsEnable || mfaStep == MfaStep::SmsRemove) {
                row("SMS Authentication", smsOn ? Rml::escape(std::string("Enabled")) : muted("Not enabled"));
                mfaForm(std::string("<div class=\"settings-description\">") +
                    (mfaStep == MfaStep::SmsEnable
                        ? "Discord will also accept a code sent by text message to " + Rml::escape(Core::maskPhone(self.phone.valueOr(""))) +
                          " when you sign in. Confirm with your password."
                        : std::string("Discord will stop sending sign-in codes by text message. Confirm with your password.")) +
                    "</div>" + input("account-mfa-password", "Password", "password") +
                    formatRml("<div class=\"account-password-mfa%1\"><label class=\"settings-label\" for=\"account-mfa-code\">"
                        "Authentication or backup code</label><input id=\"account-mfa-code\" class=\"account-password-input\" "
                        "type=\"text\" maxlength=\"16\"/></div>", std::string(mfaNeedsCode ? " open" : "")));
            } else if (!self.flags.hasValue()) {
                row("SMS Authentication", muted("Unknown"));
            } else if (smsOn) {
                row("SMS Authentication", Rml::escape(std::string("Enabled")), "<button class=\"settings-btn settings-btn-secondary\" "
                    "data-action=\"sms-remove\" aria-label=\"Remove text message codes\"><span class=\"settings-btn-label\">Remove</span></button>");
            } else if (hasPhone) {
                row("SMS Authentication", muted("Not enabled"), "<button class=\"settings-btn settings-btn-secondary\" "
                    "data-action=\"sms-enable\" aria-label=\"Enable text message codes\"><span class=\"settings-btn-label\">Enable</span></button>");
            } else {
                row("SMS Authentication", muted("Add a verified phone number in the official Discord app first"));
            }
        }
        // Backup codes (reference 012): Discord emails a key before showing them again.
        if (mfaOn && mfaStep != MfaStep::BackupCodes) {
            if (mfaStep == MfaStep::BackupPassword || mfaStep == MfaStep::BackupKey || mfaStep == MfaStep::BackupList)
                row("Backup Codes", std::string());
            else
                row("Backup Codes", std::string(), "<button class=\"settings-btn settings-btn-secondary\" data-action=\"backup-show\">"
                    "<span class=\"settings-btn-label\">Show Backup Codes</span></button>");
            if (mfaStep == MfaStep::BackupPassword)
                mfaForm("<div class=\"settings-description\">Confirm your password. Discord will email you a verification key to show "
                        "your backup codes.</div>" + input("account-mfa-password", "Password", "password"));
            else if (mfaStep == MfaStep::BackupKey)
                mfaForm("<div class=\"settings-description\">Enter the verification key Discord just emailed you.</div>" +
                        input("account-mfa-key", "Verification key", "text"));
            else if (mfaStep == MfaStep::BackupList)
                codeList("Each code signs you in once if you lose your authenticator app. Used codes are crossed out. "
                         "Keep them somewhere safe and don't share them.",
                         backupRegenerateNonce.empty() ? std::string()
                             : "<button class=\"settings-btn settings-btn-secondary\" data-action=\"backup-regenerate\">"
                               "<span class=\"settings-btn-label\">Generate New Codes</span></button>");
        }
        if (ready) {
            row("Security Keys", muted("Not available in Styx"));
            rml += "<div class=\"settings-description\">Discord signs in with security keys and passkeys through WebAuthn, "
                   "which Styx can't use. Add or remove them in the official Discord app.</div>";
        }
        // Logged-in devices (reference 011), loaded when opened.
        if (ready && devicesState == DevicesState::Hidden) {
            row("Logged-in Devices", std::string(), "<button class=\"settings-btn settings-btn-secondary\" data-action=\"devices-show\">"
                "<span class=\"settings-btn-label\">Show Devices</span></button>");
        } else if (ready) {
            row("Logged-in Devices", std::string(), "<button class=\"settings-btn settings-btn-secondary\" data-action=\"devices-hide\">"
                "<span class=\"settings-btn-label\">Hide</span></button>");
            rml += "<div class=\"account-password-form account-devices\">";
            if (devicesState == DevicesState::Loading) {
                rml += "<div class=\"settings-description\">Loading devices...</div>";
            } else if (devicesState == DevicesState::Failed) {
                rml += "<div class=\"settings-keybind-error\" role=\"alert\">" + Rml::escape(devicesError) + "</div>"
                       "<div class=\"settings-button-group\"><button class=\"settings-btn settings-btn-secondary\" data-action=\"devices-show\">"
                       "<span class=\"settings-btn-label\">Try Again</span></button></div>";
            } else {
                const std::string &currentHash = inst->discord()->authSessionIdHash();
                auto device = [&rml](const Discord::AuthSession &session, bool removable) {
                    const bool mobile = session.os == "iOS" || session.os == "Android";
                    std::string title = Rml::escape(session.os.empty() ? std::string("Unknown device") : session.os);
                    if (!session.platform.empty())
                        title += " <span class=\"account-device-platform\">\xE2\x80\xA2 " + Rml::escape(session.platform) + "</span>";
                    std::string detail = Rml::escape(session.location.empty() ? std::string("Unknown location") : session.location);
                    if (removable)
                        if (const auto used = Core::TimeUtils::relativeTime(Platform::DateTime::parse(session.lastUsed)); !used.empty())
                            detail += " \xE2\x80\xA2 " + Rml::escape(used);
                    rml += formatRml("<div class=\"account-device\"><div class=\"account-device-icon\"><img src=\"icon:%1\"/></div>"
                        "<div class=\"account-device-text\"><div class=\"account-device-title\">%2</div><div class=\"account-device-detail\">%3</div></div>",
                        std::string(mobile ? "phone" : "monitor"), title, detail);
                    if (removable)
                        rml += formatRml("<button class=\"account-device-logout\" data-action=\"device-logout\" "
                            "data-session=\"%1\" title=\"Log out this device\" aria-label=\"Log out this device\"><img src=\"icon:x\"/></button>",
                            Rml::escape(session.idHash));
                    rml += "</div>";
                };
                rml += "<div class=\"settings-description\">These are all the devices that are currently logged in with your Discord account. "
                       "Log out of devices you don't recognize.</div><div class=\"account-devices-heading\">Current Device</div>";
                const auto current = std::ranges::find(devices, currentHash, &Discord::AuthSession::idHash);
                if (!currentHash.empty() && current != devices.end())
                    device(*current, false);
                else
                    rml += "<div class=\"settings-description\">Styx couldn't find this login in Discord's list.</div>";
                rml += "<div class=\"account-devices-heading\">Other Devices</div>";
                const bool others = std::ranges::any_of(devices, [&](const auto &session) { return session.idHash != currentHash; });
                for (const auto &session : devices)
                    if (session.idHash != currentHash) device(session, true);
                if (!others) {
                    rml += "<div class=\"settings-description\">No other devices are logged in.</div>";
                } else {
                    rml += "<div class=\"account-devices-heading\">Log out of all known devices</div>"
                           "<div class=\"settings-description\">You'll have to log back in on all logged out devices.</div>"
                           "<div class=\"settings-button-group\"><button class=\"settings-btn settings-btn-danger\" data-action=\"devices-logout-all\">"
                           "<span class=\"settings-btn-label\">Log Out All Known Devices</span></button></div>";
                }
                rml += "<div id=\"account-devices-error\" class=\"settings-keybind-error\" role=\"alert\"></div>";
            }
            rml += "</div>";
        }
        rml += "<div class=\"settings-description\">Changing your username, email or phone number isn't available in Styx yet. "
               "Use the official Discord app for it.</div>";
    }

    rml += "<div class=\"account-info-divider\"></div>";
    appendSubsectionHeading(rml, "styx-accounts");
    rml += "<div class=\"settings-description\">Styx can stay signed in to several Discord accounts at once.</div>"
           "<div class=\"settings-button-group\"><button class=\"settings-btn settings-btn-secondary\" data-action=\"manage-accounts\">"
           "<span class=\"settings-btn-label\">Switch / Manage Accounts</span></button></div>";
}

void SettingsController::renderAppearance(std::string &rml)
{
    using Core::Theme::ThemeChoice;
    const auto appearance = Core::Theme::loadAppearance();
    const auto device = View::systemTheme();
    const auto shown = Core::Theme::effectiveChoice(appearance, device);

    // Discord's Theme section: device following, default themes and account sync.
    appendSubsectionHeading(rml, "theme");
    rml += std::string("<div class=\"settings-card\">");
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
    appendSubsectionHeading(rml, "custom-theme");
    rml += std::string("<div class=\"settings-card\">");
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
    rml += std::string("<div class=\"settings-description\">Text size in chat and zoom level are in Accessibility.</div>");
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
#ifdef STYX_HAVE_MINIAUDIO
    const bool soundsAvailable = true;
#else
    const bool soundsAvailable = false;
#endif
    const bool soundsOn = soundsAvailable && getBool(Keys::SoundsEnabled, Defaults::SoundsEnabled);
    // Individual sounds keep their choice while sounds are off, but cannot be changed until they're back on.
    auto renderSoundSwitch = [this, &rml, soundsOn](Sound sound, const std::string &title, const std::string &desc) {
        const std::string key(Core::Sounds::settingKey(sound));
        rml += "<div class=\"settings-sound-row\">";
        appendSwitch(rml, key, getBool(key, true), title, desc, !soundsOn);
#ifdef STYX_HAVE_MINIAUDIO
        rml += formatRml("<button class=\"settings-btn settings-btn-secondary\" data-action=\"preview-sound\" "
            "data-sound=\"%1\" title=\"Preview %2 sound\"%3><span class=\"settings-btn-label\">Preview</span></button>",
            static_cast<int>(sound), Rml::escape(title), soundsOn ? "" : " disabled=\"disabled\"");
#endif
        rml += "</div>";
    };

    appendSubsectionHeading(rml, "overview");
    rml += std::string("<div class=\"settings-card\">");
    appendSwitch(rml, Keys::DesktopNotifications, getBool(Keys::DesktopNotifications, Defaults::DesktopNotifications),
                 "Desktop notifications",
                 "Show a notification for new messages your server and channel settings allow, unless you're looking at that channel.");
    if (!Platform::Notifications::available())
        rml += std::string("<div class=\"settings-note\">This system isn't accepting notifications from Styx right now.</div>");
    appendSwitch(rml, Keys::FlashTaskbar, getBool(Keys::FlashTaskbar, Defaults::FlashTaskbar),
                 "Flash the taskbar button",
                 "Flash Styx's taskbar button when a notification arrives and the window isn't focused.");
    const bool badgeSupported = Platform::Badge::supported();
    appendSwitch(rml, Keys::TaskbarBadge, badgeSupported && getBool(Keys::TaskbarBadge, Defaults::TaskbarBadge),
                 "Show the unread mention marker",
                 "Mark Styx's taskbar or dock icon while you have unread mentions.", !badgeSupported);
    if (!badgeSupported)
        rml += std::string("<div class=\"settings-note\">This system has no icon marker Styx can set.</div>");
    rml += std::string("<div class=\"settings-button-group\">");
    rml += std::string("<button class=\"settings-btn settings-btn-secondary\" data-action=\"test-notification\"><span class=\"settings-btn-label\">Send a test notification</span></button>");
    rml += std::string("</div>");
    rml += std::string("</div>"); // .settings-card

    appendSubsectionHeading(rml, "sounds");
    rml += std::string("<div class=\"settings-card\">");
    if (soundsAvailable)
        appendSwitch(rml, Keys::SoundsEnabled, soundsOn, "Enable notification sounds",
            "Allow message, mention, incoming-call and voice join/leave sounds.");
    else
        rml += std::string("<div class=\"settings-note\">This build of Styx can't play sounds.</div>");
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
#ifndef STYX_HAVE_SCREENSHARE
    rml += std::string("<div class=\"settings-note\">Screen sharing isn't available in this build of Styx.</div>");
#else
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

    // The Go Live window reads the same preference and also refuses audio where loopback capture is missing.
    const bool audioSupported = Core::Stream::LoopbackCapture::isSupported();
    appendSwitch(rml, Keys::StreamAudio, audioSupported && getBool(Keys::StreamAudio, Defaults::StreamAudio), "Share system audio",
        "Capture application sound when sharing a window, or system audio when sharing screen.", !audioSupported);
    if (!audioSupported)
        rml += std::string("<div class=\"settings-note\">This system can't capture audio for a stream.</div>");
#endif
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

        // Action buttons; disabled ones are inert.
        if (auto *btn = closest(target, "data-action")) {
            if (btn->HasAttribute("disabled")) return;
            std::string action = attribute(btn, "data-action");
            handleAction(action, btn);
            return;
        }

        // Subsection links open their page at the heading
        if (auto *link = closest(target, "data-subsection")) {
            openSubsection(attribute(link, "data-section"), attribute(link, "data-subsection"));
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
    // The highlighted subsection link follows the page while it scrolls.
    listen("scroll", [this](::Rml::Event &event) {
        if (event.GetTargetElement() && event.GetTargetElement()->IsClassSet("settings-content-scroll"))
            trackVisibleSubsection();
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
        // Zoom changes the slider's own size, so a dragged zoom level applies once the drag ends.
        if (pendingZoom) {
            const int zoom = *pendingZoom;
            pendingZoom.reset();
            shell.setInterfaceZoom(zoom);
        }
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
        if (event.GetTargetElement()->IsClassSet("account-password-input")) {
            const bool mfa = event.GetTargetElement()->GetId().starts_with("account-mfa-");
            if (key == ::Rml::Input::KeyIdentifier::KI_RETURN || key == ::Rml::Input::KeyIdentifier::KI_NUMPADENTER) {
                event.StopPropagation();
                if (mfa) submitMfaStep();
                else submitPasswordChange();
                return;
            }
            if (key == ::Rml::Input::KeyIdentifier::KI_ESCAPE && !(mfa ? mfaBusy : passwordSaving)) {
                event.StopPropagation();
                handleAction(mfa ? "mfa-cancel" : "cancel-password", nullptr);
                return;
            }
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
                target->IsClassSet("settings-nav-item") || target->IsClassSet("settings-nav-subitem") || target->IsClassSet("settings-close-btn") ||
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
        const bool minimized = key == Keys::StartMinimized ? nowChecked : getBool(Keys::StartMinimized, Defaults::StartMinimized);
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
    if (key.empty() || val.empty() || handleRemoteRadio(radioEl))
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
    if (sliderEl->IsClassSet("settings-stepped-slider")) {
        handleSteppedSlider(input, key, val);
        return;
    }
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

void SettingsController::handleSteppedSlider(::Rml::ElementFormControlInput *input, const std::string &key, int raw)
{
    const bool zoom = key == Keys::InterfaceZoom;
    const std::span<const int> stops = zoom ? std::span<const int>(Core::Theme::zoomLevels)
                                            : std::span<const int>(Core::Theme::chatTextSizes);
    const int current = zoom ? pendingZoom.value_or(View::interfaceZoom()) : Core::Theme::chatTextSize();
    // Arrow keys move the value by one; take that as a step to the neighbouring stop rather than snapping back.
    const int value = !sliderDragging && std::abs(raw - current) == 1
        ? Core::Theme::stepStop(stops, current, raw - current)
        : Core::Theme::nearestStop(stops, raw);
    showSteppedValue(input, value);
    if (zoom) {
        if (sliderDragging)
            pendingZoom = value;
        else
            shell.setInterfaceZoom(value);
        return;
    }
    if (value == Core::Theme::chatTextSize())
        return;
    auto &theme = Core::Theme::Manager::instance();
    Core::Theme::setChatTextSize(value);
    theme.applyFonts();
    if (!theme.save())
        Core::log(Core::LogLevel::Warning, LogUI) << "SettingsController: could not save the chat text size";
}

void SettingsController::showSteppedValue(::Rml::ElementFormControlInput *input, int value)
{
    // Snaps the knob to the stop and marks its label without reporting another change.
    const bool wasRendering = renderingControls;
    renderingControls = true;
    input->SetValue(std::to_string(value));
    renderingControls = wasRendering;
    ::Rml::ElementList ticks;
    input->GetParentNode()->GetElementsByClassName(ticks, "settings-slider-tick");
    for (auto *tick : ticks)
        tick->SetClass("active", tick->GetAttribute<int>("data-value", 0) == value);
    requestRender();
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

namespace {
// Discord revoked the account's earlier tokens: keep the replacement and reconnect with it, even if
// Settings has closed meanwhile. Returns a message when the sign-in couldn't be saved.
std::string adoptReplacementToken(const Platform::Guard<Core::Session> &session, Core::Snowflake account, const std::string &token)
{
    if (!Core::TokenStore::saveToken(account, token).success())
        return "The change was made, but Styx couldn't save the new sign-in. Use Set Token in Accounts to sign in again.";
    Storage::AccountRepository().clearHeartbeatSession(account);
    Platform::Dispatcher::main().post([session, account] {
        if (!session) return;
        session->disconnectAccount(account);
        session->connectAccount(account);
    });
    return {};
}

void setFormBusy(::Rml::Element *root, const char *form, const char *submitLabel, bool busy)
{
    ::Rml::ElementList controls;
    root->QuerySelectorAll(controls, std::string(form) + " input, " + form + " button");
    for (auto *control : controls) {
        if (busy) control->SetAttribute("disabled", "disabled");
        else control->RemoveAttribute("disabled");
    }
    if (auto *label = root->QuerySelector(std::string(form) + " .settings-btn-primary .settings-btn-label")) label->SetInnerRML(submitLabel);
}

// An account change Discord may guard with a confirmation. A typed code answers an authenticator
// challenge; accounts without a second factor confirm with their password. The change is sent
// again with the confirmation, once.
template <typename T>
struct GuardedChange
{
    std::function<void(Discord::Client &, const std::string &authorization, Discord::Client::GuardedCallback<T>)> send;
    std::function<Discord::Client *()> client;
    std::function<bool()> current;
    std::string password;
    // Empty until the form has asked for one.
    std::string code;
    // The outcome. Also runs once the request is no longer current, so replacement tokens are kept.
    std::function<void(const Core::Result<T> &)> finish;
    std::function<void()> askForCode;
};

template <typename T>
void failGuardedChange(const GuardedChange<T> &change, const char *error)
{
    change.finish(Core::Result<T>::makeError(error));
}

template <typename T>
void runGuardedChange(std::shared_ptr<const GuardedChange<T>> change, const std::string &authorization = {})
{
    auto *client = change->client();
    if (!client) { failGuardedChange(*change, "The account disconnected. Try again."); return; }
    const bool confirmed = !authorization.empty();
    change->send(*client, authorization, [change, confirmed](const Core::Result<T> &result,
                                                            const std::optional<Discord::MfaRequirement> &challenge) {
        if (!challenge) { change->finish(result); return; }
        if (!change->current()) return;
        if (confirmed) { failGuardedChange(*change, "Discord didn't accept the confirmation. Try again."); return; }
        const bool usesCode = challenge->totp || challenge->backup;
        if (!usesCode && !challenge->password) {
            failGuardedChange(*change, "Discord asked for a security key or a text message code, which Styx can't provide. "
                                       "Use the official Discord app for this change.");
            return;
        }
        if (usesCode && change->code.empty()) { change->askForCode(); return; }
        auto *client = change->client();
        if (!client) { failGuardedChange(*change, "The account disconnected. Try again."); return; }
        client->verifyMfa(challenge->ticket, usesCode ? Discord::mfaCodeProof(change->code) : Discord::mfaPasswordProof(change->password),
            [change](const Core::Result<std::string> &verified) {
                if (!change->current()) return;
                if (!verified.success()) { change->finish(Core::Result<T>::makeError(verified.error, verified.code)); return; }
                runGuardedChange(change, *verified.value);
            });
    });
}
}

void SettingsController::showPasswordError(const std::string &message)
{
    if (auto *error = regionRoot->GetElementById("account-password-error"))
        error->SetInnerRML(Rml::escape(message));
    requestRender();
}

void SettingsController::finishPasswordRequest(const std::string &error)
{
    passwordSaving = false;
    if (!regionRoot->GetElementById("account-password-form")) return;
    setFormBusy(regionRoot, "#account-password-form", passwordNeedsCode ? "Confirm" : "Done", false);
    showPasswordError(error);
    // Re-enabled inputs only accept focus once their styles have been updated.
    if (auto *context = regionRoot->GetContext()) context->Update();
    if (auto *input = regionRoot->GetElementById(passwordNeedsCode ? "account-password-code" : "account-current-password")) input->Focus();
}

void SettingsController::submitPasswordChange()
{
    auto *instance = shell.currentInstance();
    if (!instance || !readyClient(shell) || passwordSaving) return;
    auto field = [this](const char *id) {
        return rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(regionRoot->GetElementById(id));
    };
    auto *current = field("account-current-password");
    auto *next = field("account-new-password");
    auto *confirm = field("account-confirm-password");
    auto *code = field("account-password-code");
    if (!current || !next || !confirm || !code) return;
    if (auto problem = Discord::passwordChangeProblem(current->GetValue(), next->GetValue(), confirm->GetValue())) {
        showPasswordError(*problem);
        return;
    }
    if (passwordNeedsCode && code->GetValue().empty()) {
        showPasswordError("Enter a code from your authenticator app or a backup code.");
        return;
    }

    // The form stays in place while the request runs; only its controls are disabled.
    passwordSaving = true;
    setFormBusy(regionRoot, "#account-password-form", "Updating...", true);
    showPasswordError("");

    const auto account = instance->accountId();
    const auto request = ++passwordRequest;
    Platform::Guard<SettingsController> self(this);
    Platform::Guard<Core::Session> session(shell.session());
    auto isCurrent = [self, request] { return self && self->passwordRequest == request; };
    auto change = std::make_shared<GuardedChange<std::string>>();
    change->send = [current = current->GetValue(), next = next->GetValue()](Discord::Client &client, const std::string &authorization,
                                                                          Discord::Client::GuardedCallback<std::string> callback) {
        client.changePassword(current, next, authorization, std::move(callback));
    };
    change->client = [self] { return self ? readyClient(self->shell) : nullptr; };
    change->current = isCurrent;
    change->password = current->GetValue();
    if (passwordNeedsCode) change->code = code->GetValue();
    change->finish = [self, session, account, isCurrent](const Core::Result<std::string> &result) {
        const std::string error = result.success() ? adoptReplacementToken(session, account, *result.value) : result.error;
        if (!isCurrent()) return;
        if (!result.success()) { self->finishPasswordRequest(error); return; }
        self->passwordSaving = false;
        self->passwordFormOpen = false;
        self->securityNotice = error.empty()
            ? "Password changed. Discord signed out your other devices; Styx is reconnecting with the new sign-in."
            : error;
        self->securityNoticeAccount = account;
        if (self->shell.isSettingsOpen() && self->currentSection == Section::MyAccount) self->render();
        self->requestRender();
    };
    change->askForCode = [self] {
        self->passwordNeedsCode = true;
        if (auto *field = self->regionRoot->GetElementById("account-password-mfa")) field->SetClass("open", true);
        self->finishPasswordRequest("");
    };
    runGuardedChange<std::string>(std::move(change));
}

void SettingsController::clearMfaFlow()
{
    mfaStep = MfaStep::None;
    mfaBusy = false;
    mfaNeedsCode = false;
    ++mfaRequest;
    // Overwrite before releasing: a password, a second-factor secret, the emailed key and recovery codes.
    for (auto *secret : {&mfaPassword, &mfaSecret, &mfaAuthorization, &backupKey, &backupNonce, &backupRegenerateNonce}) {
        std::fill(secret->begin(), secret->end(), '\0');
        secret->clear();
    }
    for (auto &code : mfaBackupCodes) std::fill(code.code.begin(), code.code.end(), '\0');
    mfaBackupCodes.clear();
    Runtime::instance().clearFrame("account-mfa-qr");
}

const char *SettingsController::mfaSubmitLabel() const
{
    switch (mfaStep) {
    case MfaStep::Password: return "Continue";
    case MfaStep::Setup: return "Activate";
    case MfaStep::BackupPassword: return "Send Key";
    case MfaStep::BackupKey: return "Show Codes";
    case MfaStep::SmsEnable: return "Enable";
    default: return "Remove";
    }
}

void SettingsController::finishMfaRequest(const std::string &error)
{
    mfaBusy = false;
    if (auto *message = regionRoot->GetElementById("account-mfa-error")) message->SetInnerRML(Rml::escape(error));
    requestRender();
    if (!regionRoot->GetElementById("account-mfa-submit")) return;
    setFormBusy(regionRoot, ".account-mfa-form", mfaSubmitLabel(), false);
    // Re-enabled inputs only accept focus once their styles have been updated.
    if (auto *context = regionRoot->GetContext()) context->Update();
    for (const char *id : {mfaNeedsCode ? "account-mfa-code" : "account-mfa-password", "account-mfa-code", "account-mfa-key"})
        if (auto *input = regionRoot->GetElementById(id)) { input->Focus(); break; }
}

void SettingsController::submitMfaStep()
{
    auto *instance = shell.currentInstance();
    auto *client = readyClient(shell);
    if (!instance || !client || mfaBusy) return;
    auto value = [this](const char *id) {
        auto *input = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(regionRoot->GetElementById(id));
        return input ? input->GetValue() : std::string();
    };
    const auto account = instance->accountId();
    const auto request = ++mfaRequest;
    Platform::Guard<SettingsController> self(this);
    Platform::Guard<Core::Session> session(shell.session());
    auto current = [self, request] { return self && self->mfaRequest == request; };

    if (mfaStep == MfaStep::Password) {
        auto password = value("account-mfa-password");
        if (password.empty()) { finishMfaRequest("Enter your password."); return; }
        mfaBusy = true;
        setFormBusy(regionRoot, ".account-mfa-form", "Checking...", true);
        // Password accepted: keep it and the confirmation for the enable request, and show a new key.
        auto showSetup = [self, current](std::string password, std::string authorization) {
            if (!current()) return;
            self->mfaBusy = false;
            self->mfaPassword = std::move(password);
            self->mfaAuthorization = std::move(authorization);
            try {
                self->mfaSecret = Discord::totpSecretFromBytes(Platform::secureRandomBytes(20));
            } catch (const std::exception &) {
                self->finishMfaRequest("Styx couldn't create a secure key on this system.");
                return;
            }
            std::string name = "Discord";
            if (auto *instance = self->shell.currentInstance()) name = instance->discord()->getMe().username.get();
            if (auto image = qrImage(Discord::totpUri(self->mfaSecret, name), 200))
                Runtime::instance().setFrame("account-mfa-qr", *image);
            self->mfaStep = MfaStep::Setup;
            self->render();
            if (auto *code = self->regionRoot->GetElementById("account-mfa-code")) code->Focus();
        };
        // Discord first asks accounts without a second factor to confirm with their password, then checks it again.
        client->checkTotpPassword(password, {}, [self, current, password, showSetup](const Core::Result<void> &result,
                                                                                    const std::optional<Discord::MfaRequirement> &challenge) {
            if (!current()) return;
            if (result.success()) { showSetup(password, {}); return; }
            if (!challenge) { self->finishMfaRequest(result.error); return; }
            if (!challenge->password) {
                self->finishMfaRequest("Discord wants this account's existing two-factor method. Set up the authenticator in the official Discord app.");
                return;
            }
            auto *client = readyClient(self->shell);
            if (!client) { self->finishMfaRequest("The account disconnected. Try again."); return; }
            client->verifyMfa(challenge->ticket, Discord::mfaPasswordProof(password), [self, current, password, showSetup](const Core::Result<std::string> &verified) {
                if (!current()) return;
                if (!verified.success()) { self->finishMfaRequest(verified.error); return; }
                auto *client = readyClient(self->shell);
                if (!client) { self->finishMfaRequest("The account disconnected. Try again."); return; }
                const auto authorization = *verified.value;
                client->checkTotpPassword(password, authorization, [self, current, password, authorization, showSetup](
                                              const Core::Result<void> &again, const std::optional<Discord::MfaRequirement> &stillGuarded) {
                    if (!current()) return;
                    if (!again.success()) {
                        self->finishMfaRequest(stillGuarded ? "Discord didn't accept the confirmation. Try again." : again.error);
                        return;
                    }
                    showSetup(password, authorization);
                });
            });
        });
    } else if (mfaStep == MfaStep::Setup) {
        const auto code = Discord::normalizedTotpCode(value("account-mfa-code"));
        if (code.empty()) { finishMfaRequest("Enter the 6-digit code from your authenticator app."); return; }
        mfaBusy = true;
        setFormBusy(regionRoot, ".account-mfa-form", "Activating...", true);
        client->enableTotp(mfaPassword, mfaSecret, code, mfaAuthorization, [self, session, current, account](const Core::Result<Discord::TotpEnabled> &result) {
            const std::string error = result.success() ? adoptReplacementToken(session, account, result.value->token) : result.error;
            if (!current()) return;
            if (!result.success()) { self->finishMfaRequest(error); return; }
            auto codes = result.value->backupCodes;
            self->clearMfaFlow();
            self->mfaBackupCodes = std::move(codes);
            self->mfaStep = MfaStep::BackupCodes;
            self->securityNotice = error.empty()
                ? "Authenticator app enabled. Discord signed out your other devices; Styx is reconnecting with the new sign-in."
                : error;
            self->securityNoticeAccount = account;
            if (self->shell.isSettingsOpen() && self->currentSection == Section::MyAccount) self->render();
        });
    } else if (mfaStep == MfaStep::Remove) {
        const auto code = value("account-mfa-code");
        if (code.empty()) { finishMfaRequest("Enter a code from your authenticator app or a backup code."); return; }
        mfaBusy = true;
        setFormBusy(regionRoot, ".account-mfa-form", "Removing...", true);
        auto *discord = client;
        // Discord asks for a second factor first; confirm it with the typed code, then remove with that confirmation.
        discord->disableTotp({}, [self, session, current, account, code](const Core::Result<std::string> &first,
                                                                         const std::optional<Discord::MfaRequirement> &challenge) {
            if (!current()) return;
            auto removed = [self, session, current, account](const Core::Result<std::string> &result) {
                const std::string error = result.success() ? adoptReplacementToken(session, account, *result.value) : result.error;
                if (!current()) return;
                if (!result.success()) { self->finishMfaRequest(error); return; }
                self->clearMfaFlow();
                self->securityNotice = error.empty()
                    ? "Authenticator app removed. Discord signed out your other devices; Styx is reconnecting with the new sign-in."
                    : error;
                self->securityNoticeAccount = account;
                if (self->shell.isSettingsOpen() && self->currentSection == Section::MyAccount) self->render();
            };
            if (!challenge) { removed(first); return; }
            auto *client = readyClient(self->shell);
            if (!client) { self->finishMfaRequest("The account disconnected. Try again."); return; }
            client->verifyMfa(challenge->ticket, Discord::mfaCodeProof(code), [self, current, removed](const Core::Result<std::string> &verified) {
                if (!current()) return;
                if (!verified.success()) { self->finishMfaRequest(verified.error); return; }
                auto *client = readyClient(self->shell);
                if (!client) { self->finishMfaRequest("The account disconnected. Try again."); return; }
                client->disableTotp(*verified.value, [removed](const Core::Result<std::string> &result, const std::optional<Discord::MfaRequirement> &again) {
                    removed(again ? Core::Result<std::string>::makeError("Discord didn't accept the confirmation. Try again.") : result);
                });
            });
        });
    } else if (mfaStep == MfaStep::SmsEnable || mfaStep == MfaStep::SmsRemove) {
        const bool enable = mfaStep == MfaStep::SmsEnable;
        auto password = value("account-mfa-password");
        auto code = value("account-mfa-code");
        if (password.empty()) { finishMfaRequest("Enter your password."); return; }
        if (mfaNeedsCode && code.empty()) { finishMfaRequest("Enter a code from your authenticator app or a backup code."); return; }
        mfaBusy = true;
        setFormBusy(regionRoot, ".account-mfa-form", enable ? "Enabling..." : "Removing...", true);
        auto change = std::make_shared<GuardedChange<void>>();
        change->send = [enable, password](Discord::Client &client, const std::string &authorization, Discord::Client::GuardedCallback<void> callback) {
            client.setSmsMfa(enable, password, authorization, std::move(callback));
        };
        change->client = [self] { return self ? readyClient(self->shell) : nullptr; };
        change->current = current;
        change->password = password;
        if (mfaNeedsCode) change->code = std::move(code);
        change->finish = [self, current, account, enable](const Core::Result<void> &result) {
            if (!current()) return;
            if (!result.success()) { self->finishMfaRequest(result.error); return; }
            self->clearMfaFlow();
            self->securityNotice = enable ? "Text message codes enabled. Discord can now send sign-in codes to your phone."
                                          : "Text message codes removed.";
            self->securityNoticeAccount = account;
            if (self->shell.isSettingsOpen() && self->currentSection == Section::MyAccount) self->render();
        };
        change->askForCode = [self] {
            self->mfaNeedsCode = true;
            if (auto *field = self->regionRoot->QuerySelector(".account-mfa-form .account-password-mfa")) field->SetClass("open", true);
            self->finishMfaRequest("");
        };
        runGuardedChange<void>(std::move(change));
    } else if (mfaStep == MfaStep::BackupPassword) {
        auto password = value("account-mfa-password");
        if (password.empty()) { finishMfaRequest("Enter your password."); return; }
        mfaBusy = true;
        setFormBusy(regionRoot, ".account-mfa-form", "Sending...", true);
        client->requestBackupCodes(password, [self, current](const Core::Result<Discord::BackupCodeNonces> &result) {
            if (!current()) return;
            if (!result.success()) { self->finishMfaRequest(result.error); return; }
            self->mfaBusy = false;
            self->backupNonce = result.value->view;
            self->backupRegenerateNonce = result.value->regenerate;
            self->mfaStep = MfaStep::BackupKey;
            self->render();
            if (auto *key = self->regionRoot->GetElementById("account-mfa-key")) key->Focus();
        });
    } else if (mfaStep == MfaStep::BackupKey) {
        auto key = value("account-mfa-key");
        std::erase_if(key, [](unsigned char c) { return std::isspace(c); });
        if (key.empty()) { finishMfaRequest("Enter the verification key from the email."); return; }
        mfaBusy = true;
        setFormBusy(regionRoot, ".account-mfa-form", "Checking...", true);
        client->fetchBackupCodes(key, backupNonce, false, [self, current, key](const Core::Result<std::vector<Discord::BackupCode>> &result) {
            if (!current()) return;
            if (!result.success()) { self->finishMfaRequest(result.error); return; }
            self->mfaBusy = false;
            // The viewing nonce is spent; the key stays for generating new codes.
            std::fill(self->backupNonce.begin(), self->backupNonce.end(), '\0');
            self->backupNonce.clear();
            self->backupKey = key;
            self->mfaBackupCodes = *result.value;
            self->mfaStep = MfaStep::BackupList;
            self->render();
            if (auto *done = self->regionRoot->QuerySelector("button[data-action=\"mfa-done\"]")) done->Focus();
        });
    }
}

void SettingsController::hideDevices()
{
    devicesState = DevicesState::Hidden;
    devices.clear();
    devicesError.clear();
    devicesBusy = false;
    ++devicesRequest;
}

void SettingsController::loadDevices()
{
    auto *client = readyClient(shell);
    if (!client) return;
    devicesState = DevicesState::Loading;
    devicesBusy = false;
    const auto request = ++devicesRequest;
    Platform::Guard<SettingsController> self(this);
    client->fetchAuthSessions([self, request](const Core::Result<std::vector<Discord::AuthSession>> &result) {
        if (!self || self->devicesRequest != request) return;
        if (result.success()) {
            self->devices = *result.value;
            self->devicesState = DevicesState::Loaded;
        } else {
            self->devicesError = result.error;
            self->devicesState = DevicesState::Failed;
        }
        if (!self->shell.isSettingsOpen() || self->currentSection != Section::MyAccount) return;
        self->render();
        if (auto *hide = self->regionRoot->QuerySelector("button[data-action=\"devices-hide\"]")) hide->Focus();
    });
    render();
    if (auto *hide = regionRoot->QuerySelector("button[data-action=\"devices-hide\"]")) hide->Focus();
}

void SettingsController::logoutDevices(std::vector<std::string> idHashes)
{
    auto *client = readyClient(shell);
    if (!client || devicesBusy || idHashes.empty()) return;
    // Discord takes at most 64 per request; the refreshed list shows any that remain.
    if (idHashes.size() > Discord::MaxSessionsPerLogout) idHashes.resize(Discord::MaxSessionsPerLogout);
    devicesBusy = true;
    ::Rml::ElementList buttons;
    regionRoot->QuerySelectorAll(buttons, ".account-devices button");
    for (auto *button : buttons) button->SetAttribute("disabled", "disabled");
    const auto request = devicesRequest;
    Platform::Guard<SettingsController> self(this);
    client->logoutAuthSessions(idHashes, [self, request](const Core::Result<void> &result) {
        if (!self || self->devicesRequest != request) return;
        self->devicesBusy = false;
        if (result.success()) {
            self->loadDevices();
            return;
        }
        ::Rml::ElementList buttons;
        self->regionRoot->QuerySelectorAll(buttons, ".account-devices button");
        for (auto *button : buttons) button->RemoveAttribute("disabled");
        if (auto *error = self->regionRoot->GetElementById("account-devices-error")) error->SetInnerRML(Rml::escape(result.error));
        self->requestRender();
    });
}

void SettingsController::regenerateBackupCodes()
{
    auto *client = readyClient(shell);
    if (mfaStep != MfaStep::BackupList || mfaBusy || backupRegenerateNonce.empty()) return;
    if (!client) { finishMfaRequest("The account disconnected. Try again."); return; }
    auto *button = regionRoot->QuerySelector("button[data-action=\"backup-regenerate\"]");
    if (!button) return;
    mfaBusy = true;
    button->SetAttribute("disabled", "disabled");
    const auto request = ++mfaRequest;
    Platform::Guard<SettingsController> self(this);
    client->fetchBackupCodes(backupKey, backupRegenerateNonce, true, [self, request](const Core::Result<std::vector<Discord::BackupCode>> &result) {
        if (!self || self->mfaRequest != request) return;
        if (!result.success()) {
            if (auto *button = self->regionRoot->QuerySelector("button[data-action=\"backup-regenerate\"]")) button->RemoveAttribute("disabled");
            self->finishMfaRequest(result.error);
            return;
        }
        self->mfaBusy = false;
        // Discord accepts the regeneration nonce once.
        std::fill(self->backupRegenerateNonce.begin(), self->backupRegenerateNonce.end(), '\0');
        self->backupRegenerateNonce.clear();
        for (auto &code : self->mfaBackupCodes) std::fill(code.code.begin(), code.code.end(), '\0');
        self->mfaBackupCodes = *result.value;
        self->render();
        if (auto *done = self->regionRoot->QuerySelector("button[data-action=\"mfa-done\"]")) done->Focus();
    });
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
    } else if (action == std::string("open-password-form")) {
        passwordFormOpen = true;
        passwordNeedsCode = false;
        securityNotice.clear();
        render();
        if (auto *input = regionRoot->GetElementById("account-current-password")) input->Focus();
    } else if (action == std::string("cancel-password")) {
        if (passwordSaving) return;
        passwordFormOpen = false;
        render();
        if (auto *edit = regionRoot->QuerySelector("button[data-action=\"open-password-form\"]")) edit->Focus();
    } else if (action == std::string("submit-password")) {
        submitPasswordChange();
    } else if (action == "mfa-enable" || action == "mfa-remove" || action == "backup-show" || action == "sms-enable" || action == "sms-remove") {
        clearMfaFlow();
        mfaStep = action == "mfa-enable" ? MfaStep::Password : action == "mfa-remove" ? MfaStep::Remove
                : action == "sms-enable" ? MfaStep::SmsEnable : action == "sms-remove" ? MfaStep::SmsRemove : MfaStep::BackupPassword;
        securityNotice.clear();
        render();
        if (auto *input = regionRoot->QuerySelector(".account-mfa-form input")) input->Focus();
    } else if (action == std::string("mfa-submit")) {
        submitMfaStep();
    } else if (action == std::string("backup-regenerate")) {
        if (mfaBusy || backupRegenerateNonce.empty()) return;
        Platform::Guard<SettingsController> self(this);
        const auto request = mfaRequest;
        showConfirmModal(shell, "Generate new backup codes?",
            "Your current backup codes stop working. Save the new ones somewhere safe.", "Generate New Codes",
            [self, request] { if (self && self->mfaRequest == request) self->regenerateBackupCodes(); }, nullptr, true);
    } else if (action == std::string("mfa-cancel") || action == std::string("mfa-done")) {
        if (mfaBusy) return;
        // Focus returns to the button that opened the flow.
        const char *opener = [this] {
            switch (mfaStep) {
            case MfaStep::BackupCodes: case MfaStep::Remove: return "mfa-remove";
            case MfaStep::Password: case MfaStep::Setup: return "mfa-enable";
            case MfaStep::SmsEnable: return "sms-enable";
            case MfaStep::SmsRemove: return "sms-remove";
            default: return "backup-show";
            }
        }();
        clearMfaFlow();
        render();
        if (auto *button = regionRoot->QuerySelector(std::string("button[data-action=\"") + opener + "\"]")) button->Focus();
    } else if (action == std::string("devices-show")) {
        loadDevices();
    } else if (action == std::string("devices-hide")) {
        hideDevices();
        render();
        if (auto *show = regionRoot->QuerySelector("button[data-action=\"devices-show\"]")) show->Focus();
    } else if (action == std::string("device-logout") || action == std::string("devices-logout-all")) {
        auto *client = readyClient(shell);
        if (!client || devicesBusy || devicesState != DevicesState::Loaded) return;
        const bool all = action == std::string("devices-logout-all");
        const std::string &currentHash = client->authSessionIdHash();
        std::vector<std::string> hashes;
        std::string body = "Every device except this one will have to log in again.";
        if (all) {
            for (const auto &session : devices)
                if (session.idHash != currentHash) hashes.push_back(session.idHash);
        } else {
            const auto hash = target ? target->GetAttribute<std::string>("data-session", "") : std::string();
            const auto session = std::ranges::find(devices, hash, &Discord::AuthSession::idHash);
            if (session == devices.end() || hash == currentHash) return;
            hashes.push_back(hash);
            body = (session->os.empty() ? std::string("This device") : session->os) +
                   (session->platform.empty() ? std::string() : " (" + session->platform + ")") + " will have to log in again.";
        }
        Platform::Guard<SettingsController> self(this);
        const auto request = devicesRequest;
        showConfirmModal(shell, all ? "Log out all known devices?" : "Log out this device?", body, "Log Out",
            [self, request, hashes = std::move(hashes)] {
                if (self && self->devicesRequest == request) self->logoutDevices(hashes);
            }, nullptr, true);
    } else if (action == std::string("mfa-copy-key")) {
        Platform::Desktop::setClipboardText(mfaSecret);
    } else if (action == std::string("mfa-copy-codes")) {
        std::string text;
        for (const auto &code : mfaBackupCodes)
            if (!code.consumed) text += code.code + "\n";
        Platform::Desktop::setClipboardText(text);
    } else if (action == std::string("reveal-email")) {
        emailRevealed = !emailRevealed;
        render();
    } else if (action == std::string("reveal-phone")) {
        phoneRevealed = !phoneRevealed;
        render();
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
