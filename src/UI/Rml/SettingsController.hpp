#pragma once

#include "Controller.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/AccountSecurity.hpp"
#include "Core/Theme/Appearance.hpp"
#include "Keybinds.hpp"
#include "Platform/Native.hpp"
#include "Platform/Timer.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <RmlUi/Core/ObserverPtr.h>

namespace Rml { class ElementFormControlInput; }

namespace Styx {
namespace Core {
class ClientInstance;
}
namespace Discord { class Client; }
namespace Proto { struct PreloadedSettingsEdit; }
namespace UI {
namespace Rml {
class ProfileEditorController;


class SettingsController final : public Controller
{
public:
    // Pages in Discord's settings order. Several pages combine former Styx pages as subsections.
    enum class Section {
        MyAccount,
        Profiles,
        MessagingPermissions,
        Notifications,
        VoiceVideo,
        Appearance,
        Accessibility,
        System,
        Language,
        ActivityPrivacy,
    };

    SettingsController(Shell &shell, ::Rml::Element *root);
    ~SettingsController() override;

    void setSection(Section section);
    [[nodiscard]] Section section() const { return currentSection; }

private:
    void onSettingsOpenChanged(bool open);
    void rebuild();

private:
    void render();
    void renderSidebar(std::string &rml);
    void renderContent(std::string &rml);
    void renderNavigation(std::string &rml);
    void refreshNavigation();
    void selectSection(const std::string &id);
    // Opens a page at one of its subsection headings, as Discord's nested sidebar links do.
    void openSubsection(const std::string &sectionId, const std::string &subsectionId);
    static void appendSubsectionHeading(std::string &rml, std::string_view id);
    void trackVisibleSubsection();
    std::string_view sectionTitle() const;
    void bindAccountSettings();
    bool isRemoteSection() const;
    bool confirmProfileLeave(std::function<void()> proceed);
    void patchRemoteSettings(Proto::PreloadedSettingsEdit edit);
    bool handleRemoteSwitch(::Rml::Element *element);
    void renderRemoteStatus(std::string &rml);
    void renderProfiles(std::string &rml);
    void renderPrivacy(std::string &rml);
    void renderFriendRequests(std::string &rml);
    void renderAccessibility(std::string &rml);
    void renderActivityPrivacy(std::string &rml);
    void renderSystem(std::string &rml);
    void renderLanguage(std::string &rml);
    static void appendSwitch(std::string &rml, const std::string &key, bool checked,
                             const std::string &title, const std::string &description, bool disabled = false);
    // A slider with labeled stops that snaps to them, as Discord's text size and zoom sliders do.
    static void appendSteppedSlider(std::string &rml, const std::string &id, const std::string &key, const std::string &title,
                                    const std::string &description, std::span<const int> stops, int value, const std::string &unit);

    void renderMyAccount(std::string &rml);
    void renderAppearance(std::string &rml);
    void applyGeneratedTheme();
    void applyAppearanceChoice(Core::Theme::Appearance appearance, bool explicitChoice);
    void selectDefaultTheme(Core::Theme::ThemeChoice choice);
    void pushThemeToAccount(Core::Theme::ThemeChoice choice);
    void adoptAccountTheme();
    void onDeviceThemeChanged();
    static Discord::Client *readyClient(Shell &shell);
    void readPickerColor();
    void refreshColorPicker(bool updateHex = true);
    void pickColorAt(float x, float y);
    void applyPickerColor();
    void renderChat(std::string &rml);
    void renderNotifications(std::string &rml);
    void renderVoiceVideo(std::string &rml);
    void renderKeybinds(std::string &rml);
    void renderGoLive(std::string &rml);
    void renderAdvanced(std::string &rml);

