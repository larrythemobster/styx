#include "GoLiveController.hpp"

#include "Shell.hpp"
#include "RmlRuntime.hpp"
#include "RmlView.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/Stream/LoopbackCapture.hpp"
#include "Core/Stream/StreamManager.hpp"
#include "Platform/Dispatcher.hpp"

#include <QCoreApplication>
#include <QSettings>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

namespace Styx {
namespace UI {
namespace Rml {

GoLiveController::GoLiveController(Shell &shell, QObject *parent)
    : Controller(shell, nullptr, parent)
{
}

GoLiveController::~GoLiveController()
{
    stopThumbnailCapture();
    for (const QString &key : std::as_const(registeredFrames)) {
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
            QString tabName = attribute(tabEl, "data-tab");
            switchTab(tabName == QLatin1String("screens") ? Tab::Screens : Tab::Applications);
            return;
        }

        // Source card click
        if (auto *cardEl = closest(target, "data-index")) {
            int idx = attribute(cardEl, "data-index").toInt();
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
            int idx = attribute(cardEl, "data-index").toInt();
            selectSource(idx);
            goLive();
        }
    }, modalDoc);

    listen("change", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        if (target && target->GetId() == "quality-select") {
            auto *sel = rmlui_dynamic_cast<::Rml::ElementFormControlSelect *>(target);
            if (sel) {
                selectedQualityId = QString::fromStdString(sel->GetValue());
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

    for (const QString &key : std::as_const(registeredFrames)) {
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
    const qint64 ownPid = QCoreApplication::applicationPid();

    screenSources.clear();
    appSources.clear();

    for (const auto &source : sources) {
        if (source.kind == Core::Stream::CaptureSource::Kind::Window &&
            source.processId == static_cast<quint32>(ownPid)) {
            continue; // Sharing ourselves only shows recursive previews
        }
        if (source.kind == Core::Stream::CaptureSource::Kind::Monitor) {
            screenSources.append(source);
        } else {
            appSources.append(source);
        }
    }

    if (currentTab == Tab::Applications && appSources.isEmpty() && !screenSources.isEmpty()) {
        currentTab = Tab::Screens;
    }

    // Default source selection
    selectedSourceIndex = -1;
    const auto &curSources = (currentTab == Tab::Screens) ? screenSources : appSources;
    if (currentTab == Tab::Screens) {
        for (int i = 0; i < screenSources.size(); ++i) {
            if (screenSources[i].primary) {
                selectedSourceIndex = i;
                break;
            }
        }
        if (selectedSourceIndex < 0 && !screenSources.isEmpty())
            selectedSourceIndex = 0;
    } else {
        if (!appSources.isEmpty())
            selectedSourceIndex = 0;
    }

    // Load saved settings
    QSettings settings;
    selectedQualityId = settings.value(QStringLiteral("stream/quality"), QStringLiteral("720p30")).toString();

    audioSupported = Core::Stream::LoopbackCapture::isSupported();
    audioEnabled = audioSupported && settings.value(QStringLiteral("stream/audio"), true).toBool();

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
    if (sources.isEmpty()) {
        container->SetInnerRML("<div class=\"golive-empty\">No capture sources available</div>");
        return;
    }

    QString rml;
    for (int i = 0; i < sources.size(); ++i) {
        const auto &s = sources[i];
        const QString frameKey = QStringLiteral("golive_thumb_%1_%2")
                                     .arg(s.kind == Core::Stream::CaptureSource::Kind::Monitor ? "mon" : "win")
                                     .arg(s.handle);
        registeredFrames.insert(frameKey);

        const bool isSelected = (i == selectedSourceIndex);
        QString title = s.title;
        if (title.isEmpty()) {
            title = (s.kind == Core::Stream::CaptureSource::Kind::Monitor)
                        ? QStringLiteral("Screen %1").arg(i + 1)
                        : QStringLiteral("Application");
        }

        rml += QStringLiteral(
                   "<div class=\"golive-card%1\" data-index=\"%2\">"
                   "  <div class=\"golive-thumb-wrap\">"
                   "    <img class=\"golive-thumb-img\" src=\"frame:%3\"/>"
                   "  </div>"
                   "  <div class=\"golive-card-title\">%4</div>"
                   "</div>")
                   .arg(isSelected ? QStringLiteral(" selected") : QString())
                   .arg(i)
                   .arg(frameKey, escape(title));
    }

    container->SetInnerRML(rml.toUtf8().constData());
}

void GoLiveController::renderQualityPresets()
{
    if (!modalDoc)
        return;
    auto *selectEl = modalDoc->GetElementById("quality-select");
    if (!selectEl)
        return;

    QString optionsRml;
    bool found = false;
    for (const auto &preset : Core::Stream::StreamQuality::presets()) {
        const bool selected = (preset.id == selectedQualityId);
        if (selected)
            found = true;
        optionsRml += QStringLiteral("<option value=\"%1\"%2>%3</option>")
                          .arg(preset.id, selected ? QStringLiteral(" selected") : QString(), preset.label());
    }

    if (!found && !Core::Stream::StreamQuality::presets().isEmpty()) {
        selectedQualityId = Core::Stream::StreamQuality::presets().first().id;
    }

    selectEl->SetInnerRML(optionsRml.toUtf8().constData());
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

    QString reason;
    const bool canStream = streamMgr && streamMgr->canStream(&reason);
    const auto &sources = (currentTab == Tab::Screens) ? screenSources : appSources;
    const bool hasSelection = (selectedSourceIndex >= 0 && selectedSourceIndex < sources.size());

    const bool enabled = canStream && hasSelection;

    if (btn) {
        btn->SetClass("disabled", !enabled);
    }

    if (statusEl) {
        if (!canStream && !reason.isEmpty()) {
            statusEl->SetInnerRML(escape(reason).toUtf8().constData());
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
    selectedSourceIndex = sources.isEmpty() ? -1 : 0;
    if (currentTab == Tab::Screens) {
        for (int i = 0; i < screenSources.size(); ++i) {
            if (screenSources[i].primary) {
                selectedSourceIndex = i;
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
    if (index < 0 || index >= sources.size())
        return;

    selectedSourceIndex = index;

    if (modalDoc) {
        ::Rml::ElementList cards;
        modalDoc->GetElementsByClassName(cards, "golive-card");
        for (auto *card : cards) {
            int cardIdx = attribute(card, "data-index").toInt();
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
    if (selectedSourceIndex < 0 || selectedSourceIndex >= sources.size())
        return;

    auto *inst = shell.currentInstance();
    if (!inst)
        return;
    auto *streamMgr = inst->streams();
    if (!streamMgr)
        return;

    QString reason;
    if (!streamMgr->canStream(&reason))
        return;

    const auto source = sources[selectedSourceIndex];
    const auto quality = Core::Stream::StreamQuality::byId(selectedQualityId);

    QSettings settings;
    settings.setValue(QStringLiteral("stream/quality"), quality.id);
    settings.setValue(QStringLiteral("stream/audio"), audioEnabled);

    close();

    streamMgr->startStreaming(source, quality, audioEnabled);
}

void GoLiveController::startThumbnailCapture()
{
    stopThumbnailCapture();

    updateThumbnailSources();
    thumbnailRunning = true;
    const QPointer<GoLiveController> guard(this);
    thumbnailThread = std::thread([this, guard]() {
        while (thumbnailRunning.load()) {
            QList<Core::Stream::CaptureSource> currentList;
            std::uint64_t generation;
            {
                std::lock_guard<std::mutex> lock(thumbnailMutex);
                currentList = thumbnailSources;
                generation = thumbnailGeneration;
            }

            for (int i = 0; i < currentList.size() && thumbnailRunning.load(); ++i) {
                {
                    std::lock_guard<std::mutex> lock(thumbnailMutex);
                    if (generation != thumbnailGeneration)
                        break;
                }
                const auto &s = currentList[i];
                const QString frameKey = QStringLiteral("golive_thumb_%1_%2")
                                             .arg(s.kind == Core::Stream::CaptureSource::Kind::Monitor ? "mon" : "win")
                                             .arg(s.handle);
                QImage img = Core::Stream::captureThumbnail(s, QSize(320, 180));
                if (!img.isNull() && thumbnailRunning.load()) {
                    Platform::postToMain([guard, generation, frameKey, img]() {
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
