#include "VoiceGateway.hpp"

#include "Discord/CurlUtils.hpp"
#include "Core/Logging.hpp"

#include "Platform/WorkerLoop.hpp"
#include "Platform/Dispatcher.hpp"

#include <cstdlib>

namespace Styx {
namespace Discord {
namespace Voice {

VoiceGateway::VoiceGateway(const std::string &endpoint, Core::Snowflake serverId,
                           Core::Snowflake channelId, Core::Snowflake userId,
                           const std::string &sessionId, const std::string &token,
                           const Core::ProxyConfig &proxy, Platform::WorkerLoop *ownerLoop)
    : endpoint(endpoint),
      serverId(serverId),
      channelId(channelId),
      userId(userId),
      sessionId(sessionId),
      token(token),
      proxy(proxy),
      ownerLoop(ownerLoop)
{
}

VoiceGateway::~VoiceGateway()
{
    hardStop();
}

void VoiceGateway::dispatch(std::function<void()> task)
{
    Platform::Guard<VoiceGateway> guard(this);
    auto guarded = [guard, task = std::move(task)] { if (guard) task(); };
    if (ownerLoop)
        ownerLoop->post(std::move(guarded));
    else
        Platform::Dispatcher::main().post(std::move(guarded));
}

void VoiceGateway::start()
{
    if (running) {
        Core::log(Core::LogLevel::Warning, LogVoice) << "Attempt to start already running voice gateway";
        return;
    }

    wantToClose = false;
    running = true;
    networkThread = std::thread(&VoiceGateway::networkLoop, this);
}

void VoiceGateway::stop()
{
    closeTime = std::chrono::steady_clock::now();
    wantToClose = true;
}

void VoiceGateway::hardStop()
{
    shouldReconnect = false;
    running = false;

    heartbeatCv.notify_all();
    if (networkThread.joinable())
        networkThread.join();
    if (heartbeatThread.joinable())
        heartbeatThread.join();
}

void VoiceGateway::sendSelectProtocol(const std::string &address, int port, const std::string &mode)
{
    Codec opus;
    opus.name = "opus";
    opus.payloadType = 120;
    opus.priority = 1000;
    opus.type = "audio";

    SelectProtocolData data;
    data.address = address;
    data.port = port;
    data.mode = mode;
    data.codecs = std::vector<Codec>{ opus };

    sendPayload(VoiceOpCode::SELECT_PROTOCOL, data.toJson());
}

void VoiceGateway::sendSelectProtocolWebRtc(const std::string &sdp, const std::vector<Codec> &codecs,
                                           const std::string &rtcConnectionId)
{
    Core::OrderedJson::Array codecArray;
    for (const Codec &codec : codecs)
        codecArray.append(codec.toJson());

    Core::OrderedJson d;
    d.insert("protocol", "webrtc");
    d.insert("data", sdp);
    d.insert("sdp", sdp);
    d.insert("codecs", codecArray);
    d.insert("rtc_connection_id", rtcConnectionId);
    sendPayload(VoiceOpCode::SELECT_PROTOCOL, d);
}

void VoiceGateway::sendVideo(const Core::OrderedJson &d)
{
    sendPayload(VoiceOpCode::VIDEO, d);
}

void VoiceGateway::sendMediaSinkWants(const Core::OrderedJson &d)
{
    sendPayload(VoiceOpCode::MEDIA_SINK_WANTS, d);
}

void VoiceGateway::sendSpeaking(int flags, int delay, uint32_t ssrc)
{
    Core::OrderedJson d;
    d.insert("speaking", flags);
    d.insert("delay", delay);
    d.insert("ssrc", static_cast<int64_t>(ssrc));
    sendPayload(VoiceOpCode::SPEAKING, d);
}

void VoiceGateway::sendBinaryPayload(int opcode, const std::string &data)
{
    std::string frame;
    frame.reserve(1 + data.size());
    frame.push_back(static_cast<char>(opcode));
    frame.append(data);

    Core::log(Core::LogLevel::Debug, LogVoice) << "Voice binary >>> opcode =" << opcode << "size =" << data.size();

    CurlUtils::wsSend(curl, curlMutex, frame.data(), frame.size(), CURLWS_BINARY, "voice binary");
}

void VoiceGateway::sendDaveReadyForTransition(int transitionId)
{
    Core::OrderedJson d;
    d.insert("transition_id", transitionId);
    sendPayload(VoiceOpCode::DAVE_PROTOCOL_READY_FOR_TRANSITION, d);
}

void VoiceGateway::sendDaveInvalidCommitWelcome(int transitionId)
{
    Core::OrderedJson d;
    d.insert("transition_id", transitionId);
    sendPayload(VoiceOpCode::DAVE_MLS_INVALID_COMMIT_WELCOME, d);
}

void VoiceGateway::sendPayload(VoiceOpCode op, const Core::OrderedJson &d)
{
    Core::OrderedJson obj;
    obj.insert("op", static_cast<int>(op));
    obj.insert("d", d);

    std::string json = obj.toBytes();
    Core::log(Core::LogLevel::Debug, LogVoice) << "Voice >>>" << json;
    sendPayload(json);
}

void VoiceGateway::sendPayload(const std::string &data)
{
    CurlUtils::wsSend(curl, curlMutex, data.data(), data.size(), CURLWS_TEXT, "voice");
}

void VoiceGateway::onPayloadReceived(const nlohmann::json &root)
{
    Core::log(Core::LogLevel::Debug, LogVoice) << "Voice <<<" << root.dump();

    if (root.contains("seq") && root["seq"].is_number_integer())
        lastReceivedSeq = root["seq"].get<int>();

    int op = root.value("op", -1);
    const auto d = root.value("d", nlohmann::json{});

    switch (static_cast<VoiceOpCode>(op)) {
    case VoiceOpCode::HELLO:
        handleHello(d);
        break;
    case VoiceOpCode::READY:
        handleReady(d);
        break;
    case VoiceOpCode::SESSION_DESCRIPTION:
        handleSessionDescription(d);
        break;
    case VoiceOpCode::SPEAKING:
        handleSpeaking(d);
        break;
    case VoiceOpCode::HEARTBEAT_ACK:
        if (const auto ack = HeartbeatAck::fromJson(d); ack.nonce)
            handleHeartbeatAck(*ack.nonce);
        else
            Core::log(Core::LogLevel::Warning, LogVoice) << "Invalid voice heartbeat acknowledgement";
        break;
    case VoiceOpCode::RESUMED:
        handleResumed();
        break;
    case VoiceOpCode::CLIENT_CONNECT:
        handleClientsConnected(d);
        break;
    case VoiceOpCode::SESSION_UPDATE:
        handleClientConnect(d);
        break;
    case VoiceOpCode::CLIENT_DISCONNECT:
        handleClientDisconnect(d);
        break;
    case VoiceOpCode::DAVE_PROTOCOL_PREPARE_TRANSITION:
        handleDavePrepareTransition(d);
        break;
    case VoiceOpCode::DAVE_PROTOCOL_EXECUTE_TRANSITION:
        handleDaveExecuteTransition(d);
        break;
    case VoiceOpCode::DAVE_PROTOCOL_PREPARE_EPOCH:
        handleDavePrepareEpoch(d);
        break;
    case VoiceOpCode::VIDEO:
        videoReceived.publish(VideoData::fromJson(d));
        break;
    default:
        Core::log(Core::LogLevel::Debug, LogVoice) << "Unhandled voice opcode:" << op;
        break;
    }
}

void VoiceGateway::handleHello(const nlohmann::json &data)
{
    VoiceHello hello = VoiceHello::fromJson(data);

    Core::log(Core::LogLevel::Info, LogVoice) << "Voice Hello, heartbeat interval:" << hello.heartbeatInterval.get() << "ms";

    heartbeatInterval = hello.heartbeatInterval.get();
    heartbeatAckReceived = true;

    if (isResuming && canResume)
        resume();
    else
        identify();

    reconnectAttempts = 0;
    isResuming = false;

    if (!heartbeatThread.joinable())
        heartbeatThread = std::thread(&VoiceGateway::heartbeatLoop, this);

    helloReceived.publish(hello.heartbeatInterval.get());
}

void VoiceGateway::handleReady(const nlohmann::json &data)
{
    VoiceReady ready = VoiceReady::fromJson(data);
    canResume = true;
    readyReceived.publish(ready);
}

void VoiceGateway::handleSessionDescription(const nlohmann::json &data)
{
    SessionDescription desc = SessionDescription::fromJson(data);
    sessionDescriptionReceived.publish(desc);
}

void VoiceGateway::handleSpeaking(const nlohmann::json &data)
{
    SpeakingData speaking = SpeakingData::fromJson(data);

    Core::log(Core::LogLevel::Debug, LogVoice) << "Speaking: user =" << speaking.userId.get()
                      << "ssrc =" << speaking.ssrc.get()
                      << "flags =" << speaking.speaking.get();

    speakingReceived.publish(speaking);
}

void VoiceGateway::handleHeartbeatAck(uint64_t nonce)
{
    heartbeatAckReceived = true;

    Core::log(Core::LogLevel::Debug, LogVoice) << "Voice heartbeat ACK, nonce:" << nonce;
}

void VoiceGateway::handleResumed()
{
    Core::log(Core::LogLevel::Info, LogVoice) << "Voice session resumed";
    canResume = true;
    resumed.publish();
}

void VoiceGateway::handleClientConnect(const nlohmann::json &data)
{
    ClientConnectData client = ClientConnectData::fromJson(data);

    Core::log(Core::LogLevel::Info, LogVoice) << "Client connected: user =" << client.userId.get()
                     << "audio_ssrc =" << client.audioSsrc.get()
                     << "video_ssrc =" << client.videoSsrc.get();

    clientConnected.publish(client);
}

void VoiceGateway::handleClientDisconnect(const nlohmann::json &data)
{
    Core::Snowflake userId = Core::Snowflake(std::strtoull(data.value("user_id", std::string{}).c_str(), nullptr, 10));

    Core::log(Core::LogLevel::Info, LogVoice) << "Client disconnected: user =" << userId;

    clientDisconnected.publish(userId);
}

void VoiceGateway::handleClientsConnected(const nlohmann::json &data)
{
    std::vector<std::string> userIds;
    if (auto it = data.find("user_ids"); it != data.end() && it->is_array())
        for (const auto &val : *it)
            if (val.is_string()) userIds.push_back(val.get<std::string>());

    Core::log(Core::LogLevel::Info, LogVoice) << "Clients connected:" << userIds.size();

    clientsConnected.publish(userIds);
}

void VoiceGateway::handleDavePrepareTransition(const nlohmann::json &data)
{
    daveTransitionPrepare.publish(data.value("protocol_version", 0), data.value("transition_id", 0));
}

void VoiceGateway::handleDaveExecuteTransition(const nlohmann::json &data)
{
    daveTransitionExecute.publish(data.value("transition_id", 0));
}

void VoiceGateway::handleDavePrepareEpoch(const nlohmann::json &data)
{
    daveEpochPrepare.publish(data.value("protocol_version", 0), data.value("epoch", 0));
}

void VoiceGateway::identify()
{
    Core::log(Core::LogLevel::Info, LogVoice) << "Sending voice Identify for server" << serverId;

    VoiceIdentifyData id;
    if (channelId.isValid())
        id.channelId = channelId;
    id.serverId = serverId;
    id.userId = userId;
    id.sessionId = sessionId;
    id.token = token;
    if (videoEnabled)
        id.video = true;
    sendPayload(VoiceOpCode::IDENTIFY, id.toJson());
}

void VoiceGateway::resume()
{
    Core::log(Core::LogLevel::Info, LogVoice) << "Sending voice Resume";

    VoiceResumeData data;
    data.token = token;
    data.sessionId = sessionId;
    data.serverId = serverId;
    if (channelId.isValid())
        data.channelId = channelId;
    data.seqAck = lastReceivedSeq.load();
    sendPayload(VoiceOpCode::RESUME, data.toJson());
}

bool VoiceGateway::isFatalCloseCode(VoiceCloseCode code) const
{
    switch (code) {
    case VoiceCloseCode::AUTHENTICATION_FAILED:
    case VoiceCloseCode::DISCONNECTED:
    case VoiceCloseCode::RATE_LIMITED:
    case VoiceCloseCode::DISCONNECTED_ALL:
        return true;
    default:
        return false;
    }
}

void VoiceGateway::networkLoop()
{
    VoiceCloseCode lastCloseCode = VoiceCloseCode::INTERNAL;
    std::string lastCloseReason;
    bool terminalEmitted = false;

    do {
        shouldReconnect = false;

        std::string connectUrl = "wss://" + endpoint + "/?v=" + std::to_string(VOICE_GATEWAY_VERSION);

        curl = curl_easy_init();
        if (!curl) {
            Core::log(Core::LogLevel::Error, LogVoice) << "Failed to initialize curl for voice gateway";
            running = false;
            dispatch([this] { disconnected.publish(VoiceCloseCode::INTERNAL, "Failed to initialize curl for voice gateway"); });
            return;
        }

        curl_easy_setopt(curl, CURLOPT_URL, connectUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
        CurlUtils::applyCommonOptions(curl);
        CurlUtils::applyProxy(curl, proxy);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            Core::log(Core::LogLevel::Warning, LogVoice) << "Failed to connect to voice gateway:"
                                << curl_easy_strerror(res);

            if (isResuming && reconnectAttempts < maxReconnectAttempts) {
                std::lock_guard lock(curlMutex);
                curl_easy_cleanup(curl);
                curl = nullptr;

                reconnectAttempts++;
                int delay = 1000 + (std::rand() % 4000);
                Core::log(Core::LogLevel::Info, LogVoice) << "Voice reconnect attempt" << reconnectAttempts.load()
                                 << "in" << delay << "ms";
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                shouldReconnect = true;
                continue;
            }

            {
                std::lock_guard lock(curlMutex);
                curl_easy_cleanup(curl);
                curl = nullptr;
            }
            running = false;
            dispatch([this, reason = std::string("Failed to connect to voice gateway: ") + curl_easy_strerror(res)] { disconnected.publish(VoiceCloseCode::INTERNAL, reason); });
            return;
        }

        Core::log(Core::LogLevel::Info, LogVoice) << "Voice WebSocket connected to" << endpoint;
        generation++;
        dispatch([this] { connected.publish(); });

        char chunk[8192];
        size_t rlen = 0;
        const curl_ws_frame *meta = nullptr;
        std::string frameBuffer;
        std::string binaryFrameBuffer;

        bool closeSent = false;
        while (running) {
            if (shouldReconnect)
                break;

            {
                std::lock_guard lock(curlMutex);

                if (wantToClose) {
                    if (!closeSent) {
                        closeSent = true;
                        uint8_t close_payload[2] = { 0x03, 0xE8 };
                        size_t bytesSent = 0;
                        curl_ws_send(curl, close_payload, sizeof(close_payload),
                                     &bytesSent, 0, CURLWS_CLOSE);
                    }

                    auto now = std::chrono::steady_clock::now();
                    if (now - closeTime > closeTimeout) {
                        running = false;
                        Core::log(Core::LogLevel::Debug, LogVoice) << "Voice gateway close timeout";
                        break;
                    }
                }

                res = curl_ws_recv(curl, chunk, sizeof(chunk), &rlen, &meta);
            }

            if (res == CURLE_AGAIN || res == CURLE_GOT_NOTHING || !meta) {
                CurlUtils::wsRecvWait(curl, curlMutex);

                if (shouldReconnect)
                    break;

                continue;
            }

            if (meta->flags & CURLWS_CLOSE) {
                int closeCode = 1000;
                std::string closeReason;

                if (rlen >= 2) {
                    closeCode = (uint8_t(chunk[0]) << 8) | uint8_t(chunk[1]);
                    if (rlen > 2)
                        closeReason = std::string(chunk + 2, rlen - 2);
                }

                Core::log(Core::LogLevel::Info, LogVoice) << "Voice connection closed with code" << closeCode
                                 << "reason:" << closeReason;

                VoiceCloseCode cc = static_cast<VoiceCloseCode>(closeCode);

                if (!wantToClose && !isFatalCloseCode(cc) && canResume) {
                    lastCloseCode = cc;
                    lastCloseReason = closeReason;
                    shouldReconnect = true;
                } else {
                    terminalEmitted = true;
                    dispatch([this, cc, closeReason] { disconnected.publish(cc, closeReason); });
                }
                break;
            }

            if (meta->flags & (CURLWS_PING | CURLWS_PONG))
                continue;

            if (meta->flags & CURLWS_BINARY) {
                binaryFrameBuffer.append(chunk, rlen);

                if (meta->bytesleft == 0) {
                    if (binaryFrameBuffer.size() >= 3) {
                        const auto *raw = reinterpret_cast<const uint8_t *>(binaryFrameBuffer.data());
                        // uint16_t seq = (raw[0] << 8) | raw[1];
                        int opcode = raw[2];
                        std::string payload = binaryFrameBuffer.substr(3);

                        Core::log(Core::LogLevel::Debug, LogVoice) << "Voice binary frame: opcode =" << opcode
                                          << "payload size =" << payload.size();
                        dispatch([this, opcode, payload = std::move(payload)] { binaryPayloadReceived.publish(opcode, payload); });
                    }
                    binaryFrameBuffer.clear();
                }
                continue;
            }

            frameBuffer.append(chunk, rlen);

            if (meta->bytesleft == 0) {
                auto doc = nlohmann::json::parse(frameBuffer, nullptr, false);
                frameBuffer.clear();
                if (doc.is_discarded()) {
                    Core::log(Core::LogLevel::Warning, LogVoice) << "Failed to parse voice payload";
                } else if (doc.is_object()) {
                    dispatch([this, doc = std::move(doc)] {
                        Platform::Guard<VoiceGateway> guard(this);
                        payloadReceived.publish(doc);
                        if (guard) onPayloadReceived(doc);
                    });
                }
            }
        }

        {
            std::lock_guard lock(curlMutex);
            curl_easy_cleanup(curl);
            curl = nullptr;
        }

        if (shouldReconnect && running && reconnectAttempts < maxReconnectAttempts) {
            reconnectAttempts++;
            isResuming = canResume.load();

            if (heartbeatThread.joinable()) {
                heartbeatCv.notify_all();
                heartbeatThread.join();
            }

            int delay = 1000 + (std::rand() % 4000);
            Core::log(Core::LogLevel::Info, LogVoice) << "Voice reconnecting in" << delay << "ms (attempt"
                             << reconnectAttempts.load() << ")";
            dispatch([this, attempt = reconnectAttempts.load()] { reconnecting.publish(attempt, maxReconnectAttempts); });
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }

    } while (shouldReconnect && running && reconnectAttempts < maxReconnectAttempts);

    if (running && !wantToClose && !terminalEmitted)
        dispatch([this, lastCloseCode, lastCloseReason] { disconnected.publish(lastCloseCode, lastCloseReason); });

    running = false;
    heartbeatCv.notify_all();
    if (heartbeatThread.joinable())
        heartbeatThread.join();
}

void VoiceGateway::heartbeatLoop()
{
    Core::log(Core::LogLevel::Debug, LogVoice) << "Voice heartbeat loop started, interval:" << heartbeatInterval.load();

    // wait before first heartbeat
    {
        std::unique_lock lock(heartbeatMutex);
        bool stop = heartbeatCv.wait_for(lock, std::chrono::milliseconds(heartbeatInterval.load()),
                                         [this] { return !running || shouldReconnect.load(); });
        if (stop)
            return;
    }

    while (running) {
        if (!heartbeatAckReceived) {
            Core::log(Core::LogLevel::Warning, LogVoice) << "No voice heartbeat ACK — zombie connection";
            shouldReconnect = true;
            break;
        }
        heartbeatAckReceived = false;

        uint64_t nonce = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        lastSentNonce = nonce;

        Core::OrderedJson d;
        d.insert("t", static_cast<int64_t>(nonce));
        d.insert("seq_ack", lastReceivedSeq.load());
        sendPayload(VoiceOpCode::HEARTBEAT, d);

        {
            std::unique_lock lock(heartbeatMutex);
            bool stop = heartbeatCv.wait_for(lock, std::chrono::milliseconds(heartbeatInterval.load()),
                                             [this] { return !running || shouldReconnect.load(); });
            if (stop)
                break;
        }
    }
}

} // namespace Voice
} // namespace Discord
} // namespace Styx
