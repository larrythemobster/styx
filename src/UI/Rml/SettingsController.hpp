#pragma once

#include "Controller.hpp"
#include "Platform/Native.hpp"
#include "Platform/Timer.hpp"
#include <string>

namespace Styx {
namespace Core {
class ClientInstance;
}
namespace UI {
namespace Rml {

class SettingsController final : public Controller
{
public:
    enum class Section {
        MyAccount,
        Appearance,
        Chat,
        Notifications,
        VoiceVideo,
        GoLive,
        Advanced,
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

    void renderMyAccount(std::string &rml);
    void renderAppearance(std::string &rml);
    void renderChat(std::string &rml);
    void renderNotifications(std::string &rml);
    void renderVoiceVideo(std::string &rml);
    void renderGoLive(std::string &rml);
    void renderAdvanced(std::string &rml);

    void setupEventListeners();
    void handleSwitchClick(::Rml::Element *switchEl);
    void handleRadioClick(::Rml::Element *radioEl);
    void handleSliderChange(::Rml::Element *sliderEl);
    void handleSelectChange(::Rml::Element *selectEl);
    void handleAction(const std::string &action, ::Rml::Element *target);

    Section currentSection = Section::MyAccount;
    Platform::Timer rebuildTimer;
    Platform::Timer hideTransitionTimer;
    Platform::Color themeSeedColor;
    int themeSchemeIndex = 0;
    bool themeDarkMode = true;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