    void setupEventListeners();
    void handleSwitchClick(::Rml::Element *switchEl);
    void handleRadioClick(::Rml::Element *radioEl);
    void handleSliderChange(::Rml::Element *sliderEl);
    void showSliderHint(::Rml::Element *sliderEl);
    void handleSteppedSlider(::Rml::ElementFormControlInput *input, const std::string &key, int raw);
    void showSteppedValue(::Rml::ElementFormControlInput *input, int value);
    void clearSliderHint();
    void handleSelectChange(::Rml::Element *selectEl);
    void handleAction(const std::string &action, ::Rml::Element *target);
    void submitPasswordChange();
    // Sends the change, confirming Discord's challenge with the typed code (or the password on
    // accounts without a second factor) and retrying with the confirmation.
    void sendPasswordChange(std::uint64_t request, Core::Snowflake account, std::string current, std::string next,
                            std::string code, std::string authorization);
    void finishPasswordRequest(const std::string &error);
    void showPasswordError(const std::string &message);
    // Authenticator app setup/removal and backup codes: runs the step that's on screen.
    void submitMfaStep();
    void finishMfaRequest(const std::string &error);
    [[nodiscard]] const char *mfaSubmitLabel() const;
    // Replaces the backup codes on screen with new ones, after confirmation.
    void regenerateBackupCodes();
    void clearMfaFlow();
    void renderKeybindRow(std::string &rml, KeybindAction action);
    void refreshKeybinds();
    void stopRecordingKeybind();
    void recordConfiguredKey(int key, int rmlModifiers);
    void applyPushToTalk();

    Section currentSection = Section::MyAccount;
    Platform::Timer rebuildTimer;
    Platform::Color themeSeedColor;
    int themeSchemeIndex = 0;
    bool themeDarkMode = true;
    std::string themeError;
    std::string appearanceSaveError;
    std::string searchQuery;
    std::string currentSubsection;
    Platform::Timer subsectionTimer;
    std::string remoteSettingsError;
    bool remoteSettingsSaving = false;
    std::uint64_t remoteSettingsRequest = 0;
    Platform::Connection accountSettingsConnection;
    bool colorPickerOpen = false;
    // Account page: whether the full email/phone are shown. Cleared when leaving the page or account.
    bool emailRevealed = false;
    bool phoneRevealed = false;
    // Account page password change. Passwords live only in the form's inputs and the request in flight.
    bool passwordFormOpen = false;
    bool passwordSaving = false;
    // Discord asked this account for an authenticator or backup code; the form shows a field for it.
    bool passwordNeedsCode = false;
    std::uint64_t passwordRequest = 0;
    // Outcome of the last password or authenticator change, shown for that account only.
    std::string securityNotice;
    Core::Snowflake securityNoticeAccount;
    // Authenticator app and backup code flows. The password, secret, emailed key, nonces and Discord's
    // confirmation are held only between their steps and the backup codes only until the user leaves
    // them; clearMfaFlow wipes them all. BackupCodes follows setup; BackupPassword, BackupKey and
    // BackupList show the codes again later.
    enum class MfaStep { None, Password, Setup, BackupCodes, Remove, BackupPassword, BackupKey, BackupList };
    MfaStep mfaStep = MfaStep::None;
    bool mfaBusy = false;
    std::uint64_t mfaRequest = 0;
    std::string mfaPassword;
    std::string mfaSecret;
    std::string mfaAuthorization;
    std::string backupKey;
    std::string backupNonce;
    std::string backupRegenerateNonce;
    std::vector<Discord::BackupCode> mfaBackupCodes;
    bool colorDragging = false;
    double pickerHue = 0;
    double pickerSaturation = 0;
    double pickerBrightness = 0;
    bool renderingControls = false;
    ::Rml::Element *hintSlider = nullptr;
    ::Rml::ObserverPtr<::Rml::Element> previousFocus;
    bool sliderDragging = false;
    std::optional<int> pendingZoom;
    std::optional<KeybindAction> recordingKeybind;
    std::string keybindError;
    std::unique_ptr<ProfileEditorController> profileEditor;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
