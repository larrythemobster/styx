#include "CallController.hpp"

#include "RmlRuntime.hpp"
#include "Shell.hpp"

#include "Core/Audio/VoiceManager.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/UserManager.hpp"
#include "Discord/CdnUrls.hpp"
#include "Discord/Entities.hpp"

#ifdef STYX_HAVE_SCREENSHARE
#include "Core/Stream/StreamManager.hpp"
#endif

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>

#include <algorithm>
#include <format>
#include <string_view>

namespace Styx {
namespace UI {
namespace Rml {

using Core::Snowflake;

CallController::CallController(Shell &shell, ::Rml::Element *root, QObject *parent)
    : Controller(shell, root, parent)
{
    updateDebounceTimer.setSingleShot(true);
    updateDebounceTimer.setInterval(16);
    connect(&updateDebounceTimer, &QTimer::timeout, this, &CallController::updateCallView);

    setupConnections();

    // Event delegation on root
    listen("click", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();

        // 1. Close call
        if (closest(target, "data-action-close")) {
            onCloseClicked();
            return;
        }

        // 2. Fullscreen toggle
        if (closest(target, "data-action-fullscreen")) {
            onFullscreenClicked();
            return;
        }

        // 3. Bottom controls
        if (closest(target, "data-action-mute")) {
            onMuteClicked();
            return;
        }
        if (closest(target, "data-action-deaf")) {
            onDeafClicked();
            return;
        }
        if (closest(target, "data-action-share")) {
            onShareClicked();
            return;
        }
        if (closest(target, "data-action-disconnect")) {
            onDisconnectClicked();
            return;
        }

        // 4. Watch stream
        if (auto *watchEl = closest(target, "data-action-watch")) {
            std::string idStr = attribute(watchEl, "data-action-watch");
            if (!idStr.empty()) {
                onWatchClicked(Snowflake(idStr.toULongLong()));
                return;
            }
        }

        // 5. Stop watch stream
        if (auto *stopEl = closest(target, "data-action-stop-watch")) {
            std::string idStr = attribute(stopEl, "data-action-stop-watch");
            if (!idStr.empty()) {
                onStopWatchClicked(Snowflake(idStr.toULongLong()));
                return;
            }
        }

        // 6. Tile click to focus / unfocus
        if (auto *tileEl = closest(target, "data-user-id")) {
            std::string idStr = attribute(tileEl, "data-user-id");
            if (!idStr.empty()) {
                onTileClicked(Snowflake(idStr.toULongLong()));
                return;
            }
        }
    });

