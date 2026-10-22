#include "StreamTransport.hpp"

#include "Core/Logging.hpp"

#include <rtc/rtc.hpp>

#include <QStringList>

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

QString sdpLine(const QStringList &lines, const QString &prefix)
{
    for (const QString &line : lines) {
        if (line.startsWith(prefix))
            return line.trimmed();
    }
    return {};
}

} // namespace

StreamTransport::StreamTransport(Role role, QObject *parent)
    : QObject(parent), role(role), guard(std::make_shared<Guard>())
{
}

StreamTransport::~StreamTransport()
{
    close();
}

QList<Voice::Codec> StreamTransport::codecs()
{
    Voice::Codec opus;
    opus.name = QStringLiteral("opus");
    opus.type = QStringLiteral("audio");
    opus.priority = 1000;
    opus.payloadType = OPUS_PAYLOAD_TYPE;

    Voice::Codec h264;
    h264.name = QStringLiteral("H264");
    h264.type = QStringLiteral("video");
    h264.priority = 1000;
    h264.payloadType = H264_PAYLOAD_TYPE;
    h264.rtxPayloadType = H264_RTX_PAYLOAD_TYPE;
    h264.encode = true;
    h264.decode = true;

    return { opus, h264 };
}

void StreamTransport::setLocalSsrcs(quint32 audio, quint32 video, quint32 rtx)
{
    audioSsrc = audio;
    videoSsrc = video;
    rtxSsrc = rtx;
}

void StreamTransport::postToThread(std::function<void()> fn)
{
    QMetaObject::invokeMethod(this, std::move(fn), Qt::QueuedConnection);
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
            QByteArray packet(reinterpret_cast<const char *>(message->data()), static_cast<qsizetype>(message->size()));
            emit rtpReceived(packet);
        }));
    }

    pc->onLocalDescription([this, g](rtc::Description description) {
        std::lock_guard lock(g->mutex);
        if (!g->alive)
            return;
        QString sdp = QString::fromStdString(std::string(description));
        postToThread([this, sdp] { emit localOfferReady(sdp); });
    });

    pc->onStateChange([this, g](rtc::PeerConnection::State state) {
        std::lock_guard lock(g->mutex);
        if (!g->alive)
            return;
        qCInfo(LogStream) << "Stream transport state:" << static_cast<int>(state);
        switch (state) {
        case rtc::PeerConnection::State::Connected:
            connectedFlag = true;
            postToThread([this] { emit connected(); });
            break;
        case rtc::PeerConnection::State::Failed:
        case rtc::PeerConnection::State::Closed:
        case rtc::PeerConnection::State::Disconnected: {
            bool was = connectedFlag.exchange(false);
            QString reason = state == rtc::PeerConnection::State::Failed ? QStringLiteral("ICE/DTLS failed")
                                                                          : QStringLiteral("transport closed");
            if (was || state == rtc::PeerConnection::State::Failed)
                postToThread([this, reason] { emit failed(reason); });
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
            postToThread([this] { emit keyframeRequested(); });
    }));
    videoPacketizer->addToChain(std::make_shared<rtc::RembHandler>([this, g](unsigned int bitrate) {
        std::lock_guard lock(g->mutex);
        if (g->alive)
            postToThread([this, bitrate] { emit bandwidthEstimate(bitrate); });
    }));
    // Spread keyframe bursts (100+ KB) over 5 ms slots at a few times the stream bitrate
    // instead of dumping them on the socket at once.
    const double pacingRate = std::max(8'000'000.0, 3.0 * maxBitrate);
    videoPacketizer->addToChain(std::make_shared<rtc::PacingHandler>(pacingRate, std::chrono::milliseconds(5)));
    videoTrack->setMediaHandler(videoPacketizer);
}

