#include "GoLiveController.hpp"

#include "Shell.hpp"
#include "RmlRuntime.hpp"
#include "RmlView.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/Stream/LoopbackCapture.hpp"
#include "Core/Stream/StreamManager.hpp"
#include "Platform/Dispatcher.hpp"

#include "Platform/Settings.hpp"

#include <charconv>
#include <chrono>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#endif

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

namespace Styx {
namespace UI {
namespace Rml {

GoLiveController::GoLiveController(Shell &shell)
    : Controller(shell, nullptr)
{
}

GoLiveController::~GoLiveController()
{
    stopThumbnailCapture();
    for (const auto &key : registeredFrames) {
        Runtime::instance().clearFrame(key);
    }
    registeredFrames.clear();

    if (modalDoc) {
        modalDoc->Close();
        modalDoc = nullptr;
        regionRoot = nullptr;
    }
}

void GoLiveController::ensureDocument()
{
    if (modalDoc)
        return;

    auto *view = shell.view();
    if (!view || !view->context())
        return;

    modalDoc = view->context()->LoadDocument("golive.rml");
    if (!modalDoc)
        return;

    regionRoot = modalDoc;
    setupEventListeners();
}

void GoLiveController::setupEventListeners()
{
    listen("click", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        if (!target)
            return;
        auto *control = closest(target, "id");
        const auto id = control ? control->GetId() : std::string();
        if (id == "golive-close-btn" || target == modalDoc->GetElementById("golive-backdrop")) {
            close();
            return;
        }
        if (id == "golive-quality-button") {
            setQualityMenuOpen(!qualityMenuOpen);
            return;
        }
        if (auto *menu = closest(target, "data-quality-menu")) {
            const auto value = attribute(menu, "data-quality-menu");
            const auto quality = Core::Stream::StreamQuality::byId(selectedQualityId);
            if (value == "gaming" || value == "screenshare" || value == "custom") {
                streamMode = value;
                if (value == "gaming") selectedQualityId = "1440p60";
                if (value == "screenshare") selectedQualityId = "source15";
            } else if (value == "audio") {
                if (audioSupported) audioEnabled = !audioEnabled;
            } else if (value == "cursor") {
                captureCursor = !captureCursor;
            } else if (value.starts_with("height:") || value.starts_with("fps:")) {
                const auto text = value.substr(value.find(':') + 1);
                int number = 0;
                std::from_chars(text.data(), text.data() + text.size(), number);
                selectedQualityId = Core::Stream::StreamQuality::custom(
                    value.starts_with("height:") ? number : quality.size.height,
                    value.starts_with("fps:") ? number : quality.framerate).id;
                streamMode = "custom";
            } else {
                qualityPage = value == "back" ? "" : value;
            }
            savePreferences();
            updateQualitySummary();
            renderQualityMenu();
            requestRender();
            return;
        }
        setQualityMenuOpen(false);
        if (auto *tab = closest(target, "data-tab")) {
            switchTab(attribute(tab, "data-tab") == "screens" ? Tab::Screens : Tab::Applications);
            return;
        }
        if (auto *share = closest(target, "data-share")) {
            const auto text = attribute(share, "data-share");
            int index = -1;
            std::from_chars(text.data(), text.data() + text.size(), index);
            selectSource(index);
            goLive();
            return;
        }
        if (auto *card = closest(target, "data-index")) {
            const auto text = attribute(card, "data-index");
            int index = -1;
            std::from_chars(text.data(), text.data() + text.size(), index);
            selectSource(index);
        }
    }, modalDoc);
    listen("dblclick", [this](::Rml::Event &event) {
        if (auto *card = closest(event.GetTargetElement(), "data-index")) {
            const auto text = attribute(card, "data-index");
            int index = -1;
            std::from_chars(text.data(), text.data() + text.size(), index);
            selectSource(index);
            goLive();
        }
    }, modalDoc);
    listen("keydown", [this](::Rml::Event &event) {
        const auto key = event.GetParameter("key_identifier", 0);
        if (key == ::Rml::Input::KI_ESCAPE) {
            if (qualityMenuOpen) setQualityMenuOpen(false);
            else close();
            event.StopPropagation();
        } else if (key == ::Rml::Input::KI_RETURN) {
            auto *target = event.GetTargetElement();
            if (target && target->IsClassSet("golive-card")) {
                const auto text = attribute(target, "data-index");
                int index = -1;
                std::from_chars(text.data(), text.data() + text.size(), index);
                selectSource(index);
                goLive();
                event.StopPropagation();
            }
        }
    }, modalDoc);
}

void GoLiveController::open(bool screensTab)
{
    ensureDocument();
    if (!modalDoc)
        return;

    stopThumbnailCapture();
    populate();
    if (screensTab) {
        switchTab(Tab::Screens);
    }
    visible = true;
    startThumbnailCapture();

    modalDoc->Show(::Rml::ModalFlag::Modal);

    if (shell.view())
        shell.view()->requestRender(200);
}

void GoLiveController::close()
{
    setQualityMenuOpen(false);
    stopThumbnailCapture();

    for (const auto &key : registeredFrames) {
        Runtime::instance().clearFrame(key);
    }
    registeredFrames.clear();

    if (modalDoc) {
        modalDoc->Hide();
    }
    visible = false;

    if (shell.view())
        shell.view()->requestRender(100);
}

void GoLiveController::populate()
{
    const auto sources = Core::Stream::enumerateCaptureSources();
#ifdef _WIN32
    const auto ownPid = GetCurrentProcessId();
#endif

    screenSources.clear();
    appSources.clear();

    for (const auto &source : sources) {
#ifdef _WIN32
        if (source.kind == Core::Stream::CaptureSource::Kind::Window &&
            source.processId == ownPid) {
            continue; // Sharing ourselves only shows recursive previews
        }
#endif
        if (source.kind == Core::Stream::CaptureSource::Kind::Monitor) {
            screenSources.push_back(source);
        } else {
            appSources.push_back(source);
        }
    }

    if (currentTab == Tab::Applications && appSources.empty() && !screenSources.empty()) {
        currentTab = Tab::Screens;
    }

    // Default source selection
    selectedSourceIndex = -1;
    if (currentTab == Tab::Screens) {
        for (size_t i = 0; i < screenSources.size(); ++i) {
            if (screenSources[i].primary) {
                selectedSourceIndex = static_cast<int>(i);
                break;
            }
        }
        if (selectedSourceIndex < 0 && !screenSources.empty())
            selectedSourceIndex = 0;
    } else {
        if (!appSources.empty())
            selectedSourceIndex = 0;
    }

    auto &settings = Platform::Settings::instance();
    selectedQualityId = Core::Stream::StreamQuality::byId(settings.getString("stream/quality", "720p30")).id;
    streamMode = settings.getString("stream/mode", "custom");
    if ((streamMode == "gaming" && selectedQualityId != "1440p60") ||
        (streamMode == "screenshare" && selectedQualityId != "source15") ||
        (streamMode != "gaming" && streamMode != "screenshare"))
        streamMode = "custom";
    captureCursor = settings.getBool("stream/capture_cursor", true);
    audioSupported = Core::Stream::LoopbackCapture::isSupported();
    audioEnabled = audioSupported && settings.getBool("stream/audio", true);

    // Update Tab UI
    if (auto *appTab = modalDoc->GetElementById("tab-applications"))
        appTab->SetClass("active", currentTab == Tab::Applications);
    if (auto *screenTab = modalDoc->GetElementById("tab-screens"))
        screenTab->SetClass("active", currentTab == Tab::Screens);

    renderSources();
    updateQualitySummary();
    updateSharingAvailability();
}

void GoLiveController::renderSources()
{
    if (!modalDoc)
        return;
    auto *container = modalDoc->GetElementById("golive-sources");
    if (!container)
        return;

    const auto &sources = (currentTab == Tab::Screens) ? screenSources : appSources;
    if (sources.empty()) {
        container->SetInnerRML("<div class=\"golive-empty\">No capture sources available</div>");
        return;
    }

    std::string rml;
    for (size_t i = 0; i < sources.size(); ++i) {
        const auto &s = sources[i];
        const std::string frameKey = "golive_thumb_" +
            std::string(s.kind == Core::Stream::CaptureSource::Kind::Monitor ? "mon_" : "win_") +
            std::to_string(s.handle);
        registeredFrames.insert(frameKey);

        const bool isSelected = static_cast<int>(i) == selectedSourceIndex;
        std::string title = s.title;
        if (title.empty()) {
            title = s.kind == Core::Stream::CaptureSource::Kind::Monitor
                ? "Screen " + std::to_string(i + 1) : "Application";
        }

        rml += "<div class=\"golive-card" + std::string(isSelected ? " selected" : "") +
            "\" data-index=\"" + std::to_string(i) + "\" title=\"" + Platform::htmlEscape(title) +
            "\"><div class=\"golive-thumb-wrap\"><img class=\"golive-thumb-img\" src=\"frame:" +
            frameKey + "\"/><button class=\"golive-share-action\" data-share=\"" + std::to_string(i) +
            "\"><span>Share Screen</span></button></div><div class=\"golive-card-title\"><img src=\"icon:monitor\"/><span>" +
            Platform::htmlEscape(title) + "</span></div></div>";
    }

    container->SetInnerRML(rml);
}

void GoLiveController::renderQualityMenu()
{
    auto *menu = modalDoc ? modalDoc->GetElementById("golive-quality-menu") : nullptr;
    if (!menu)
        return;
    const auto quality = Core::Stream::StreamQuality::byId(selectedQualityId);
    std::string rml;
    auto row = [&rml](const std::string &value, const std::string &label, const std::string &subtitle,
                     const std::string &indicator, bool enabled = true) {
        rml += "<button class=\"golive-menu-row" + std::string(enabled ? "" : " disabled") +
            "\" data-quality-menu=\"" + value + "\"" +
            (enabled ? "" : " disabled=\"disabled\" aria-disabled=\"true\"") + "><div class=\"golive-menu-text\">" +
            Platform::htmlEscape(label);
        if (!subtitle.empty())
            rml += "<div class=\"golive-menu-description\">" + Platform::htmlEscape(subtitle) + "</div>";
        rml += "</div>" + indicator + "</button>";
    };
    auto radio = [](bool selected) {
        return std::string("<div class=\"golive-radio") + (selected ? " checked" : "") + "\"><div></div></div>";
    };
    auto check = [](bool selected) {
        return std::string("<div class=\"golive-checkbox") + (selected ? " checked" : "") + "\">" +
            (selected ? "<img src=\"icon:check\"/>" : "") + "</div>";
    };
    const std::string arrow = "<img class=\"golive-menu-arrow\" src=\"icon:chevron-right\"/>";
    if (qualityPage.empty()) {
        rml = "<div class=\"golive-menu-heading\">Stream Mode</div>";
        row("gaming", "Gaming", "Smoother video (1440p, 60fps)", radio(streamMode == "gaming"));
        row("screenshare", "Screenshare", "Clearer text (Source, 15fps)", radio(streamMode == "screenshare"));
        row("custom", "Custom", "", radio(streamMode == "custom"));
        rml += "<div class=\"golive-menu-separator\"></div>";
        row("resolution", "Screen Resolution", "", arrow);
        row("framerate", "Frame Rate", "", arrow);
        rml += "<div class=\"golive-menu-separator\"></div>";
        row("audio", "Mute stream audio", audioSupported ? "" : "Audio sharing is not supported on this system",
            check(!audioEnabled), audioSupported);
        row("advanced", "Advanced", "", arrow);
    } else {
        row("back", "Back", "", "<img class=\"golive-menu-arrow\" src=\"icon:arrow-left\"/>");
        rml += "<div class=\"golive-menu-separator\"></div>";
        if (qualityPage == "resolution") {
            rml += "<div class=\"golive-menu-heading\">Screen Resolution</div>";
            for (int height : {0, 480, 720, 1080, 1440})
                row("height:" + std::to_string(height), height ? std::to_string(height) + "p" : "Source",
                    height == 0 || height > 720 ? "Requires Nitro" : "", radio(quality.size.height == height));
        } else if (qualityPage == "framerate") {
            rml += "<div class=\"golive-menu-heading\">Frame Rate</div>";
            for (int fps : {15, 30, 60})
                row("fps:" + std::to_string(fps), std::to_string(fps) + " fps",
                    fps > 30 ? "Requires Nitro" : "", radio(quality.framerate == fps));
        } else if (qualityPage == "advanced") {
            row("cursor", "Show cursor", "Include your pointer in the stream", check(captureCursor));
        }
    }
    menu->SetInnerRML(rml);
}

void GoLiveController::updateQualitySummary()
{
    if (!modalDoc)
        return;
    const auto quality = Core::Stream::StreamQuality::byId(selectedQualityId);
    if (auto *mode = modalDoc->GetElementById("golive-quality-mode"))
        mode->SetInnerRML(streamMode == "gaming" ? "Gaming" : streamMode == "screenshare" ? "Screenshare" : "Custom");
    if (auto *details = modalDoc->GetElementById("golive-quality-detail")) {
        const std::string resolution = quality.size.height ? std::to_string(quality.size.height) + "p" : "Source";
        details->SetInnerRML(resolution + " · " + std::to_string(quality.framerate) + "fps" +
            (!audioEnabled ? " · Audio muted" : ""));
    }
}

void GoLiveController::updateSharingAvailability()
{
    auto *inst = shell.currentInstance();
    auto *streams = inst ? inst->streams() : nullptr;
    std::string reason;
    shareAllowed = streams && streams->canStream(&reason);
    if (!streams)
        reason = "Join a voice channel to share your screen.";
    if (auto *status = modalDoc->GetElementById("golive-status"))
        status->SetInnerRML(Platform::htmlEscape(reason));
    ::Rml::ElementList buttons;
    modalDoc->GetElementsByClassName(buttons, "golive-share-action");
    for (auto *button : buttons) {
        button->SetClass("disabled", !shareAllowed);
        button->SetAttribute("aria-disabled", shareAllowed ? "false" : "true");
        if (shareAllowed) button->RemoveAttribute("disabled");
        else button->SetAttribute("disabled", "disabled");
    }
}

void GoLiveController::savePreferences()
{
    auto &settings = Platform::Settings::instance();
    settings.setString("stream/quality", selectedQualityId);
    settings.setString("stream/mode", streamMode);
    settings.setBool("stream/audio", audioEnabled);
    settings.setBool("stream/capture_cursor", captureCursor);
    settings.save();
}

void GoLiveController::setQualityMenuOpen(bool open)
{
    qualityMenuOpen = open;
    qualityPage.clear();
    if (auto *menu = modalDoc ? modalDoc->GetElementById("golive-quality-menu") : nullptr) {
        menu->SetClass("open", open);
        if (open) renderQualityMenu();
    }
    if (auto *button = modalDoc ? modalDoc->GetElementById("golive-quality-button") : nullptr)
        button->SetClass("active", open);
    requestRender();
}

void GoLiveController::switchTab(Tab tab)
{
    if (currentTab == tab)
        return;

    currentTab = tab;
    if (auto *appTab = modalDoc->GetElementById("tab-applications"))
        appTab->SetClass("active", currentTab == Tab::Applications);
    if (auto *screenTab = modalDoc->GetElementById("tab-screens"))
        screenTab->SetClass("active", currentTab == Tab::Screens);

    const auto &sources = (currentTab == Tab::Screens) ? screenSources : appSources;
    selectedSourceIndex = sources.empty() ? -1 : 0;
    if (currentTab == Tab::Screens) {
        for (size_t i = 0; i < screenSources.size(); ++i) {
            if (screenSources[i].primary) {
                selectedSourceIndex = static_cast<int>(i);
                break;
            }
        }
    }

    renderSources();
    updateSharingAvailability();

    updateThumbnailSources();

    if (shell.view())
        shell.view()->requestRender(100);
}

void GoLiveController::selectSource(int index)
{
    const auto &sources = (currentTab == Tab::Screens) ? screenSources : appSources;
    if (index < 0 || static_cast<size_t>(index) >= sources.size())
        return;

    selectedSourceIndex = index;

    if (modalDoc) {
        ::Rml::ElementList cards;
        modalDoc->GetElementsByClassName(cards, "golive-card");
        for (auto *card : cards) {
            const auto text = attribute(card, "data-index");
            int cardIdx = -1;
            std::from_chars(text.data(), text.data() + text.size(), cardIdx);
            card->SetClass("selected", cardIdx == selectedSourceIndex);
        }
    }

    updateSharingAvailability();

    if (shell.view())
        shell.view()->requestRender(100);
}


void GoLiveController::goLive()
{
    const auto &sources = (currentTab == Tab::Screens) ? screenSources : appSources;
    if (selectedSourceIndex < 0 || static_cast<size_t>(selectedSourceIndex) >= sources.size())
        return;

    auto *inst = shell.currentInstance();
    if (!inst)
        return;
    auto *streamMgr = inst->streams();
    if (!streamMgr)
        return;

    std::string reason;
    if (!streamMgr->canStream(&reason))
        return;

    const auto source = sources[selectedSourceIndex];
    auto quality = Core::Stream::StreamQuality::byId(selectedQualityId);
    quality.captureCursor = captureCursor;
    savePreferences();

    close();

    streamMgr->startStreaming(source, quality, audioEnabled);
}

void GoLiveController::startThumbnailCapture()
{
    stopThumbnailCapture();

    updateThumbnailSources();
    thumbnailRunning = true;
    const Platform::Guard<GoLiveController> guard(this);
    thumbnailThread = std::thread([this, guard]() {
        while (thumbnailRunning.load()) {
            std::vector<Core::Stream::CaptureSource> currentList;
            std::uint64_t generation;
            {
                std::lock_guard<std::mutex> lock(thumbnailMutex);
                currentList = thumbnailSources;
                generation = thumbnailGeneration;
            }

            for (size_t i = 0; i < currentList.size() && thumbnailRunning.load(); ++i) {
                {
                    std::lock_guard<std::mutex> lock(thumbnailMutex);
                    if (generation != thumbnailGeneration)
                        break;
                }
                const auto &s = currentList[i];
                const std::string frameKey = "golive_thumb_" +
                    std::string(s.kind == Core::Stream::CaptureSource::Kind::Monitor ? "mon_" : "win_") +
                    std::to_string(s.handle);
                auto img = Core::Stream::captureThumbnail(s, {640, 360});
                if (!img.empty() && thumbnailRunning.load()) {
                    Platform::postToMain([guard, generation, frameKey, img = std::move(img)]() {
                        if (guard && guard->visible && guard->thumbnailGeneration == generation)
                            Runtime::instance().setFrame(frameKey, img);
                    });
                }
            }

            std::unique_lock<std::mutex> lock(thumbnailMutex);
            thumbnailCv.wait_for(lock, std::chrono::milliseconds(1500), [this, generation]() {
                return !thumbnailRunning.load() || thumbnailGeneration != generation;
            });
        }
    });
}

void GoLiveController::updateThumbnailSources()
{
    {
        std::lock_guard<std::mutex> lock(thumbnailMutex);
        thumbnailSources = currentTab == Tab::Screens ? screenSources : appSources;
        ++thumbnailGeneration;
    }
    thumbnailCv.notify_all();
}

void GoLiveController::stopThumbnailCapture()
{
    if (thumbnailRunning.exchange(false)) {
        thumbnailCv.notify_all();
        if (thumbnailThread.joinable())
            thumbnailThread.join();
    }
    ++thumbnailGeneration;
}

} // namespace Rml
} // namespace UI
} // namespace Styx
