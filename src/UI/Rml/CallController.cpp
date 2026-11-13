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
#include <RmlUi/Core/Input.h>

#include <algorithm>
#include <format>
#include <string_view>
#include <charconv>

namespace {
Styx::Core::Snowflake parseUserId(std::string_view value)
{
    uint64_t id = 0;
    std::from_chars(value.data(), value.data() + value.size(), id);
    return id;
}
}

namespace Styx {
namespace UI {
namespace Rml {

using Core::Snowflake;

CallController::CallController(Shell &shell, ::Rml::Element *root)
    : Controller(shell, root)
{
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
                onWatchClicked(parseUserId(idStr));
                return;
            }
        }

        // 5. Stop watch stream
        if (auto *stopEl = closest(target, "data-action-stop-watch")) {
            std::string idStr = attribute(stopEl, "data-action-stop-watch");
            if (!idStr.empty()) {
                onStopWatchClicked(parseUserId(idStr));
                return;
            }
        }

        // 6. Tile click to focus / unfocus
        if (auto *tileEl = closest(target, "data-user-id")) {
            std::string idStr = attribute(tileEl, "data-user-id");
            if (!idStr.empty()) {
                onTileClicked(parseUserId(idStr));
                return;
            }
        }
    });
    listen("keydown", [](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        const int key = event.GetParameter<int>("key_identifier", 0);
        if (target && target->GetTagName() == "button"
            && (key == ::Rml::Input::KI_RETURN || key == ::Rml::Input::KI_NUMPADENTER || key == ::Rml::Input::KI_SPACE)) {
            event.StopPropagation();
            target->DispatchEvent("click", {});
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
    track(shell.locationChanged.connect([this](const Location &) { onLocationChanged(); }));
    track(shell.instanceChanged.connect([this](Core::ClientInstance *) { onInstanceChanged(); }));
    track(shell.callOpenChanged.connect([this](bool open) { onCallOpenChanged(open); }));
    track(shell.selfMuteChanged.connect([this](bool) { scheduleUpdate(); }));
    track(shell.selfDeafChanged.connect([this](bool) { scheduleUpdate(); }));
}

void CallController::scheduleUpdate()
{
    if (!updateDebounceTimer.active())
        updateDebounceTimer.start(std::chrono::milliseconds(16), false, [this] { updateCallView(); });
}

void CallController::onLocationChanged()
{
    scheduleUpdate();
}

void CallController::onCallOpenChanged(bool open)
{
    if (open)
        scheduleUpdate();
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
    scheduleUpdate();
}

void CallController::connectCurrentVoice()
{
    if (currentVoiceManager) {
        voiceConnections.push_back(currentVoiceManager->voiceConnected.connect([this] { scheduleUpdate(); }));
        voiceConnections.push_back(currentVoiceManager->voiceDisconnected.connect([this] { scheduleUpdate(); }));
        voiceConnections.push_back(currentVoiceManager->participantJoined.connect([this](Snowflake) { scheduleUpdate(); }));
        voiceConnections.push_back(currentVoiceManager->participantLeft.connect([this](Snowflake id) {
            if (focusedUserId == id) focusedUserId = Snowflake::Invalid;
            scheduleUpdate();
        }));
        voiceConnections.push_back(currentVoiceManager->participantUpdated.connect([this](Snowflake) { scheduleUpdate(); }));
        voiceConnections.push_back(currentVoiceManager->participantSpeakingChanged.connect([this](Snowflake, bool) { scheduleUpdate(); }));
    }
#ifdef STYX_HAVE_SCREENSHARE
    if (currentStreamManager) {
        voiceConnections.push_back(currentStreamManager->streamingChanged.connect([this](bool) { scheduleUpdate(); }));
        voiceConnections.push_back(currentStreamManager->watchStarted.connect([this](Snowflake id) { shell.setCallOpen(true); focusedUserId = id; scheduleUpdate(); }));
        voiceConnections.push_back(currentStreamManager->watchEnded.connect([this](Snowflake id, const std::string &) {
            Runtime::instance().clearFrame(id.toString());
            if (focusedUserId == id) focusedUserId = Snowflake::Invalid;
            scheduleUpdate();
        }));
        voiceConnections.push_back(currentStreamManager->viewerFrame.connect([](Snowflake id, const Platform::ImageBuffer &image, const Platform::Size &) {
            Runtime::instance().setFrame(id.toString(), image);
        }));
    }
#endif
}
void CallController::disconnectCurrentVoice()
{
    voiceConnections.clear();
#ifdef STYX_HAVE_SCREENSHARE
    if (currentStreamManager) for (const auto &participant : currentVoiceManager ? currentVoiceManager->currentParticipants() : std::vector<Core::Audio::VoiceParticipant>{})
        Runtime::instance().clearFrame(participant.userId.toString());
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
    scheduleUpdate();
}

void CallController::onMuteClicked()
{
    if (!currentVoiceManager)
        return;
    shell.setSelfMute(!shell.isSelfMute());
    scheduleUpdate();
}

void CallController::onDeafClicked()
{
    if (!currentVoiceManager)
        return;
    shell.setSelfDeaf(!shell.isSelfDeaf());
    scheduleUpdate();
}

void CallController::onShareClicked()
{
#ifdef STYX_HAVE_SCREENSHARE
    if (!currentStreamManager)
        return;

    if (currentStreamManager->isStreaming()) {
        currentStreamManager->stopStreaming();
        scheduleUpdate();
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
    scheduleUpdate();
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
    scheduleUpdate();
#endif
}

void CallController::onTileClicked(Snowflake userId)
{
    if (focusedUserId == userId) {
        focusedUserId = Snowflake::Invalid; // toggle off focus
    } else {
        focusedUserId = userId; // focus this tile
    }
    scheduleUpdate();
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
    if (currentStreamManager)
        isSelfStreaming = currentStreamManager->isStreaming();
#endif
    const auto voiceState = currentVoiceManager ? currentVoiceManager->clientState() : Discord::Voice::VoiceClient::State::Disconnected;
    const bool isConnected = voiceState == Discord::Voice::VoiceClient::State::Connected;
    const bool isConnecting = !isConnected && (voiceState != Discord::Voice::VoiceClient::State::Disconnected
        || (currentInstance && currentInstance->isInVoice()));
    const char *connectionLabel = isConnected ? "Connected" : isConnecting ? "Connecting" : "Disconnected";

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
        if (avatarUrl.empty())
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
        if (isSelf)
            tileClasses += " self";

        std::string tileContent;
        if (isWatching) {
            tileContent += std::format("<img class=\"call-video-frame\" src=\"frame:{0}\"/>", uid.toString());
            tileContent += std::format("<button class=\"call-stop-watch-btn\" data-action-stop-watch=\"{0}\">Stop watching</button>", uid.toString());
        } else {
            // Avatar and any caption stack in the middle of the tile; small strip tiles show only the avatar.
            tileContent += std::format("<div class=\"call-tile-center\"><div class=\"call-tile-avatar-center\"><img class=\"call-tile-avatar\" src=\"{0}\"/></div>",
                Rml::escape(avatarSrc));
            if (isSelf && isSelfStreaming && !inStrip)
                tileContent += "<div class=\"call-self-preview-text\">You are sharing your screen</div>";
            tileContent += "</div>";
        }

        if (isStreaming) {
            tileContent += "<div class=\"call-badge-live\">LIVE</div>";
            if (!isWatching && !isSelf && !inStrip)
                tileContent += std::format("<button class=\"call-watch-btn\" data-action-watch=\"{0}\">Watch stream</button>", uid.toString());
        }

        std::string statusIcons;
        if (p.selfMute || p.serverMute)
            statusIcons += std::format("<img class=\"call-tile-icon\" src=\"icon:mic-off\" title=\"{0}\"/>", p.serverMute ? "Server muted" : "Muted");
        if (p.selfDeaf || p.serverDeaf)
            statusIcons += std::format("<img class=\"call-tile-icon\" src=\"icon:headphone-off\" title=\"{0}\"/>", p.serverDeaf ? "Server deafened" : "Deafened");
        if (isSelf)
            name = "You";

        tileContent += std::format("<div class=\"call-tile-pill\"><div class=\"call-tile-name\">{0}</div><div class=\"call-tile-icons\">{1}</div></div>", escape(name), statusIcons);
        return std::format("<div class=\"{0}\" data-user-id=\"{1}\">{2}</div>", tileClasses, uid.toString(), tileContent);
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
        const int count = static_cast<int>(participants.size());
        std::string tilesHtml;
        for (const auto &p : participants)
            tilesHtml += renderTile(p, false);

        if (participants.empty()) {
            tilesHtml = std::format(
                "<div class=\"call-empty\"><div class=\"call-empty-title\">{0}</div><div class=\"call-empty-description\">{1}</div></div>",
                isConnecting ? "Joining voice…" : isConnected ? "You are the only one here" : "Not connected",
                isConnecting ? "Connecting to the voice server" : isConnected ? "Wait for someone to join" : "Close this view and join a call to reconnect");
        }

        std::string gridClass = "call-grid";
        if (count <= 1) gridClass += " tiles-1";
        else if (count == 2) gridClass += " tiles-2";
        else if (count <= 4) gridClass += " tiles-4";
        else if (count <= 6) gridClass += " tiles-6";
        else gridClass += " tiles-more";
        bodyHtml = std::format("<div class=\"{0}\">{1}</div>", gridClass, tilesHtml);
    }

    // 4. Header & Controls
    const bool selfMuted = shell.isSelfMute();
    const bool selfDeafened = shell.isSelfDeaf();

    const std::string muteBtnClass = selfMuted ? "call-btn active" : "call-btn";
    const std::string muteIcon = selfMuted ? "icon:mic-off" : "icon:mic";
    const std::string deafBtnClass = selfDeafened ? "call-btn active" : "call-btn";
    const std::string deafIcon = selfDeafened ? "icon:headphone-off" : "icon:headphones";
    // Sharing is a state, not a warning: the button lights up rather than turning red like mute and disconnect.
    const std::string shareBtnClass = isSelfStreaming ? "call-btn on" : "call-btn";
    const std::string fullscreenIcon = isFullscreen ? "icon:minimize" : "icon:maximize";

    std::string shareButton;
#ifdef STYX_HAVE_SCREENSHARE
    shareButton = std::format(
        "<button class=\"{0}\" data-action-share=\"1\" title=\"{1}\" aria-label=\"{1}\"><img src=\"icon:monitor\"/><span>{1}</span></button>",
        shareBtnClass, isSelfStreaming ? "Stop sharing" : "Share screen");
#endif

    std::string rml = std::format(
        "<div class=\"call-header\">"
        "  <div class=\"call-header-left\"><img class=\"call-header-icon\" src=\"icon:volume-2\"/><div class=\"call-header-heading\"><div class=\"call-header-title\">{0}</div><div class=\"call-header-status\">{1}</div></div></div>"
        "  <div class=\"call-header-right\">"
        "    <button class=\"call-header-btn\" data-action-fullscreen=\"1\" title=\"Toggle fullscreen\" aria-label=\"Toggle fullscreen\"><img src=\"{2}\"/></button>"
        "    <button class=\"call-header-btn\" data-action-close=\"1\" title=\"Close call view\" aria-label=\"Close call view\"><img src=\"icon:x\"/></button>"
        "  </div>"
        "</div>"
        "<div class=\"call-body\">{3}</div>"
        "<div class=\"call-controls\">"
        "  <div class=\"call-control-group\">"
        "    <button class=\"{4}\" data-action-mute=\"1\" title=\"{5}\" aria-label=\"{5}\"><img src=\"{6}\"/><span>{5}</span></button>"
        "    <button class=\"{7}\" data-action-deaf=\"1\" title=\"{8}\" aria-label=\"{8}\"><img src=\"{9}\"/><span>{8}</span></button>"
        "    {10}"
        "  </div>"
        "  <button class=\"call-btn-disconnect\" data-action-disconnect=\"1\" title=\"Disconnect from voice\" aria-label=\"Disconnect from voice\"><img src=\"icon:phone-off\"/><span>Disconnect</span></button>"
        "</div>",
        escape(channelName), connectionLabel, fullscreenIcon, bodyHtml,
        muteBtnClass, selfMuted ? "Unmute" : "Mute", muteIcon,
        deafBtnClass, selfDeafened ? "Undeafen" : "Deafen", deafIcon,
        shareButton);

    regionRoot->SetInnerRML(rml.c_str());
    requestRender();
}

} // namespace Rml
} // namespace UI
} // namespace Styx
