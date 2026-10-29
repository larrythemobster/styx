#include "StreamTransport.hpp"

#include "Core/Logging.hpp"

#include <rtc/rtc.hpp>

#include "Platform/Dispatcher.hpp"
#include "Platform/WorkerLoop.hpp"
#include "Platform/Utf8.hpp"
#include <sstream>

#include <cstring>
#include <deque>
#include <unordered_map>

namespace Styx {
namespace Discord {
namespace Stream {

namespace {

// Consumes every incoming RTP packet before libdatachannel dispatches by SSRC. A viewer
// does not know the streamer's SSRCs when the session is negotiated (they arrive later in
// op 12), and libdatachannel drops packets for SSRCs absent from the description.
class RtpTap final : public rtc::MediaHandler
{
public:
    using Callback = std::function<void(const rtc::message_ptr &)>;
    explicit RtpTap(Callback callback) : callback(std::move(callback)) { }

    void incoming(rtc::message_vector &messages, const rtc::message_callback &) override
    {
        rtc::message_vector rest;
        rest.reserve(messages.size());
        for (auto &message : messages) {
            if (message->type == rtc::Message::Binary)
                callback(message);
            else
                rest.push_back(std::move(message));
        }
        messages.swap(rest);
    }

private:
    Callback callback;
};

// Answers NACKs with RFC 4588 retransmissions on the RTX SSRC. libdatachannel's own
// RtcpNackResponder resends the original packet, which carries an SRTP index the media
// server has already seen, so its replay protection drops it.
class RtxNackResponder final : public rtc::MediaHandler
{
public:
    RtxNackResponder(uint32_t mediaSsrc, uint32_t rtxSsrc, uint8_t rtxPayloadType)
        : mediaSsrc(mediaSsrc), rtxSsrc(rtxSsrc), rtxPayloadType(rtxPayloadType)
    {
    }

    void outgoing(rtc::message_vector &messages, const rtc::message_callback &) override
    {
        std::lock_guard lock(mutex);
        for (const auto &message : messages) {
            if (message->type == rtc::Message::Control || message->size() < sizeof(rtc::RtpHeader))
                continue;
            auto *header = reinterpret_cast<const rtc::RtpHeader *>(message->data());
            if (header->ssrc() != mediaSsrc)
                continue;
            history[header->seqNumber()] = message;
            order.push_back(header->seqNumber());
            if (order.size() > HISTORY) {
                history.erase(order.front());
                order.pop_front();
            }
        }
    }

    void incoming(rtc::message_vector &messages, const rtc::message_callback &send) override
    {
        for (const auto &message : messages) {
            if (message->type != rtc::Message::Control)
                continue;
            size_t offset = 0;
            while (offset + sizeof(rtc::RtcpHeader) <= message->size()) {
                auto *header = reinterpret_cast<const rtc::RtcpHeader *>(message->data() + offset);
                const size_t length = header->lengthInBytes();
                if (length == 0 || offset + length > message->size())
                    break;
                // RTPFB (205), FMT 1: generic NACK
                if (header->payloadType() == 205 && header->reportCount() == 1 && length >= 16) {
                    auto *nack = reinterpret_cast<const rtc::RtcpNack *>(header);
                    if (nack->header.mediaSourceSSRC() == mediaSsrc) {
                        const unsigned int fields = static_cast<unsigned int>((length - 12) / 4);
                        for (unsigned int i = 0; i < fields; ++i) {
                            rtc::RtcpNackPart part = nack->parts[i];
                            for (uint16_t seq : part.getSequenceNumbers())
                                retransmit(seq, send);
                        }
                    }
                }
                offset += length;
            }
        }
    }

private:
    void retransmit(uint16_t seq, const rtc::message_callback &send)
    {
        rtc::message_ptr original;
        uint16_t rtxSeq = 0;
        {
            std::lock_guard lock(mutex);
            auto it = history.find(seq);
            if (it == history.end())
                return;
            original = it->second;
            rtxSeq = nextRtxSequence++;
        }
        auto *header = reinterpret_cast<const rtc::RtpHeader *>(original->data());
        const size_t headerSize = header->getSize() + header->getExtensionHeaderSize();
        if (original->size() < headerSize)
            return;

        auto rtx = rtc::make_message(original->size() + 2, rtc::Message::Binary);
        std::memcpy(rtx->data(), original->data(), headerSize);
        auto *rtxHeader = reinterpret_cast<rtc::RtpHeader *>(rtx->data());
        rtxHeader->setPayloadType(rtxPayloadType);
        rtxHeader->setSeqNumber(rtxSeq);
        rtxHeader->setSsrc(rtxSsrc);
        rtx->data()[headerSize] = std::byte(seq >> 8);
        rtx->data()[headerSize + 1] = std::byte(seq & 0xFF);
        std::memcpy(rtx->data() + headerSize + 2, original->data() + headerSize, original->size() - headerSize);
        send(rtx);
    }

