#pragma once

#include "Controller.hpp"
#include "Core/Stream/ScreenCapture.hpp"
#include "Core/Stream/StreamQuality.hpp"

#include <QList>
#include <QSet>
#include <QString>
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace Rml {
class ElementDocument;
}

namespace Styx {
namespace UI {
class Shell;
namespace Rml {

class GoLiveController final : public Controller
{
    Q_OBJECT
public:
    explicit GoLiveController(Shell &shell, QObject *parent = nullptr);
    ~GoLiveController() override;

    void open(bool screensTab = false);
    void close();
    [[nodiscard]] bool isOpen() const { return visible; }

private:
    enum class Tab {
        Applications,
        Screens
    };

    void ensureDocument();
    void setupEventListeners();
    void populate();
    void renderSources();
    void renderQualityPresets();
    void updateAudioUi();
    void updateGoLiveButton();
    void switchTab(Tab tab);
    void selectSource(int index);
    void toggleAudio();
    void goLive();

    void startThumbnailCapture();
    void stopThumbnailCapture();
    void updateThumbnailSources();

    ::Rml::ElementDocument *modalDoc = nullptr;
    bool visible = false;
    Tab currentTab = Tab::Applications;

    QList<Core::Stream::CaptureSource> screenSources;
    QList<Core::Stream::CaptureSource> appSources;
    int selectedSourceIndex = -1;

    QString selectedQualityId;
    bool audioEnabled = true;
    bool audioSupported = true;

    QSet<QString> registeredFrames;

    std::atomic<bool> thumbnailRunning{false};
    std::thread thumbnailThread;
    std::mutex thumbnailMutex;
    std::condition_variable thumbnailCv;
    QList<Core::Stream::CaptureSource> thumbnailSources;
    std::uint64_t thumbnailGeneration = 0;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
