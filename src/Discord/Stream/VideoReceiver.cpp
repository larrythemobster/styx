#include "VideoReceiver.hpp"

#include "StreamTransport.hpp"

#include "Core/Logging.hpp"

#include <QtEndian>

#include <vector>

namespace Styx {
namespace Discord {
namespace Stream {

namespace {

constexpr uint8_t NAL_TYPE_MASK = 0x1F;
constexpr uint8_t NAL_IDR = 5;
constexpr uint8_t NAL_SPS = 7;
constexpr uint8_t NAL_STAP_A = 24;
constexpr uint8_t NAL_FU_A = 28;
constexpr char START_CODE[4] = { 0, 0, 0, 1 };

struct ParsedRtp
{
    uint8_t payloadType = 0;
    bool marker = false;
    quint16 sequence = 0;
    quint32 timestamp = 0;
    quint32 ssrc = 0;
    int payloadOffset = 0;
    int payloadSize = 0;
};

bool parseRtp(const QByteArray &packet, ParsedRtp &out)
{
    const int size = static_cast<int>(packet.size());
    if (size < 12)
        return false;
    const auto *p = reinterpret_cast<const uint8_t *>(packet.constData());
    if ((p[0] >> 6) != 2)
        return false;

    const bool padding = p[0] & 0x20;
    const bool extension = p[0] & 0x10;
    const int csrcCount = p[0] & 0x0F;

    out.marker = p[1] & 0x80;
    out.payloadType = p[1] & 0x7F;
    out.sequence = qFromBigEndian<quint16>(p + 2);
    out.timestamp = qFromBigEndian<quint32>(p + 4);
    out.ssrc = qFromBigEndian<quint32>(p + 8);

    int offset = 12 + csrcCount * 4;
    if (extension) {
        if (size < offset + 4)
            return false;
        offset += 4 + qFromBigEndian<quint16>(p + offset + 2) * 4;
    }
    int end = size;
    if (padding) {
        if (size < 1)
            return false;
        end -= p[size - 1];
    }
    if (offset > end)
        return false;

    out.payloadOffset = offset;
    out.payloadSize = end - offset;
    return true;
}

} // namespace

VideoReceiver::VideoReceiver(QObject *parent) : QObject(parent)
{
    clock.start();
    tick.setInterval(20);
    connect(&tick, &QTimer::timeout, this, &VideoReceiver::onTick);
    tick.start();
}

void VideoReceiver::setSsrcs(quint32 video, quint32 rtx, quint32 audio)
{
    if (video != videoSsrc)
        reset();
    videoSsrc = video;
    rtxSsrc = rtx;
    audioSsrc = audio;
}

void VideoReceiver::reset()
{
    buffer.clear();
    missing.clear();
    lastUnwrapped.reset();
    nextFrameStart.reset();
    waitingForKeyframe = true;
}

int64_t VideoReceiver::unwrap(quint16 sequence)
{
    if (!lastUnwrapped) {
        lastUnwrapped = sequence;
        return sequence;
    }
    const auto lastLow = static_cast<quint16>(*lastUnwrapped & 0xFFFF);
    const int16_t delta = static_cast<int16_t>(static_cast<quint16>(sequence - lastLow));
    const int64_t value = *lastUnwrapped + delta;
    if (value > *lastUnwrapped)
        lastUnwrapped = value;
    return value;
}

bool VideoReceiver::payloadStartsKeyframe(const QByteArray &payload)
{
    if (payload.isEmpty())
        return false;
    const auto *p = reinterpret_cast<const uint8_t *>(payload.constData());
    const uint8_t type = p[0] & NAL_TYPE_MASK;
    if (type == NAL_SPS)
        return true;
    if (type == NAL_STAP_A && payload.size() >= 4)
        return (p[3] & NAL_TYPE_MASK) == NAL_SPS;
    if (type == NAL_IDR)
        return true;
    if (type == NAL_FU_A && payload.size() >= 2)
        return (p[1] & 0x80) && (p[1] & NAL_TYPE_MASK) == NAL_IDR;
    return false;
}

void VideoReceiver::handleRtp(const QByteArray &packet)
{
    ParsedRtp rtp;
    if (!parseRtp(packet, rtp))
        return;

    const bool isAudio = (audioSsrc != 0 && rtp.ssrc == audioSsrc) || (audioSsrc == 0 && rtp.payloadType == OPUS_PAYLOAD_TYPE);
    if (isAudio) {
        if (rtp.payloadSize > 0)
            emit audioReady(rtp.ssrc, rtp.sequence, rtp.timestamp, packet.mid(rtp.payloadOffset, rtp.payloadSize));
        return;
    }

    if (rtp.payloadType == H264_RTX_PAYLOAD_TYPE || (rtxSsrc != 0 && rtp.ssrc == rtxSsrc)) {
        // RFC 4588: 2-byte original sequence number, then the original payload.
        // Shorter packets are bandwidth probes (padding only).
        if (rtp.payloadSize <= 2)
            return;
        const auto *p = reinterpret_cast<const uint8_t *>(packet.constData()) + rtp.payloadOffset;
        const quint16 original = qFromBigEndian<quint16>(p);
        counters.packets++;
        insertVideo(original, rtp.timestamp, rtp.marker, packet.mid(rtp.payloadOffset + 2, rtp.payloadSize - 2));
        return;
    }

    if (rtp.payloadType != H264_PAYLOAD_TYPE)
        return;
    if (videoSsrc == 0)
        videoSsrc = rtp.ssrc;
    else if (rtp.ssrc != videoSsrc)
        return;
    if (rtp.payloadSize == 0)
        return;

    counters.packets++;
    insertVideo(rtp.sequence, rtp.timestamp, rtp.marker, packet.mid(rtp.payloadOffset, rtp.payloadSize));
}

void VideoReceiver::insertVideo(quint16 sequence, quint32 timestamp, bool marker, QByteArray payload)
{
    const int64_t seq = unwrap(sequence);
    const qint64 now = clock.elapsed();

    if (nextFrameStart && seq < *nextFrameStart)
        return; // already emitted or given up on
    if (buffer.count(seq))
        return;

    if (missing.erase(seq))
        counters.recovered++;

    // a jump this large is a sender restart, not loss
    if (!buffer.empty() && std::abs(seq - buffer.rbegin()->first) > 1000) {
        qCInfo(LogStream) << "Video sequence jumped, resetting receiver";
        reset();
        unwrap(sequence);
    }

    if (!buffer.empty()) {
        const int64_t newest = buffer.rbegin()->first;
        for (int64_t s = newest + 1; s < seq; ++s)
            missing.try_emplace(s, Missing{ now, 0, 0 });
    }

    Packet entry;
    entry.timestamp = timestamp;
    entry.marker = marker;
    entry.keyframeStart = payloadStartsKeyframe(payload);
    entry.payload = std::move(payload);
    buffer.emplace(seq, std::move(entry));

    while (static_cast<int>(buffer.size()) > MAX_BUFFERED_PACKETS) {
        missing.erase(missing.begin(), missing.upper_bound(buffer.begin()->first));
        buffer.erase(buffer.begin());
        waitingForKeyframe = true;
        nextFrameStart.reset();
    }

    assembleFrames();
}

void VideoReceiver::resyncToKeyframe()
{
    // NAL type the packet's payload begins with (inner type for STAP-A / a starting FU-A)
    auto firstNalType = [](const QByteArray &payload) -> int {
        if (payload.isEmpty())
            return -1;
        const auto *p = reinterpret_cast<const uint8_t *>(payload.constData());
        const uint8_t type = p[0] & NAL_TYPE_MASK;
        if (type == NAL_STAP_A)
            return payload.size() >= 4 ? (p[3] & NAL_TYPE_MASK) : -1;
        if (type == NAL_FU_A)
            return payload.size() >= 2 && (p[1] & 0x80) ? (p[1] & NAL_TYPE_MASK) : -1;
        return type;
    };
    constexpr int NAL_AUD = 9;

    for (auto it = buffer.begin(); it != buffer.end(); ++it) {
        if (!it->second.keyframeStart)
            continue;

        // An access unit may open with an AUD or SEI before its SPS: back up to the first
        // packet of this timestamp we hold.
        int64_t start = it->first;
        for (auto prev = buffer.find(start - 1); prev != buffer.end() && prev->second.timestamp == it->second.timestamp;
             prev = buffer.find(start - 1))
            start = prev->first;

        // That packet is the access unit's first only if the one before it belongs to an
        // older frame, or it opens with a NAL that can only start an access unit.
        auto before = buffer.find(start - 1);
        const bool previousFrameEnded = before != buffer.end() && before->second.timestamp != it->second.timestamp;
        const int nal = firstNalType(buffer.at(start).payload);
        if (!previousFrameEnded && nal != NAL_AUD && nal != NAL_SPS)
            continue;

        buffer.erase(buffer.begin(), buffer.find(start));
        missing.erase(missing.begin(), missing.lower_bound(start));
        nextFrameStart = start;
        waitingForKeyframe = false;
        return;
    }
}

void VideoReceiver::assembleFrames()
{
    for (;;) {
        if (waitingForKeyframe) {
            resyncToKeyframe();
            if (waitingForKeyframe)
                return;
        }

        const int64_t start = *nextFrameStart;
        auto it = buffer.find(start);
        if (it == buffer.end())
            return;

        const quint32 timestamp = it->second.timestamp;
        int64_t seq = start;
        int64_t end = -1;
        for (;;) {
            auto current = buffer.find(seq);
            if (current == buffer.end())
                return; // hole: wait for retransmission
            if (current->second.timestamp != timestamp) {
                // the previous packet ended the frame but its marker never made it
                end = seq - 1;
                break;
            }
            if (current->second.marker) {
                end = seq;
                break;
            }
            ++seq;
        }

        if (end < start)
            return;
        emitFrame(start, end);
        nextFrameStart = end + 1;
    }
}

bool VideoReceiver::emitFrame(int64_t first, int64_t last)
{
    QByteArray frame;
    quint32 timestamp = 0;
    bool keyframe = false;
    bool malformed = false;

    for (int64_t seq = first; seq <= last; ++seq) {
        auto it = buffer.find(seq);
        if (it == buffer.end())
            continue;
        const Packet &packet = it->second;
        timestamp = packet.timestamp;
        const QByteArray &payload = packet.payload;
        const auto *p = reinterpret_cast<const uint8_t *>(payload.constData());
        const int size = static_cast<int>(payload.size());
        const uint8_t type = p[0] & NAL_TYPE_MASK;

        if (type >= 1 && type <= 23) {
            frame.append(START_CODE, 4);
            frame.append(payload);
            keyframe |= type == NAL_IDR || type == NAL_SPS;
        } else if (type == NAL_STAP_A) {
            int offset = 1;
            while (offset + 2 <= size) {
                const int length = qFromBigEndian<quint16>(p + offset);
                offset += 2;
                if (length == 0 || offset + length > size) {
                    malformed = true;
                    break;
                }
                const uint8_t inner = p[offset] & NAL_TYPE_MASK;
                keyframe |= inner == NAL_IDR || inner == NAL_SPS;
                frame.append(START_CODE, 4);
                frame.append(payload.constData() + offset, length);
                offset += length;
            }
        } else if (type == NAL_FU_A && size >= 2) {
            const uint8_t header = p[1];
            if (header & 0x80) {
                const uint8_t reconstructed = (p[0] & 0xE0) | (header & NAL_TYPE_MASK);
                keyframe |= (header & NAL_TYPE_MASK) == NAL_IDR;
                frame.append(START_CODE, 4);
                frame.append(static_cast<char>(reconstructed));
            }
            frame.append(payload.constData() + 2, size - 2);
        } else {
            malformed = true;
        }
    }

    buffer.erase(buffer.lower_bound(first), buffer.upper_bound(last));
    missing.erase(missing.begin(), missing.upper_bound(last));

    if (malformed || frame.isEmpty()) {
        counters.framesDropped++;
        requestKeyframe();
        return false;
    }

    counters.frames++;
    emit frameReady(frame, timestamp, keyframe);
    return true;
}

void VideoReceiver::requestKeyframe()
{
    waitingForKeyframe = true;
    nextFrameStart.reset();
    sendPli();
}

void VideoReceiver::sendPli()
{
    const qint64 now = clock.elapsed();
    if (videoSsrc == 0 || now - lastPliMs < PLI_INTERVAL_MS)
        return;
    lastPliMs = now;
    counters.keyframeRequests++;

    // RFC 4585 6.3.1: PSFB (206), FMT 1, no FCI
    QByteArray pli(12, '\0');
    auto *p = reinterpret_cast<uint8_t *>(pli.data());
    p[0] = 0x80 | 1;
    p[1] = 206;
    qToBigEndian<quint16>(2, p + 2);
    qToBigEndian<quint32>(localSsrc, p + 4);
    qToBigEndian<quint32>(videoSsrc, p + 8);
    emit sendRtcp(pli);
}

void VideoReceiver::sendNacks(qint64 now)
{
    if (videoSsrc == 0 || missing.empty())
        return;

    std::vector<int64_t> due;
    for (auto &[seq, entry] : missing) {
        if (nextFrameStart && seq < *nextFrameStart)
            continue;
        if (entry.nackCount >= MAX_NACKS_PER_PACKET || now - entry.lastNackMs < NACK_INTERVAL_MS)
            continue;
        entry.lastNackMs = now;
        entry.nackCount++;
        due.push_back(seq);
    }
    if (due.empty())
        return;

    // RFC 4585 6.2.1: RTPFB (205), FMT 1, one (PID, BLP) per 17-packet window
    QByteArray fci;
    for (size_t i = 0; i < due.size();) {
        const int64_t pid = due[i];
        quint16 blp = 0;
        size_t j = i + 1;
        while (j < due.size() && due[j] - pid <= 16) {
            blp |= static_cast<quint16>(1u << (due[j] - pid - 1));
            ++j;
        }
        char word[4];
        qToBigEndian<quint16>(static_cast<quint16>(pid & 0xFFFF), word);
        qToBigEndian<quint16>(blp, word + 2);
        fci.append(word, 4);
        i = j;
    }

    QByteArray nack(12, '\0');
    auto *p = reinterpret_cast<uint8_t *>(nack.data());
    p[0] = 0x80 | 1;
    p[1] = 205;
    qToBigEndian<quint16>(static_cast<quint16>(2 + fci.size() / 4), p + 2);
    qToBigEndian<quint32>(localSsrc, p + 4);
    qToBigEndian<quint32>(videoSsrc, p + 8);
    nack.append(fci);

    counters.nacksSent += due.size();
    emit sendRtcp(nack);
}

void VideoReceiver::onTick()
{
    const qint64 now = clock.elapsed();
    sendNacks(now);

    // give up on a hole once retransmission had its chance; restart at the next keyframe
    for (const auto &[seq, entry] : missing) {
        if (nextFrameStart && seq < *nextFrameStart)
            continue;
        if (now - entry.firstSeenMs < GIVE_UP_AFTER_MS)
            break;
        qCDebug(LogStream) << "Giving up on video packet" << seq << "- requesting keyframe";
        counters.framesDropped++;
        buffer.erase(buffer.begin(), buffer.upper_bound(seq));
        missing.erase(missing.begin(), missing.upper_bound(seq));
        requestKeyframe();
        assembleFrames();
        break;
    }

    if (waitingForKeyframe)
        sendPli();
}

} // namespace Stream
} // namespace Discord
} // namespace Styx