    static constexpr size_t HISTORY = 1024; // a 1080p keyframe is ~150 packets
    const uint32_t mediaSsrc;
    const uint32_t rtxSsrc;
    const uint8_t rtxPayloadType;
    std::mutex mutex;
    std::unordered_map<uint16_t, rtc::message_ptr> history;
    std::deque<uint16_t> order;
    uint16_t nextRtxSequence = 0;
};

std::string sdpLine(const std::string &sdp, std::string_view prefix)
{
    std::istringstream input(sdp);
    std::string line;
    while (std::getline(input, line)) {
        if (line.starts_with(prefix))
            return Platform::trim(line);
    }
    return {};
}

} // namespace

StreamTransport::StreamTransport(Role role, Platform::WorkerLoop *ownerLoop)
    : role(role), ownerLoop(ownerLoop), guard(std::make_shared<Guard>())
{
}

StreamTransport::~StreamTransport()
{
    close();
}

std::vector<Voice::Codec> StreamTransport::codecs()
{
    Voice::Codec opus;
    opus.name = "opus";
    opus.type = "audio";
    opus.priority = 1000;
    opus.payloadType = OPUS_PAYLOAD_TYPE;

    Voice::Codec h264;
    h264.name = "H264";
    h264.type = "video";
    h264.priority = 1000;
    h264.payloadType = H264_PAYLOAD_TYPE;
    h264.rtxPayloadType = H264_RTX_PAYLOAD_TYPE;
    h264.encode = true;
    h264.decode = true;

    return { opus, h264 };
}

void StreamTransport::setLocalSsrcs(uint32_t audio, uint32_t video, uint32_t rtx)
{
    audioSsrc = audio;
    videoSsrc = video;
    rtxSsrc = rtx;
}

void StreamTransport::postToThread(std::function<void()> fn)
{
    auto weak = lifetime();
    auto task = [weak, fn = std::move(fn)] { if (!weak.expired()) fn(); };
    if (ownerLoop)
        ownerLoop->post(std::move(task));
    else
        Platform::Dispatcher::main().post(std::move(task));
}

void StreamTransport::createOffer()
{
    rtc::Configuration config;
    config.iceServers.emplace_back("stun:stun.l.google.com:19302");
    config.disableAutoNegotiation = true;
    pc = std::make_shared<rtc::PeerConnection>(config);

    const std::string cname = "styx";

    rtc::Description::Audio audio("0", rtc::Description::Direction::SendRecv);
    audio.addOpusCodec(OPUS_PAYLOAD_TYPE);
    if (audioSsrc != 0)
        audio.addSSRC(audioSsrc, cname, "styx-stream", "audio");

    rtc::Description::Video video("1", rtc::Description::Direction::SendRecv);
    video.addH264Codec(H264_PAYLOAD_TYPE);
    video.addRtxCodec(H264_RTX_PAYLOAD_TYPE, H264_PAYLOAD_TYPE, VIDEO_CLOCK_RATE);
    if (videoSsrc != 0) {
        video.addSSRC(videoSsrc, cname, "styx-stream", "video");
        if (rtxSsrc != 0) {
            video.addSSRC(rtxSsrc, cname, "styx-stream", "video");
            video.addAttribute("ssrc-group:FID " + std::to_string(videoSsrc) + " " + std::to_string(rtxSsrc));
        }
    }

    audioTrack = pc->addTrack(audio);
    videoTrack = pc->addTrack(video);

    auto g = guard;

    if (role == Role::Sender) {
        setupSenderChain();
    } else {
        pc->setMediaHandler(std::make_shared<RtpTap>([this, g](const rtc::message_ptr &message) {
            std::lock_guard lock(g->mutex);
            if (!g->alive)
                return;
            std::string packet(reinterpret_cast<const char *>(message->data()), static_cast<size_t>(message->size()));
            postToThread([this, packet = std::move(packet)] { rtpReceived.publish(packet); });
        }));
    }

    pc->onLocalDescription([this, g](rtc::Description description) {
        std::lock_guard lock(g->mutex);
        if (!g->alive)
            return;
        std::string sdp(description);
        postToThread([this, sdp] { localOfferReady.publish(sdp); });
    });

    pc->onStateChange([this, g](rtc::PeerConnection::State state) {
        std::lock_guard lock(g->mutex);
        if (!g->alive)
            return;
        Core::log(Core::LogLevel::Info, LogStream) << "Stream transport state:" << static_cast<int>(state);
        switch (state) {
        case rtc::PeerConnection::State::Connected:
            connectedFlag = true;
            postToThread([this] { connected.publish(); });
            break;
        case rtc::PeerConnection::State::Failed:
        case rtc::PeerConnection::State::Closed:
        case rtc::PeerConnection::State::Disconnected: {
            bool was = connectedFlag.exchange(false);
            std::string reason = state == rtc::PeerConnection::State::Failed ? "ICE/DTLS failed" : "transport closed";
            if (was || state == rtc::PeerConnection::State::Failed)
                postToThread([this, reason] { failed.publish(reason); });
            break;
        }
        default:
            break;
        }
    });

    pc->setLocalDescription(rtc::Description::Type::Offer);
}

void StreamTransport::setupSenderChain()
{
    const std::string cname = "styx";
    auto g = guard;

    auto audioConfig = std::make_shared<rtc::RtpPacketizationConfig>(audioSsrc, cname, OPUS_PAYLOAD_TYPE, OPUS_CLOCK_RATE);
    audioConfig->playoutDelayId = 5;
    audioConfig->playoutDelayMin = 0;
    audioConfig->playoutDelayMax = 1;
    auto audioPacketizer = std::make_shared<rtc::OpusRtpPacketizer>(audioConfig);
    audioPacketizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(audioConfig));
    audioTrack->setMediaHandler(audioPacketizer);

