#pragma once

#include "Controller.hpp"
#include "Core/Theme/Appearance.hpp"
#include "Keybinds.hpp"
#include "Platform/Native.hpp"
#include "Platform/Timer.hpp"
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
    enum class Section {
        MyAccount,
        Appearance,
        Chat,
        Notifications,
        VoiceVideo,
        Keybinds,
        GoLive,
        Advanced,
        Profiles,
        Privacy,
        FriendRequests,
        Accessibility,
        ActivityPrivacy,
        System,
        Language,
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
    std::string remoteSettingsError;
    bool remoteSettingsSaving = false;
    std::uint64_t remoteSettingsRequest = 0;
    Platform::Connection accountSettingsConnection;
    bool colorPickerOpen = false;
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
