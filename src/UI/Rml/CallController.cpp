#include "CallController.hpp"

#include "RmlRuntime.hpp"
#include "Shell.hpp"

#include "Core/Audio/VoiceManager.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/UserManager.hpp"
#include "Discord/CdnUrls.hpp"
#include "Discord/Entities.hpp"

#ifdef ACHERON_HAVE_SCREENSHARE
#include "Core/Stream/StreamManager.hpp"
#include "UI/Stream/SourcePickerDialog.hpp"
#include <QMessageBox>
#endif

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>

#include <algorithm>

namespace Acheron {
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
            QString idStr = attribute(watchEl, "data-action-watch");
            if (!idStr.isEmpty()) {
                onWatchClicked(Snowflake(idStr.toULongLong()));
                return;
            }
        }

        // 5. Stop watch stream
        if (auto *stopEl = closest(target, "data-action-stop-watch")) {
            QString idStr = attribute(stopEl, "data-action-stop-watch");
            if (!idStr.isEmpty()) {
                onStopWatchClicked(Snowflake(idStr.toULongLong()));
                return;
            }
        }

        // 6. Tile click to focus / unfocus
        if (auto *tileEl = closest(target, "data-user-id")) {
            QString idStr = attribute(tileEl, "data-user-id");
            if (!idStr.isEmpty()) {
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
#ifndef ACHERON_NO_VOICE
        currentVoiceManager = currentInstance->voice();
#endif
#ifdef ACHERON_HAVE_SCREENSHARE
        currentStreamManager = currentInstance->streams();
#endif
        connectCurrentVoice();
    } else {
        currentVoiceManager = nullptr;
#ifdef ACHERON_HAVE_SCREENSHARE
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

#ifdef ACHERON_HAVE_SCREENSHARE
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
                this, [this](Snowflake userId, const QString &) {
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
#ifdef ACHERON_HAVE_SCREENSHARE
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
#ifdef ACHERON_HAVE_SCREENSHARE
    if (!currentStreamManager)
        return;

    if (currentStreamManager->isStreaming()) {
        currentStreamManager->stopStreaming();
        updateDebounceTimer.start();
        return;
    }

    QString reason;
    if (!currentStreamManager->canStream(&reason)) {
        QMessageBox::information(&shell, tr("Go Live"), reason);
        return;
    }

    SourcePickerDialog picker(&shell);
    if (picker.exec() != QDialog::Accepted)
        return;

    if (currentStreamManager && currentStreamManager->canStream(&reason)) {
        currentStreamManager->startStreaming(picker.selectedSource(), picker.selectedQuality(), picker.includeAudio());
        updateDebounceTimer.start();
    }
#endif
}

void CallController::onDisconnectClicked()
{
    shell.leaveVoice();
    shell.setCallOpen(false);
}

void CallController::onWatchClicked(Snowflake userId)
{
#ifdef ACHERON_HAVE_SCREENSHARE
    if (!currentStreamManager)
        return;

    currentStreamManager->watchStream(userId);
    currentStreamManager->setViewerSize(userId, QSize(1280, 720));
    focusedUserId = userId;
    shell.setCallOpen(true);
    updateDebounceTimer.start();
#endif
}

void CallController::onStopWatchClicked(Snowflake userId)
{
#ifdef ACHERON_HAVE_SCREENSHARE
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
    QString channelName = tr("Voice Call");
    Snowflake currentVoiceChannelId;
    if (currentVoiceManager) {
        currentVoiceChannelId = currentVoiceManager->currentChannelId();
        if (currentInstance && currentVoiceChannelId.isValid()) {
            if (auto ch = currentInstance->getChannel(currentVoiceChannelId)) {
                if (ch->name.hasValue() && !ch->name->isEmpty())
                    channelName = ch->name.get();
                else if (ch->type.valueOr(Discord::ChannelType::DM) == Discord::ChannelType::DM)
                    channelName = tr("Direct Call");
            }
        }
    }

    // 2. Participants
    QList<Core::Audio::VoiceParticipant> participants;
    if (currentVoiceManager) {
        participants = currentVoiceManager->currentParticipants();
    }

    Snowflake selfId = currentInstance ? currentInstance->accountId() : Snowflake::Invalid;
    auto *users = currentInstance ? currentInstance->users() : nullptr;

    bool isSelfStreaming = false;
#ifdef ACHERON_HAVE_SCREENSHARE
    if (currentStreamManager) {
        isSelfStreaming = currentStreamManager->isStreaming();
    }
#endif

    // Helper lambda to render a single participant tile
    auto renderTile = [&](const Core::Audio::VoiceParticipant &p, bool inStrip) -> QString {
        Snowflake uid = p.userId;
        bool isSelf = (uid == selfId);

        QString name = users ? users->getDisplayName(uid) : uid.toString();
        if (name.isEmpty())
            name = uid.toString();

        QUrl avatarUrl;
        if (users) {
            if (auto u = users->getUser(uid)) {
                if (u->avatar.hasValue() && !u->avatar->isEmpty())
                    avatarUrl = Discord::Cdn::userAvatar(uid, u->avatar.get(), 128);
            }
        }
        if (!avatarUrl.isValid())
            avatarUrl = Discord::Cdn::defaultUserAvatar(uid, 128);

        QString avatarSrc = shell.imageSource(avatarUrl, inStrip ? QSize(48, 48) : QSize(80, 80));

        bool isSpeaking = p.speaking;
        bool isStreaming = p.streaming;
        bool isWatching = false;
#ifdef ACHERON_HAVE_SCREENSHARE
        if (currentStreamManager)
            isWatching = currentStreamManager->isWatching(uid);
#endif

        QString tileClasses = QStringLiteral("call-tile");
        if (isSpeaking)
            tileClasses += QStringLiteral(" speaking");

        QString tileContent;

        if (isWatching) {
            // Render the live video frame
            tileContent += QStringLiteral("<img class=\"call-video-frame\" src=\"frame:%1\"/>").arg(uid.toString());
            tileContent += QStringLiteral(
                "<div class=\"call-stop-watch-btn\" data-action-stop-watch=\"%1\">Stop Watching</div>"
            ).arg(uid.toString());
        } else {
            // Avatar centred on background
            tileContent += QStringLiteral(
                "<div class=\"call-tile-avatar-center\">"
                "  <img class=\"call-tile-avatar\" src=\"%1\"/>"
                "</div>"
            ).arg(avatarSrc);

            if (isSelf && isSelfStreaming) {
                tileContent += QStringLiteral("<div class=\"call-self-preview-text\">You are streaming</div>");
            }
        }

        // Live badge
        if (isStreaming) {
            tileContent += QStringLiteral("<div class=\"call-badge-live\">LIVE</div>");
            if (!isWatching && !isSelf && !inStrip) {
                // Watch stream overlay button
                tileContent += QStringLiteral(
                    "<div class=\"call-stream-overlay\">"
                    "  <div class=\"call-watch-btn\" data-action-watch=\"%1\">Watch Stream</div>"
                    "</div>"
                ).arg(uid.toString());
            }
        }

        // Name pill bottom-left
        QString muteIcons;
        if (p.selfMute || p.serverMute) {
            muteIcons += QStringLiteral("<img class=\"call-tile-icon\" src=\"icon:mic-off\"/>");
        }
        if (p.selfDeaf || p.serverDeaf) {
            muteIcons += QStringLiteral("<img class=\"call-tile-icon\" src=\"icon:headphone-off\"/>");
        }

        tileContent += QStringLiteral(
            "<div class=\"call-tile-pill\">"
            "  <div class=\"call-tile-name\">%1</div>"
            "  <div class=\"call-tile-icons\">%2</div>"
            "</div>"
        ).arg(escape(name), muteIcons);

        return QStringLiteral(
            "<div class=\"%1\" data-user-id=\"%2\">%3</div>"
        ).arg(tileClasses, uid.toString(), tileContent);
    };

    // 3. Assemble Tiles (Grid vs Focus Mode)
    QString bodyHtml;
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
        QString mainTile = renderTile(*focusedPart, false);
        QString stripTiles;
        for (const auto &p : participants) {
            if (p.userId != focusedUserId) {
                stripTiles += renderTile(p, true);
            }
        }

        bodyHtml = QStringLiteral(
            "<div class=\"call-focus-view\">"
            "  <div class=\"call-focus-main\">%1</div>"
            "  <div class=\"call-focus-strip\">%2</div>"
            "</div>"
        ).arg(mainTile, stripTiles);
    } else {
        // Grid mode: responsive class tiles-1, tiles-2, tiles-3, etc.
        int count = participants.size();
        QString gridClass = QStringLiteral("call-grid");
        if (count <= 1)
            gridClass += QStringLiteral(" tiles-1");
        else if (count == 2)
            gridClass += QStringLiteral(" tiles-2");
        else if (count <= 4)
            gridClass += QStringLiteral(" tiles-4");
        else if (count <= 6)
            gridClass += QStringLiteral(" tiles-6");
        else
            gridClass += QStringLiteral(" tiles-more");

        QString tilesHtml;
        for (const auto &p : participants) {
            tilesHtml += renderTile(p, false);
        }

        bodyHtml = QStringLiteral(
            "<div class=\"%1\">%2</div>"
        ).arg(gridClass, tilesHtml);
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

    QString muteBtnClass = selfMuted ? QStringLiteral("call-btn active") : QStringLiteral("call-btn");
    QString muteIcon = selfMuted ? QStringLiteral("icon:mic-off") : QStringLiteral("icon:mic");

    QString deafBtnClass = selfDeafened ? QStringLiteral("call-btn active") : QStringLiteral("call-btn");
    QString deafIcon = selfDeafened ? QStringLiteral("icon:headphone-off") : QStringLiteral("icon:headphones");

    QString shareBtnClass = isSelfStreaming ? QStringLiteral("call-btn active") : QStringLiteral("call-btn");
    QString fullscreenIcon = isFullscreen ? QStringLiteral("icon:minimize") : QStringLiteral("icon:maximize");

    QString rml = QStringLiteral(
        "<div class=\"call-header\">"
        "  <div class=\"call-header-left\">"
        "    <img class=\"call-header-icon\" src=\"icon:volume-2\"/>"
        "    <div class=\"call-header-title\">%1</div>"
        "  </div>"
        "  <div class=\"call-header-right\">"
        "    <div class=\"call-header-btn\" data-action-fullscreen=\"1\"><img src=\"%2\"/></div>"
        "    <div class=\"call-header-btn\" data-action-close=\"1\"><img src=\"icon:x\"/></div>"
        "  </div>"
        "</div>"
        "<div class=\"call-body\">"
        "  %3"
        "</div>"
        "<div class=\"call-controls\">"
        "  <div class=\"%4\" data-action-mute=\"1\"><img src=\"%5\"/></div>"
        "  <div class=\"%6\" data-action-deaf=\"1\"><img src=\"%7\"/></div>"
        "  <div class=\"%8\" data-action-share=\"1\"><img src=\"icon:monitor\"/></div>"
        "  <div class=\"call-btn-disconnect\" data-action-disconnect=\"1\"><img src=\"icon:phone-off\"/></div>"
        "</div>"
    ).arg(escape(channelName),
         fullscreenIcon,
         bodyHtml,
         muteBtnClass,
         muteIcon,
         deafBtnClass,
         deafIcon,
         shareBtnClass);

    regionRoot->SetInnerRML(rml.toUtf8().constData());
    requestRender();
}

} // namespace Rml
} // namespace UI
} // namespace Acheron