    auto videoConfig = std::make_shared<rtc::RtpPacketizationConfig>(videoSsrc, cname, H264_PAYLOAD_TYPE, VIDEO_CLOCK_RATE);
    // playout-delay extension, in 10 ms units: ask viewers to render as soon as possible
    videoConfig->playoutDelayId = 5;
    videoConfig->playoutDelayMin = 0;
    videoConfig->playoutDelayMax = 10;
    auto videoPacketizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, videoConfig);
    videoPacketizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(videoConfig));
    videoPacketizer->addToChain(std::make_shared<RtxNackResponder>(videoSsrc, rtxSsrc, H264_RTX_PAYLOAD_TYPE));
    videoPacketizer->addToChain(std::make_shared<rtc::PliHandler>([this, g] {
        std::lock_guard lock(g->mutex);
        if (g->alive)
            postToThread([this] { keyframeRequested.publish(); });
    }));
    videoPacketizer->addToChain(std::make_shared<rtc::RembHandler>([this, g](unsigned int bitrate) {
        std::lock_guard lock(g->mutex);
        if (g->alive)
            postToThread([this, bitrate] { bandwidthEstimate.publish(bitrate); });
    }));
    // Spread keyframe bursts (100+ KB) over 5 ms slots at a few times the stream bitrate
    // instead of dumping them on the socket at once.
    const double pacingRate = std::max(8'000'000.0, 3.0 * maxBitrate);
    videoPacketizer->addToChain(std::make_shared<rtc::PacingHandler>(pacingRate, std::chrono::milliseconds(5)));
    videoTrack->setMediaHandler(videoPacketizer);
}

