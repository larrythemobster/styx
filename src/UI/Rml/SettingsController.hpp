#pragma once

#include "Controller.hpp"
#include <QPointer>
#include <QString>
#include <QTimer>

namespace Styx {
namespace Core {
class ClientInstance;
}
namespace UI {
namespace Rml {

class SettingsController final : public Controller
{
    Q_OBJECT
public:
    enum class Section {
        MyAccount,
        Appearance,
        Chat,
        VoiceVideo,
        GoLive,
        Advanced,
    };

    SettingsController(Shell &shell, ::Rml::Element *root, QObject *parent = nullptr);
    ~SettingsController() override;

    void setSection(Section section);
    [[nodiscard]] Section section() const { return currentSection; }

private slots:
    void onSettingsOpenChanged(bool open);
    void rebuild();

private:
    void render();
    void renderSidebar(QString &rml);
    void renderContent(QString &rml);

    void renderMyAccount(QString &rml);
    void renderAppearance(QString &rml);
    void renderChat(QString &rml);
    void renderVoiceVideo(QString &rml);
    void renderGoLive(QString &rml);
    void renderAdvanced(QString &rml);

    void setupEventListeners();
    void handleSwitchClick(::Rml::Element *switchEl);
    void handleRadioClick(::Rml::Element *radioEl);
    void handleSliderChange(::Rml::Element *sliderEl);
    void handleSelectChange(::Rml::Element *selectEl);
    void handleAction(const QString &action, ::Rml::Element *target);

    Section currentSection = Section::MyAccount;
    QTimer rebuildTimer;
    QTimer hideTransitionTimer;
    QColor themeSeedColor;
    int themeSchemeIndex = 0;
    bool themeDarkMode = true;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
