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
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
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
    if (!modalDoc)
        return;

    listen("click", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        if (!target)
            return;
        auto *control = closest(target, "id");
        if (!control)
            control = target;
        const auto &controlId = control->GetId();

        // Close button or cancel button or clicking the backdrop outside the dialog card
        if (controlId == "golive-close-btn" ||
            controlId == "btn-cancel" ||
            target == modalDoc->GetElementById("golive-backdrop")) {
            close();
            return;
        }

        // Tab click
        if (auto *tabEl = closest(target, "data-tab")) {
            const auto tabName = attribute(tabEl, "data-tab");
            switchTab(tabName == "screens" ? Tab::Screens : Tab::Applications);
            return;
        }

        // Source card click
        if (auto *cardEl = closest(target, "data-index")) {
            const auto text = attribute(cardEl, "data-index");
            int idx = -1;
            std::from_chars(text.data(), text.data() + text.size(), idx);
            selectSource(idx);
            return;
        }

        // Audio toggle click
        if (controlId == "audio-switch" || controlId == "audio-toggle-row") {
            toggleAudio();
            return;
        }

        // Go Live click
        if (controlId == "btn-go-live") {
            if (!control->IsClassSet("disabled"))
                goLive();
            return;
        }
    }, modalDoc);

    listen("dblclick", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        if (auto *cardEl = closest(target, "data-index")) {
            const auto text = attribute(cardEl, "data-index");
            int idx = -1;
            std::from_chars(text.data(), text.data() + text.size(), idx);
            selectSource(idx);
            goLive();
        }
    }, modalDoc);

    listen("change", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        if (target && target->GetId() == "quality-select") {
            auto *sel = rmlui_dynamic_cast<::Rml::ElementFormControlSelect *>(target);
            if (sel) {
                selectedQualityId = sel->GetValue();
            }
        }
    }, modalDoc);

    listen("keydown", [this](::Rml::Event &event) {
        if (event.GetParameter("key_identifier", 0) == ::Rml::Input::KI_ESCAPE) {
            close();
            event.StopPropagation();
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

    // Load saved settings
    auto &settings = Platform::Settings::instance();
    selectedQualityId = settings.getString("stream/quality", "720p30");

    audioSupported = Core::Stream::LoopbackCapture::isSupported();
    audioEnabled = audioSupported && settings.getBool("stream/audio", true);

    // Update Tab UI
    if (auto *appTab = modalDoc->GetElementById("tab-applications"))
        appTab->SetClass("active", currentTab == Tab::Applications);
    if (auto *screenTab = modalDoc->GetElementById("tab-screens"))
        screenTab->SetClass("active", currentTab == Tab::Screens);

    renderSources();
    renderQualityPresets();
    updateAudioUi();
    updateGoLiveButton();
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
            "\" data-index=\"" + std::to_string(i) +
            "\"><div class=\"golive-thumb-wrap\"><img class=\"golive-thumb-img\" src=\"frame:" +
            frameKey + "\"/></div><div class=\"golive-card-title\">" +
            Platform::htmlEscape(title) + "</div></div>";
    }

    container->SetInnerRML(rml);
}

void GoLiveController::renderQualityPresets()
{
    if (!modalDoc)
        return;
    auto *selectEl = modalDoc->GetElementById("quality-select");
    if (!selectEl)
        return;

    std::string optionsRml;
    bool found = false;
    const auto presets = Core::Stream::StreamQuality::presets();
    for (const auto &preset : presets) {
        const bool selected = preset.id == selectedQualityId;
        if (selected)
            found = true;
        optionsRml += "<option value=\"" + Platform::htmlEscape(preset.id) + "\"" +
            (selected ? " selected" : "") + ">" + Platform::htmlEscape(preset.label()) + "</option>";
    }

    if (!found && !presets.empty()) {
        selectedQualityId = presets.front().id;
    }

    selectEl->SetInnerRML(optionsRml);
}

void GoLiveController::updateAudioUi()
{
    if (!modalDoc)
        return;
    if (auto *sw = modalDoc->GetElementById("audio-switch")) {
        sw->SetClass("checked", audioEnabled);
        sw->SetClass("disabled", !audioSupported);
    }
    if (auto *sub = modalDoc->GetElementById("audio-subtitle")) {
        if (audioSupported) {
            sub->SetInnerRML("A window shares its app's sound; a screen shares everything except this call.");
        } else {
            sub->SetInnerRML("Sharing audio needs Windows 10 version 2004 or later.");
        }
    }
}

void GoLiveController::updateGoLiveButton()
{
    if (!modalDoc)
        return;

    auto *btn = modalDoc->GetElementById("btn-go-live");
    auto *statusEl = modalDoc->GetElementById("golive-status");

    auto *inst = shell.currentInstance();
    auto *streamMgr = inst ? inst->streams() : nullptr;

    std::string reason;
    const bool canStream = streamMgr && streamMgr->canStream(&reason);
    const auto &sources = (currentTab == Tab::Screens) ? screenSources : appSources;
    const bool hasSelection = selectedSourceIndex >= 0 && static_cast<size_t>(selectedSourceIndex) < sources.size();

    const bool enabled = canStream && hasSelection;

    if (btn) {
        btn->SetClass("disabled", !enabled);
    }

    if (statusEl) {
        if (!canStream && !reason.empty()) {
            statusEl->SetInnerRML(Platform::htmlEscape(reason));
        } else {
            statusEl->SetInnerRML("");
        }
    }
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
    updateGoLiveButton();

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

    updateGoLiveButton();

    if (shell.view())
        shell.view()->requestRender(100);
}

void GoLiveController::toggleAudio()
{
    if (!audioSupported)
        return;
    audioEnabled = !audioEnabled;
    updateAudioUi();

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
    const auto quality = Core::Stream::StreamQuality::byId(selectedQualityId);

    auto &settings = Platform::Settings::instance();
    settings.setString("stream/quality", quality.id);
    settings.setBool("stream/audio", audioEnabled);
    settings.save();

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
                auto img = Core::Stream::captureThumbnail(s, {320, 180});
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