    onInstanceChanged();
    onLocationChanged();
}

CallController::~CallController()
{
    disconnectCurrentVoice();
}

void CallController::setupConnections()
{
    connect(&shell, &Shell::locationChanged, this, &CallController::onLocationChanged);
    connect(&shell, &Shell::instanceChanged, this, &CallController::onInstanceChanged);
    connect(&shell, &Shell::callOpenChanged, this, &CallController::onCallOpenChanged);
}

void CallController::onLocationChanged()
{
    updateDebounceTimer.start();
}

void CallController::onCallOpenChanged(bool open)
{
    if (open)
        updateDebounceTimer.start();
}

void CallController::onInstanceChanged()
{
    disconnectCurrentVoice();

    currentInstance = shell.currentInstance();
    if (currentInstance) {
#ifndef STYX_NO_VOICE
        currentVoiceManager = currentInstance->voice();
#endif
#ifdef STYX_HAVE_SCREENSHARE
        currentStreamManager = currentInstance->streams();
#endif
        connectCurrentVoice();
    } else {
        currentVoiceManager = nullptr;
#ifdef STYX_HAVE_SCREENSHARE
        currentStreamManager = nullptr;
#endif
    }

    focusedUserId = Snowflake::Invalid;
    updateDebounceTimer.start();
}

void CallController::connectCurrentVoice()
{
    if (currentVoiceManager) {
        connect(currentVoiceManager, &Core::Audio::VoiceManager::voiceConnected,
                this, [this]() { updateDebounceTimer.start(); });
        connect(currentVoiceManager, &Core::Audio::VoiceManager::voiceDisconnected,
                this, [this]() { updateDebounceTimer.start(); });
        connect(currentVoiceManager, &Core::Audio::VoiceManager::participantJoined,
                this, [this](Snowflake) { updateDebounceTimer.start(); });
        connect(currentVoiceManager, &Core::Audio::VoiceManager::participantLeft,
                this, [this](Snowflake uid) {
                    if (focusedUserId == uid)
                        focusedUserId = Snowflake::Invalid;
                    updateDebounceTimer.start();
                });
        connect(currentVoiceManager, &Core::Audio::VoiceManager::participantUpdated,
                this, [this](Snowflake) { updateDebounceTimer.start(); });
        connect(currentVoiceManager, &Core::Audio::VoiceManager::participantSpeakingChanged,
                this, [this](Snowflake, bool) { updateDebounceTimer.start(); });
    }

#ifdef STYX_HAVE_SCREENSHARE
    if (currentStreamManager) {
        connect(currentStreamManager, &Core::Stream::StreamManager::streamingChanged,
                this, [this](bool) { updateDebounceTimer.start(); });
        connect(currentStreamManager, &Core::Stream::StreamManager::watchStarted,
                this, [this](Snowflake userId) {
                    shell.setCallOpen(true);
                    focusedUserId = userId;
                    updateDebounceTimer.start();
                });
        connect(currentStreamManager, &Core::Stream::StreamManager::watchEnded,
                this, [this](Snowflake userId, const std::string &) {
                    Rml::Runtime::instance().clearFrame(userId.toString());
                    if (focusedUserId == userId)
                        focusedUserId = Snowflake::Invalid;
                    updateDebounceTimer.start();
                });
        connect(currentStreamManager, &Core::Stream::StreamManager::viewerFrame,
                this, [](Snowflake userId, const QImage &img, const QSize &) {
                    Rml::Runtime::instance().setFrame(userId.toString(), img);
                });
    }
#endif
}

void CallController::disconnectCurrentVoice()
{
    if (currentVoiceManager)
        QObject::disconnect(currentVoiceManager, nullptr, this, nullptr);
#ifdef STYX_HAVE_SCREENSHARE
    if (currentStreamManager)
        QObject::disconnect(currentStreamManager, nullptr, this, nullptr);
#endif
}

void CallController::onCloseClicked()
{
    shell.setCallOpen(false);
}

void CallController::onFullscreenClicked()
{
    isFullscreen = !isFullscreen;
    if (isFullscreen) {
        shell.showFullScreen();
    } else {
        shell.showNormal();
    }
    updateDebounceTimer.start();
}

void CallController::onMuteClicked()
{
    if (!currentVoiceManager)
        return;
    Snowflake selfId = currentInstance ? currentInstance->accountId() : Snowflake::Invalid;
    const auto *selfPart = currentVoiceManager->participant(selfId);
    bool currentMute = selfPart ? selfPart->selfMute : false;
    shell.setSelfMute(!currentMute);
    updateDebounceTimer.start();
}

void CallController::onDeafClicked()
{
    if (!currentVoiceManager)
        return;
    Snowflake selfId = currentInstance ? currentInstance->accountId() : Snowflake::Invalid;
    const auto *selfPart = currentVoiceManager->participant(selfId);
    bool currentDeaf = selfPart ? selfPart->selfDeaf : false;
    shell.setSelfDeaf(!currentDeaf);
    updateDebounceTimer.start();
}

void CallController::onShareClicked()
{
#ifdef STYX_HAVE_SCREENSHARE
    if (!currentStreamManager)
        return;

    if (currentStreamManager->isStreaming()) {
        currentStreamManager->stopStreaming();
        updateDebounceTimer.start();
        return;
    }

    shell.openGoLivePicker();
#endif
}

void CallController::onDisconnectClicked()
{
    shell.leaveVoice();
    shell.setCallOpen(false);
}

void CallController::onWatchClicked(Snowflake userId)
{
#ifdef STYX_HAVE_SCREENSHARE
    if (!currentStreamManager)
        return;

    currentStreamManager->watchStream(userId);
    currentStreamManager->setViewerSize(userId, Platform::Size{1280, 720});
    focusedUserId = userId;
    shell.setCallOpen(true);
    updateDebounceTimer.start();
#endif
}

void CallController::onStopWatchClicked(Snowflake userId)
{
#ifdef STYX_HAVE_SCREENSHARE
    if (!currentStreamManager)
        return;

    currentStreamManager->stopWatching(userId);
    Rml::Runtime::instance().clearFrame(userId.toString());
    if (focusedUserId == userId)
        focusedUserId = Snowflake::Invalid;
    updateDebounceTimer.start();
#endif
}

void CallController::onTileClicked(Snowflake userId)
{
    if (focusedUserId == userId) {
        focusedUserId = Snowflake::Invalid; // toggle off focus
    } else {
        focusedUserId = userId; // focus this tile
    }
    updateDebounceTimer.start();
}

void CallController::updateCallView()
{
    if (!regionRoot)
        return;

    if (!shell.isCallOpen()) {
        regionRoot->SetInnerRML("");
        requestRender();
        return;
    }

    // 1. Channel title & info
    std::string channelName = "Voice Call";
    Snowflake currentVoiceChannelId;
    if (currentVoiceManager) {
        currentVoiceChannelId = currentVoiceManager->currentChannelId();
        if (currentInstance && currentVoiceChannelId.isValid()) {
            if (auto ch = currentInstance->getChannel(currentVoiceChannelId)) {
                if (ch->name.hasValue() && !ch->name->empty())
                    channelName = ch->name.get();
                else if (ch->type.valueOr(Discord::ChannelType::DM) == Discord::ChannelType::DM)
                    channelName = "Direct Call";
            }
        }
    }

    // 2. Participants
    std::vector<Core::Audio::VoiceParticipant> participants;
    if (currentVoiceManager) {
        participants = currentVoiceManager->currentParticipants();
    }

    Snowflake selfId = currentInstance ? currentInstance->accountId() : Snowflake::Invalid;
    auto *users = currentInstance ? currentInstance->users() : nullptr;

    bool isSelfStreaming = false;
#ifdef STYX_HAVE_SCREENSHARE
    if (currentStreamManager) {
        isSelfStreaming = currentStreamManager->isStreaming();
    }
#endif

    // Helper lambda to render a single participant tile
    auto renderTile = [&](const Core::Audio::VoiceParticipant &p, bool inStrip) -> std::string {
        Snowflake uid = p.userId;
        bool isSelf = (uid == selfId);

        std::string name = users ? users->getDisplayName(uid) : uid.toString();
        if (name.empty())
            name = uid.toString();

        std::string avatarUrl;
        if (users) {
            if (auto u = users->getUser(uid)) {
                if (u->avatar.hasValue() && !u->avatar->empty())
                    avatarUrl = Discord::Cdn::userAvatar(uid, u->avatar.get(), 128);
            }
        }
        if (!avatarUrl.isValid())
            avatarUrl = Discord::Cdn::defaultUserAvatar(uid, 128);

        std::string avatarSrc = shell.imageSource(avatarUrl, inStrip ? Platform::Size{48, 48} : Platform::Size{80, 80});

        bool isSpeaking = p.speaking;
        bool isStreaming = p.streaming;
        bool isWatching = false;
#ifdef STYX_HAVE_SCREENSHARE
        if (currentStreamManager)
            isWatching = currentStreamManager->isWatching(uid);
#endif

        std::string tileClasses = "call-tile";
        if (isSpeaking)
            tileClasses += " speaking";

        std::string tileContent;

        if (isWatching) {
            // Render the live video frame
            tileContent += std::format("<img class=\"call-video-frame\" src=\"frame:{0}\"/>", uid.toString());
            tileContent += std::format(
                "<div class=\"call-stop-watch-btn\" data-action-stop-watch=\"{0}\">Stop Watching</div>"
            , uid.toString());
        } else {
            // Avatar centred on background
            tileContent += std::format(
                "<div class=\"call-tile-avatar-center\">"
                "  <img class=\"call-tile-avatar\" src=\"{0}\"/>"
                "</div>"
            , avatarSrc);

            if (isSelf && isSelfStreaming) {
                tileContent += "<div class=\"call-self-preview-text\">You are streaming</div>";
            }
        }

        // Live badge
        if (isStreaming) {
            tileContent += "<div class=\"call-badge-live\">LIVE</div>";
            if (!isWatching && !isSelf && !inStrip) {
                // Watch stream overlay button
                tileContent += std::format(
                    "<div class=\"call-stream-overlay\">"
                    "  <div class=\"call-watch-btn\" data-action-watch=\"{0}\">Watch Stream</div>"
                    "</div>"
                , uid.toString());
            }
        }

        // Name pill bottom-left
        std::string muteIcons;
        if (p.selfMute || p.serverMute) {
            muteIcons += "<img class=\"call-tile-icon\" src=\"icon:mic-off\"/>";
        }
        if (p.selfDeaf || p.serverDeaf) {
            muteIcons += "<img class=\"call-tile-icon\" src=\"icon:headphone-off\"/>";
        }

        tileContent += std::format(
            "<div class=\"call-tile-pill\">"
            "  <div class=\"call-tile-name\">{0}</div>"
            "  <div class=\"call-tile-icons\">{1}</div>"
            "</div>"
        , escape(name), muteIcons);

        return std::format(
            "<div class=\"{0}\" data-user-id=\"{1}\">{2}</div>"
        , tileClasses, uid.toString(), tileContent);
    };

    // 3. Assemble Tiles (Grid vs Focus Mode)
    std::string bodyHtml;
    bool hasFocus = focusedUserId.isValid();
    const Core::Audio::VoiceParticipant *focusedPart = nullptr;

    if (hasFocus) {
        for (const auto &p : participants) {
            if (p.userId == focusedUserId) {
                focusedPart = &p;
                break;
            }
        }
        if (!focusedPart) {
            hasFocus = false;
            focusedUserId = Snowflake::Invalid;
        }
    }

    if (hasFocus && focusedPart) {
        // Focus mode: 1 main large tile, horizontal strip at bottom
        std::string mainTile = renderTile(*focusedPart, false);
        std::string stripTiles;
        for (const auto &p : participants) {
            if (p.userId != focusedUserId) {
                stripTiles += renderTile(p, true);
            }
        }

        bodyHtml = std::format(
            "<div class=\"call-focus-view\">"
            "  <div class=\"call-focus-main\">{0}</div>"
            "  <div class=\"call-focus-strip\">{1}</div>"
            "</div>"
        , mainTile, stripTiles);
    } else {
        // Grid mode: responsive class tiles-1, tiles-2, tiles-3, etc.
        int count = participants.size();
        std::string gridClass = "call-grid";
        if (count <= 1)
            gridClass += " tiles-1";
        else if (count == 2)
            gridClass += " tiles-2";
        else if (count <= 4)
            gridClass += " tiles-4";
        else if (count <= 6)
            gridClass += " tiles-6";
        else
            gridClass += " tiles-more";

        std::string tilesHtml;
        for (const auto &p : participants) {
            tilesHtml += renderTile(p, false);
        }

        bodyHtml = std::format(
            "<div class=\"{0}\">{1}</div>"
        , gridClass, tilesHtml);
    }

    // 4. Header & Controls
    bool selfMuted = false;
    bool selfDeafened = false;
    if (currentVoiceManager && selfId.isValid()) {
        if (const auto *me = currentVoiceManager->participant(selfId)) {
            selfMuted = me->selfMute;
            selfDeafened = me->selfDeaf;
        }
    }

    std::string muteBtnClass = selfMuted ? "call-btn active" : "call-btn";
    std::string muteIcon = selfMuted ? "icon:mic-off" : "icon:mic";

    std::string deafBtnClass = selfDeafened ? "call-btn active" : "call-btn";
    std::string deafIcon = selfDeafened ? "icon:headphone-off" : "icon:headphones";

    std::string shareBtnClass = isSelfStreaming ? "call-btn active" : "call-btn";
    std::string fullscreenIcon = isFullscreen ? "icon:minimize" : "icon:maximize";

    std::string rml = std::format(
        "<div class=\"call-header\">"
        "  <div class=\"call-header-left\">"
        "    <img class=\"call-header-icon\" src=\"icon:volume-2\"/>"
        "    <div class=\"call-header-title\">{0}</div>"
        "  </div>"
        "  <div class=\"call-header-right\">"
        "    <div class=\"call-header-btn\" data-action-fullscreen=\"1\"><img src=\"{1}\"/></div>"
        "    <div class=\"call-header-btn\" data-action-close=\"1\"><img src=\"icon:x\"/></div>"
        "  </div>"
        "</div>"
        "<div class=\"call-body\">"
        "  {2}"
        "</div>"
        "<div class=\"call-controls\">"
        "  <div class=\"{3}\" data-action-mute=\"1\"><img src=\"{4}\"/></div>"
        "  <div class=\"{5}\" data-action-deaf=\"1\"><img src=\"{6}\"/></div>"
        "  <div class=\"{7}\" data-action-share=\"1\"><img src=\"icon:monitor\"/></div>"
        "  <div class=\"call-btn-disconnect\" data-action-disconnect=\"1\"><img src=\"icon:phone-off\"/></div>"
        "</div>"
    , escape(channelName), fullscreenIcon, bodyHtml, muteBtnClass, muteIcon, deafBtnClass, deafIcon, shareBtnClass);

    regionRoot->SetInnerRML(rml.c_str());
    requestRender();
}

} // namespace Rml
} // namespace UI
} // namespace Styx