bool StreamTransport::applyServerSdp(const QString &serverSdp)
{
    if (!pc)
        return false;

    const QStringList lines = serverSdp.split(QLatin1Char('\n'));
    const QString connection = sdpLine(lines, QStringLiteral("c="));
    const QString rtcp = sdpLine(lines, QStringLiteral("a=rtcp:"));
    const QString ufrag = sdpLine(lines, QStringLiteral("a=ice-ufrag"));
    const QString pwd = sdpLine(lines, QStringLiteral("a=ice-pwd"));
    const QString fingerprint = sdpLine(lines, QStringLiteral("a=fingerprint"));
    const QString candidate = sdpLine(lines, QStringLiteral("a=candidate"));
    const QString port = rtcp.section(QLatin1Char(':'), 1, 1).section(QLatin1Char(' '), 0, 0);

    if (connection.isEmpty() || port.isEmpty() || ufrag.isEmpty() || pwd.isEmpty() || fingerprint.isEmpty() || candidate.isEmpty()) {
        qCWarning(LogStream) << "Server SDP is missing fields:" << serverSdp;
        return false;
    }

    const QString transport = QStringList{ ufrag, pwd, fingerprint, candidate, QStringLiteral("a=rtcp-mux"), QStringLiteral("a=ice-lite") }.join(QLatin1Char('\n'));

    QStringList sdp;
    sdp << QStringLiteral("v=0")
        << QStringLiteral("o=- 0 0 IN IP4 127.0.0.1")
        << QStringLiteral("s=-")
        << QStringLiteral("t=0 0")
        << QStringLiteral("a=group:BUNDLE 0 1");

    sdp << QStringLiteral("m=audio %1 UDP/TLS/RTP/SAVPF %2").arg(port).arg(OPUS_PAYLOAD_TYPE)
        << connection
        << QStringLiteral("a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level")
        << QStringLiteral("a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01")
        << QStringLiteral("a=setup:passive")
        << QStringLiteral("a=mid:0")
        << QStringLiteral("a=maxptime:60")
        << QStringLiteral("a=inactive")
        << transport
        << QStringLiteral("a=rtpmap:%1 opus/48000/2").arg(OPUS_PAYLOAD_TYPE)
        << QStringLiteral("a=fmtp:%1 minptime=10;useinbandfec=1;usedtx=1").arg(OPUS_PAYLOAD_TYPE)
        << QStringLiteral("a=rtcp-fb:%1 transport-cc").arg(OPUS_PAYLOAD_TYPE)
        << QStringLiteral("a=rtcp-fb:%1 nack").arg(OPUS_PAYLOAD_TYPE);
    // libdatachannel routes incoming RTCP by the SSRCs in the *remote* description. Feedback
    // from the media server (NACK, PLI, REMB) names our SSRCs as its media source, so list
    // them here or it never reaches the NACK responder and PLI handler.
    if (audioSsrc != 0)
        sdp << QStringLiteral("a=ssrc:%1 cname:styx").arg(audioSsrc);

    sdp << QStringLiteral("m=video %1 UDP/TLS/RTP/SAVPF %2 %3").arg(port).arg(H264_PAYLOAD_TYPE).arg(H264_RTX_PAYLOAD_TYPE)
        << connection
        << QStringLiteral("a=extmap:2 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time")
        << QStringLiteral("a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01")
        << QStringLiteral("a=extmap:14 urn:ietf:params:rtp-hdrext:toffset")
        << QStringLiteral("a=extmap:13 urn:3gpp:video-orientation")
        << QStringLiteral("a=extmap:5 http://www.webrtc.org/experiments/rtp-hdrext/playout-delay")
        << QStringLiteral("a=setup:passive")
        << QStringLiteral("a=mid:1")
        << QStringLiteral("a=inactive")
        << transport
        << QStringLiteral("a=rtpmap:%1 H264/90000").arg(H264_PAYLOAD_TYPE)
        << QStringLiteral("a=fmtp:%1 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f").arg(H264_PAYLOAD_TYPE)
        << QStringLiteral("a=rtpmap:%1 rtx/90000").arg(H264_RTX_PAYLOAD_TYPE)
        << QStringLiteral("a=fmtp:%1 apt=%2").arg(H264_RTX_PAYLOAD_TYPE).arg(H264_PAYLOAD_TYPE)
        << QStringLiteral("a=rtcp-fb:%1 ccm fir").arg(H264_PAYLOAD_TYPE)
        << QStringLiteral("a=rtcp-fb:%1 nack").arg(H264_PAYLOAD_TYPE)
        << QStringLiteral("a=rtcp-fb:%1 nack pli").arg(H264_PAYLOAD_TYPE)
        << QStringLiteral("a=rtcp-fb:%1 goog-remb").arg(H264_PAYLOAD_TYPE)
        << QStringLiteral("a=rtcp-fb:%1 transport-cc").arg(H264_PAYLOAD_TYPE);
    if (videoSsrc != 0)
        sdp << QStringLiteral("a=ssrc:%1 cname:styx").arg(videoSsrc);
    if (rtxSsrc != 0)
        sdp << QStringLiteral("a=ssrc:%1 cname:styx").arg(rtxSsrc);

    try {
        pc->setRemoteDescription(rtc::Description(sdp.join(QStringLiteral("\r\n")).toStdString() + "\r\n", rtc::Description::Type::Answer));
    } catch (const std::exception &e) {
        qCWarning(LogStream) << "Rejected server SDP:" << e.what();
        return false;
    }
    return true;
}

void StreamTransport::sendVideoFrame(const QByteArray &frame, std::chrono::microseconds pts)
{
    if (!connectedFlag || !videoTrack || !videoTrack->isOpen())
        return;
    try {
        videoTrack->sendFrame(reinterpret_cast<const std::byte *>(frame.constData()), static_cast<size_t>(frame.size()),
                              rtc::FrameInfo(std::chrono::duration<double>(pts)));
    } catch (const std::exception &e) {
        qCWarning(LogStream) << "Video send failed:" << e.what();
    }
}

void StreamTransport::sendAudioFrame(const QByteArray &opus, std::chrono::microseconds pts)
{
    if (!connectedFlag || !audioTrack || !audioTrack->isOpen())
        return;
    try {
        audioTrack->sendFrame(reinterpret_cast<const std::byte *>(opus.constData()), static_cast<size_t>(opus.size()),
                              rtc::FrameInfo(std::chrono::duration<double>(pts)));
    } catch (const std::exception &e) {
        qCWarning(LogStream) << "Audio send failed:" << e.what();
    }
}

void StreamTransport::sendRtcp(const QByteArray &packet)
{
    if (!connectedFlag || !videoTrack || !videoTrack->isOpen())
        return;
    try {
        videoTrack->send(reinterpret_cast<const std::byte *>(packet.constData()), static_cast<size_t>(packet.size()));
    } catch (const std::exception &e) {
        qCDebug(LogStream) << "RTCP send failed:" << e.what();
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
            qCDebug(LogStream) << "PeerConnection close:" << e.what();
        }
    }
    audioTrack.reset();
    videoTrack.reset();
    pc.reset();
}

} // namespace Stream
} // namespace Discord
} // namespace Styx