bool StreamTransport::applyServerSdp(const std::string &serverSdp)
{
    if (!pc)
        return false;

    const std::string connection = sdpLine(serverSdp, "c=");
    const std::string rtcp = sdpLine(serverSdp, "a=rtcp:");
    const std::string ufrag = sdpLine(serverSdp, "a=ice-ufrag");
    const std::string pwd = sdpLine(serverSdp, "a=ice-pwd");
    const std::string fingerprint = sdpLine(serverSdp, "a=fingerprint");
    const std::string candidate = sdpLine(serverSdp, "a=candidate");
    const auto colon = rtcp.find(':');
    const auto space = rtcp.find(' ', colon == std::string::npos ? 0 : colon + 1);
    const std::string port = colon == std::string::npos ? "" : rtcp.substr(colon + 1, space - colon - 1);
    if (connection.empty() || port.empty() || ufrag.empty() || pwd.empty() || fingerprint.empty() || candidate.empty()) {
        Core::log(Core::LogLevel::Warning, LogStream) << "Server SDP is missing fields:" << serverSdp;
        return false;
    }
    const std::string transport = ufrag + "\r\n" + pwd + "\r\n" + fingerprint + "\r\n" + candidate + "\r\na=rtcp-mux\r\na=ice-lite\r\n";
    std::ostringstream sdp;
    sdp << "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0 1\r\n"
        << "m=audio " << port << " UDP/TLS/RTP/SAVPF " << OPUS_PAYLOAD_TYPE << "\r\n" << connection << "\r\n"
        << "a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level\r\n"
        << "a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n"
        << "a=setup:passive\r\na=mid:0\r\na=maxptime:60\r\na=inactive\r\n" << transport
        << "a=rtpmap:" << OPUS_PAYLOAD_TYPE << " opus/48000/2\r\na=fmtp:" << OPUS_PAYLOAD_TYPE
        << " minptime=10;useinbandfec=1;usedtx=1\r\na=rtcp-fb:" << OPUS_PAYLOAD_TYPE
        << " transport-cc\r\na=rtcp-fb:" << OPUS_PAYLOAD_TYPE << " nack\r\n";
    if (audioSsrc) sdp << "a=ssrc:" << audioSsrc << " cname:styx\r\n";
    sdp << "m=video " << port << " UDP/TLS/RTP/SAVPF " << H264_PAYLOAD_TYPE << ' ' << H264_RTX_PAYLOAD_TYPE << "\r\n" << connection << "\r\n"
        << "a=extmap:2 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time\r\n"
        << "a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n"
        << "a=extmap:14 urn:ietf:params:rtp-hdrext:toffset\r\na=extmap:13 urn:3gpp:video-orientation\r\n"
        << "a=extmap:5 http://www.webrtc.org/experiments/rtp-hdrext/playout-delay\r\n"
        << "a=setup:passive\r\na=mid:1\r\na=inactive\r\n" << transport
        << "a=rtpmap:" << H264_PAYLOAD_TYPE << " H264/90000\r\na=fmtp:" << H264_PAYLOAD_TYPE
        << " level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\na=rtpmap:" << H264_RTX_PAYLOAD_TYPE
        << " rtx/90000\r\na=fmtp:" << H264_RTX_PAYLOAD_TYPE << " apt=" << H264_PAYLOAD_TYPE << "\r\n";
    for (const char *feedback : { "ccm fir", "nack", "nack pli", "goog-remb", "transport-cc" })
        sdp << "a=rtcp-fb:" << H264_PAYLOAD_TYPE << ' ' << feedback << "\r\n";
    if (videoSsrc) sdp << "a=ssrc:" << videoSsrc << " cname:styx\r\n";
    if (rtxSsrc) sdp << "a=ssrc:" << rtxSsrc << " cname:styx\r\n";
    try {
        pc->setRemoteDescription(rtc::Description(sdp.str(), rtc::Description::Type::Answer));
    } catch (const std::exception &e) {
        Core::log(Core::LogLevel::Warning, LogStream) << "Rejected server SDP:" << e.what();
        return false;
    }
    return true;
}

void StreamTransport::sendVideoFrame(const std::string &frame, std::chrono::microseconds pts)
{
    if (!connectedFlag || !videoTrack || !videoTrack->isOpen())
        return;
    try {
        videoTrack->sendFrame(reinterpret_cast<const std::byte *>(frame.data()), static_cast<size_t>(frame.size()),
                              rtc::FrameInfo(std::chrono::duration<double>(pts)));
    } catch (const std::exception &e) {
        Core::log(Core::LogLevel::Warning, LogStream) << "Video send failed:" << e.what();
    }
}

void StreamTransport::sendAudioFrame(const std::string &opus, std::chrono::microseconds pts)
{
    if (!connectedFlag || !audioTrack || !audioTrack->isOpen())
        return;
    try {
        audioTrack->sendFrame(reinterpret_cast<const std::byte *>(opus.data()), static_cast<size_t>(opus.size()),
                              rtc::FrameInfo(std::chrono::duration<double>(pts)));
    } catch (const std::exception &e) {
        Core::log(Core::LogLevel::Warning, LogStream) << "Audio send failed:" << e.what();
    }
}

void StreamTransport::sendRtcp(const std::string &packet)
{
    if (!connectedFlag || !videoTrack || !videoTrack->isOpen())
        return;
    try {
        videoTrack->send(reinterpret_cast<const std::byte *>(packet.data()), static_cast<size_t>(packet.size()));
    } catch (const std::exception &e) {
        Core::log(Core::LogLevel::Debug, LogStream) << "RTCP send failed:" << e.what();
    }
}

void StreamTransport::close()
{
    {
        std::lock_guard lock(guard->mutex);
        guard->alive = false;
    }
    connectedFlag = false;
    if (pc) {
        pc->resetCallbacks();
        try {
            pc->close();
        } catch (const std::exception &e) {
            Core::log(Core::LogLevel::Debug, LogStream) << "PeerConnection close:" << e.what();
        }
    }
    audioTrack.reset();
    videoTrack.reset();
    pc.reset();
}

} // namespace Stream
} // namespace Discord
} // namespace Styx
