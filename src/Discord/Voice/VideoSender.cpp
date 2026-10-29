// Ported from ouwou upstream/video-streams (71b4907); original implementation retained.
#include "VideoSender.hpp"

#include <algorithm>

#include "ProtocolEndian.hpp"

namespace Styx {
namespace Discord {
namespace Voice {

namespace {

constexpr int VIDEO_CLOCK_KHZ = 90;
constexpr int64_t HISTORY_US = int64_t(VideoSendStream::HISTORY_MS) * 1000;
constexpr int64_t BURST_US = int64_t(VideoSendStream::BURST_MS) * 1000;
constexpr int64_t MAX_QUEUE_US = int64_t(VideoSendStream::MAX_QUEUE_MS) * 1000;
constexpr int64_t RETRANSMISSION_WINDOW_US = int64_t(VideoSendStream::RETRANSMISSION_WINDOW_MS) * 1000;
// A reception report answers one of the last few sender reports; older ones are of no use.
constexpr size_t REMEMBERED_REPORTS = 8;

} // namespace

VideoSendStream::VideoSendStream(uint32_t ssrc, uint32_t rtxSsrc, const Numbering &first)
    : mediaSsrc(ssrc), retransmissionSsrc(rtxSsrc), nextSequence(first.sequence), nextRtxSequence(first.rtxSequence), nextTransportSequence(first.transportSequence), firstPictureId(first.pictureId)
{
}

void VideoSendStream::setCodec(VideoCodec codec, uint8_t mediaPayloadType, std::optional<uint8_t> retransmissionPayloadType)
{
    if (this->codec() == codec && payloadType == mediaPayloadType && rtxPayloadType == retransmissionPayloadType)
        return;

    clear();
    payloadType = mediaPayloadType;
    rtxPayloadType = retransmissionPayloadType;
    packetizer.reset();
    if (VideoPacketizer::supports(codec))
        packetizer.emplace(codec, firstPictureId);
}

void VideoSendStream::setBitrate(int bitsPerSecond)
{
    if (bitsPerSecond > 0)
        targetBitrate = bitsPerSecond;
}

void VideoSendStream::setPacingFactor(double factor)
{
    pacingFactor = factor;
}

int VideoSendStream::maxPayloadSize() const
{
    // As WebRTC does, room is left in every packet for it to be sent again whole.
    return MAX_PACKET_SIZE - CLEAR_SIZE - EXTENSIONS_SIZE - (retransmits() ? RTX_HEADER_SIZE : 0);
}

bool VideoSendStream::queueFrame(std::span<const uint8_t> frame, uint32_t rtpTimestamp, bool keyframe, int64_t nowUs)
{
    if (!packetizer)
        return false;

    std::vector<std::vector<uint8_t>> payloads = packetizer->packetize(frame, keyframe, maxPayloadSize());
    if (payloads.empty())
        return false;

    for (size_t i = 0; i < payloads.size(); i++) {
        Packet &packet = queued.emplace_back();
        packet.payload = std::move(payloads[i]);
        packet.timestamp = rtpTimestamp;
        packet.marker = i + 1 == payloads.size();
        packet.queuedUs = nowUs;
        queuedBytes += static_cast<int64_t>(packet.payload.size());
    }

    framesQueued = true;
    lastFrameTimestamp = rtpTimestamp;
    lastFrameQueuedUs = nowUs;
    counters.frames++;
    if (keyframe)
        counters.keyframes++;
    return true;
}

void VideoSendStream::retransmit(std::span<const uint16_t> sequences, int64_t nowUs)
{
    counters.nacksReceived += sequences.size();
    if (!retransmits())
        return;

    forgetOld(nowUs);
    for (uint16_t sequence : sequences) {
        Packet *sent = findSent(sequence);
        if (!sent || sent->resendWaiting)
            continue;
        if (sent->resentUs >= 0 && nowUs - sent->resentUs < roundTripUs())
            continue;
        if (!takeResendShare(static_cast<int64_t>(sent->payload.size()), nowUs))
            break;
        sent->resendWaiting = true;
        resends.push_back(sequence);
    }
}

bool VideoSendStream::takeResendShare(int64_t bytes, int64_t nowUs)
{
    while (!resentLately.empty() && nowUs - resentLately.front().atUs >= RETRANSMISSION_WINDOW_US) {
        resentLatelyBytes -= resentLately.front().bytes;
        resentLately.pop_front();
    }

    const int64_t share = static_cast<int64_t>(targetBitrate) * RETRANSMISSION_WINDOW_MS / 8000;
    if (resentLatelyBytes + bytes > share)
        return false;
    resentLately.push_back({ nowUs, bytes });
    resentLatelyBytes += bytes;
    return true;
}

void VideoSendStream::clear()
{
    queued.clear();
    resends.clear();
    history.clear();
    queuedBytes = 0;
    budget = 0;
    budgetedUs = -1;
}

int64_t VideoSendStream::pacingRate(int64_t nowUs) const
{
    const int64_t rate = static_cast<int64_t>(targetBitrate * pacingFactor);
    if (queued.empty())
        return rate;
    const int64_t leftUs = std::max<int64_t>(1000, MAX_QUEUE_US - (nowUs - queued.front().queuedUs));
    return std::max(rate, queuedBytes * 8 * 1000000 / leftUs);
}

void VideoSendStream::startInterval(int64_t nowUs)
{
    const int64_t rate = pacingRate(nowUs);
    const int64_t burst = std::min<int64_t>(rate * BURST_MS / 8000, MAX_BURST_BYTES);
    // After a pause a whole burst may leave at once, as after the first frame.
    const int64_t earnedUs = budgetedUs < 0 ? BURST_US : std::clamp<int64_t>(nowUs - budgetedUs, 0, BURST_US);
    budgetedUs = nowUs;
    budget = std::min(burst, budget + rate * earnedUs / 8000000);
}

bool VideoSendStream::writeNext(int64_t nowUs, PacketBuffer &out)
{
    if (budget <= 0)
        return false;
    forgetOld(nowUs);

    while (!resends.empty()) {
        const uint16_t sequence = resends.front();
        resends.pop_front();

        Packet *original = findSent(sequence);
        if (!original)
            continue;
        original->resendWaiting = false;
        if (!write(out, *original, true, nowUs))
            continue;
        original->resentUs = nowUs;
        budget -= out.size();
        counters.retransmitted++;
        return true;
    }

    while (!queued.empty()) {
        Packet packet = std::move(queued.front());
        queued.pop_front();
        queuedBytes -= static_cast<int64_t>(packet.payload.size());
        packet.sequence = nextSequence;
        packet.sentUs = nowUs;
        if (!write(out, packet, false, nowUs))
            continue;

        nextSequence++;
        budget -= out.size();
        counters.packets++;
        counters.bytes += packet.payload.size();
        history.push_back(std::move(packet));
        return true;
    }
    return false;
}

bool VideoSendStream::write(PacketBuffer &out, const Packet &packet, bool retransmission, int64_t nowUs)
{
    RtpHeader header;
    header.extension = true;
    header.marker = packet.marker;
    header.payloadType = retransmission ? *rtxPayloadType : payloadType;
    header.sequence = retransmission ? nextRtxSequence : packet.sequence;
    header.timestamp = packet.timestamp;
    header.ssrc = retransmission ? retransmissionSsrc : mediaSsrc;

    // abs-send-time is the time in seconds as 6.18 fixed point, cut to its low 24 bits.
    const uint64_t sendTime = ((static_cast<uint64_t>(nowUs) << 18) + 500000) / 1000000;
    const uint8_t sendTimeBytes[3] = { static_cast<uint8_t>(sendTime >> 16), static_cast<uint8_t>(sendTime >> 8), static_cast<uint8_t>(sendTime) };
    // The transmission offset is there but always zero, as the official client sends it.
    const uint8_t noOffset[3] = {};
    const int size = CLEAR_SIZE + EXTENSIONS_SIZE + (retransmission ? RTX_HEADER_SIZE : 0) + static_cast<int>(packet.payload.size());
    uint8_t transportSequence[2];
    writeBigEndian<uint16_t>(transport ? transport->numberPacket(nowUs, size) : nextTransportSequence, transportSequence);
    // In the order WebRTC lays them out, which is not the order of their ids.
    const RtpExtension elements[] = { { ABS_SEND_TIME_EXTENSION, sendTimeBytes }, { TRANSMISSION_OFFSET_EXTENSION, noOffset }, { TRANSPORT_SEQUENCE_EXTENSION, transportSequence } };
    uint8_t extensions[EXTENSIONS_SIZE];
    if (writeRtpExtensions(elements, extensions) != EXTENSIONS_SIZE)
        return false;

    out.clear();
    if (writeRtpHeader(header, { out.data(), static_cast<size_t>(out.tailroom()) }, EXTENSIONS_SIZE / 4) != CLEAR_SIZE)
        return false;
    out.resize(CLEAR_SIZE);
    out.append(extensions);
    if (retransmission) {
        uint8_t originalSequence[RTX_HEADER_SIZE];
        writeBigEndian<uint16_t>(packet.sequence, originalSequence);
        out.append(originalSequence);
    }
    if (!out.append(packet.payload))
        return false;

    nextTransportSequence++;
    if (retransmission)
        nextRtxSequence++;
    counters.bytesWritten += static_cast<uint64_t>(out.size());
    return true;
}

VideoSendStream::Packet *VideoSendStream::findSent(uint16_t sequence)
{
    if (history.empty())
        return nullptr;
    const uint16_t index = static_cast<uint16_t>(sequence - history.front().sequence);
    return index < history.size() ? &history[index] : nullptr;
}

int64_t VideoSendStream::roundTripUs() const
{
    return counters.roundTripMs < 0 ? 0 : int64_t(counters.roundTripMs + ROUND_TRIP_MARGIN_MS) * 1000;
}

void VideoSendStream::forgetOld(int64_t nowUs)
{
    const int64_t keptUs = std::max(HISTORY_US, HISTORY_ROUND_TRIPS * roundTripUs());
    while (!history.empty()) {
        const Packet &oldest = history.front();
        if (history.size() < size_t(MAX_HISTORY_PACKETS)) {
            const int64_t age = nowUs - oldest.sentUs;
            if (oldest.resendWaiting || age <= keptUs)
                return;
            if (history.size() < size_t(HISTORY_PACKETS) && age <= keptUs * SPARSE_HISTORY_FACTOR)
                return;
        }
        history.pop_front();
    }
}

int VideoSendStream::writeSenderReport(std::span<uint8_t> out, int64_t nowUs, uint64_t ntpTime)
{
    if (!canReport())
        return -1;

    SenderInfo info;
    info.ssrc = mediaSsrc;
    info.ntpTime = ntpTime;
    // The RTP time of this moment: the last frame's, moved on by the time since.
    info.rtpTimestamp = lastFrameTimestamp + static_cast<uint32_t>((nowUs - lastFrameQueuedUs) * VIDEO_CLOCK_KHZ / 1000);
    info.packetCount = static_cast<uint32_t>(counters.packets);
    info.octetCount = static_cast<uint32_t>(counters.bytes);

    const int size = Voice::writeSenderReport(out, info);
    if (size < 0)
        return size;

    sentReports.push_back({ compactNtp(ntpTime), nowUs });
    if (sentReports.size() > REMEMBERED_REPORTS)
        sentReports.pop_front();
    return size;
}

void VideoSendStream::onReceptionReport(const ReceptionReport &report, int64_t nowUs)
{
    counters.lossFraction = report.fractionLost / 256.0;

    // Zero stands for no sender report received yet.
    if (report.lastSenderReport == 0)
        return;
    const auto answered = std::find_if(sentReports.begin(), sentReports.end(), [&report](const SentReport &sent) { return sent.compactNtp == report.lastSenderReport; });
    if (answered == sentReports.end())
        return;
    // The receiver says how long it held the report, in units of 1/65536 of a second.
    const int64_t heldUs = static_cast<int64_t>(report.delaySinceLastSenderReport) * 1000000 / 65536;
    counters.roundTripMs = static_cast<int>(std::max<int64_t>(0, nowUs - answered->sentUs - heldUs) / 1000);
}

} // namespace Voice
} // namespace Discord
} // namespace Styx
