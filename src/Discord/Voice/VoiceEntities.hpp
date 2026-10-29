#pragma once

#include <charconv>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Core/JsonUtils.hpp"
#include "Core/OrderedJson.hpp"
#include "Core/ProxyConfig.hpp"
#include "Core/Snowflake.hpp"
#include "VoiceEnums.hpp"


namespace Styx {
namespace Discord {
namespace Voice {

struct VoiceHello : Core::JsonUtils::JsonObject
{
    Field<int> heartbeatInterval;

    static VoiceHello fromJson(const nlohmann::json &obj)
    {
        VoiceHello hello;
        get(obj, "heartbeat_interval", hello.heartbeatInterval);
        return hello;
    }
};

// One simulcast layer of a media session. Go Live sessions get one ("screen", rid "100").
struct StreamResolution : Core::JsonUtils::JsonObject
{
    Field<std::string> type;
    Field<int> width;
    Field<int> height;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "type", type);
        insert(obj, "width", width);
        insert(obj, "height", height);
        return obj;
    }

    static StreamResolution fromJson(const nlohmann::json &obj)
    {
        StreamResolution resolution;
        get(obj, "type", resolution.type);
        get(obj, "width", resolution.width);
        get(obj, "height", resolution.height);
        return resolution;
    }
};

// One entry of a `streams` array: a video layer as IDENTIFY offers it, READY assigns it, and
// VIDEO (opcode 12) announces it.
struct StreamDescription : Core::JsonUtils::JsonObject
{
    Field<std::string> type;
    Field<std::string> rid;
    Field<std::uint32_t, true> ssrc;
    Field<bool, true> active;
    Field<int, true> quality;
    Field<std::uint32_t, true> rtxSsrc;
    Field<int, true> maxBitrate;
    Field<int, true> maxFramerate;
    Field<StreamResolution, true> maxResolution;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "type", type);
        insert(obj, "rid", rid);
        insert(obj, "ssrc", ssrc);
        insert(obj, "active", active);
        insert(obj, "quality", quality);
        insert(obj, "rtx_ssrc", rtxSsrc);
        insert(obj, "max_bitrate", maxBitrate);
        insert(obj, "max_framerate", maxFramerate);
        insert(obj, "max_resolution", maxResolution);
        return obj;
    }

    static StreamDescription fromJson(const nlohmann::json &obj)
    {
        StreamDescription stream;
        get(obj, "type", stream.type);
        get(obj, "rid", stream.rid);
        get(obj, "ssrc", stream.ssrc);
        get(obj, "active", stream.active);
        get(obj, "quality", stream.quality);
        get(obj, "rtx_ssrc", stream.rtxSsrc);
        get(obj, "max_bitrate", stream.maxBitrate);
        get(obj, "max_framerate", stream.maxFramerate);
        get(obj, "max_resolution", stream.maxResolution);
        return stream;
    }
};

using MediaStreamInfo = StreamDescription;

struct VoiceReady : Core::JsonUtils::JsonObject
{
    Field<std::uint32_t> ssrc;
    Field<std::string> ip;
    Field<int> port;
    Field<std::vector<std::string>> modes;
    Field<std::vector<StreamDescription>, true> streams;

    static VoiceReady fromJson(const nlohmann::json &obj)
    {
        VoiceReady ready;
        get(obj, "ssrc", ready.ssrc);
        get(obj, "ip", ready.ip);
        get(obj, "port", ready.port);
        get(obj, "modes", ready.modes);
        get(obj, "streams", ready.streams);
        return ready;
    }
};

// Inbound op 12: which SSRCs another participant sends video on.
struct VideoData : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake, true> userId;
    Field<std::uint32_t> audioSsrc;
    Field<std::uint32_t> videoSsrc;
    Field<std::uint32_t, true> rtxSsrc;
    Field<std::vector<StreamDescription>, true> streams;

    static VideoData fromJson(const nlohmann::json &obj)
    {
        VideoData data;
        get(obj, "user_id", data.userId);
        get(obj, "audio_ssrc", data.audioSsrc);
        get(obj, "video_ssrc", data.videoSsrc);
        get(obj, "rtx_ssrc", data.rtxSsrc);
        get(obj, "streams", data.streams);
        return data;
    }
};

struct Codec : Core::JsonUtils::JsonObject
{
    Field<std::string> name;
    Field<std::string> type;
    Field<int> priority;
    Field<int> payloadType;
    Field<int, true> rtxPayloadType;
    Field<bool, true> encode;
    Field<bool, true> decode;

    static constexpr int OPUS_PAYLOAD_TYPE = 120;

    static Codec opus()
    {
        Codec codec;
        codec.name = "opus";
        codec.type = "audio";
        codec.priority = 1000;
        codec.payloadType = OPUS_PAYLOAD_TYPE;
        return codec;
    }

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "name", name);
        insert(obj, "type", type);
        insert(obj, "priority", priority);
        insert(obj, "payload_type", payloadType);
        insert(obj, "rtx_payload_type", rtxPayloadType);
        insert(obj, "encode", encode);
        insert(obj, "decode", decode);
        return obj;
    }
};

struct SelectProtocolData : Core::JsonUtils::JsonObject
{
    Field<std::string> protocol = "udp";
    Field<std::string> address;
    Field<int> port;
    Field<std::string> mode;
    Field<std::vector<Codec>> codecs;
    Field<std::string, true> rtcConnectionId;
    Field<std::vector<std::string>, true> experiments;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson data;
        insert(data, "address", address);
        insert(data, "port", port);
        insert(data, "mode", mode);

        Core::OrderedJson obj;
        insert(obj, "protocol", protocol);
        obj.insert("data", data);
        insert(obj, "address", address);
        insert(obj, "port", port);
        insert(obj, "mode", mode);
        insert(obj, "codecs", codecs);
        insert(obj, "rtc_connection_id", rtcConnectionId);
        insert(obj, "experiments", experiments);
        return obj;
    }
};

struct SessionDescription : Core::JsonUtils::JsonObject
{
    Field<std::string> mode;
    Field<std::string> secretKey;
    Field<int, true> daveProtocolVersion;
    Field<std::string, true> sdp; // only for protocol "webrtc"
    Field<std::string, true> videoCodec;

    static SessionDescription fromJson(const nlohmann::json &obj)
    {
        SessionDescription desc;
        get(obj, "mode", desc.mode);
        if (auto it = obj.find("secret_key"); it != obj.end() && it->is_array()) {
            std::string key;
            key.reserve(it->size());
            for (const auto &byte : *it)
                key.push_back(static_cast<char>(byte.get<int>()));
            desc.secretKey = key;
        }
        get(obj, "dave_protocol_version", desc.daveProtocolVersion);
        get(obj, "sdp", desc.sdp);
        get(obj, "video_codec", desc.videoCodec);
        return desc;
    }
};

// SESSION_UPDATE (opcode 14) from the server: any of these may change mid-session.
struct SessionUpdate : Core::JsonUtils::JsonObject
{
    Field<std::string, true> audioCodec;
    Field<std::string, true> videoCodec;
    Field<std::string, true> mediaSessionId;
    Field<int, true> keyframeInterval;

    static SessionUpdate fromJson(const nlohmann::json &obj)
    {
        SessionUpdate update;
        get(obj, "audio_codec", update.audioCodec);
        get(obj, "video_codec", update.videoCodec);
        get(obj, "media_session_id", update.mediaSessionId);
        get(obj, "keyframe_interval", update.keyframeInterval);
        return update;
    }
};

// VIDEO (opcode 12) to the server: what this client is sending.
struct LocalVideoState : Core::JsonUtils::JsonObject
{
    Field<std::uint32_t> audioSsrc;
    Field<std::uint32_t> videoSsrc;
    Field<std::uint32_t> rtxSsrc;
    Field<std::vector<StreamDescription>> streams;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "audio_ssrc", audioSsrc);
        insert(obj, "video_ssrc", videoSsrc);
        insert(obj, "rtx_ssrc", rtxSsrc);
        insert(obj, "streams", streams);
        return obj;
    }
};

// MEDIA_SINK_WANTS (opcode 15): how much of each sender's video a receiver wants, 0 to 100.
struct MediaSinkWants
{
    std::optional<int> any;
    std::map<std::uint32_t, int> bySsrc;
    std::map<std::uint32_t, int> pixelCounts;

    // The official client builds this as a JS object, which serializes integer-like keys first
    // in ascending order, so the SSRC entries precede `any` whatever order they were set in.
    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        for (const auto &[ssrc, value] : bySsrc)
            obj.insert(std::to_string(ssrc), value);
        if (any)
            obj.insert("any", *any);
        if (!pixelCounts.empty()) {
            Core::OrderedJson counts;
            for (const auto &[ssrc, value] : pixelCounts)
                counts.insert(std::to_string(ssrc), value);
            obj.insert("pixelCounts", counts);
        }
        return obj;
    }

    static MediaSinkWants fromJson(const nlohmann::json &obj)
    {
        MediaSinkWants wants;
        if (!obj.is_object())
            return wants;

        auto number = [](const nlohmann::json &value) {
            return value.is_number() ? value.get<int>() : 0;
        };
        auto ssrcKey = [](const std::string &key, std::uint32_t &ssrc) {
            const auto result = std::from_chars(key.data(), key.data() + key.size(), ssrc);
            return result.ec == std::errc{} && result.ptr == key.data() + key.size();
        };
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (it.key() == "any") {
                wants.any = number(it.value());
            } else if (it.key() == "pixelCounts") {
                if (it.value().is_object()) {
                    for (auto count = it.value().begin(); count != it.value().end(); ++count) {
                        std::uint32_t ssrc = 0;
                        ssrcKey(count.key(), ssrc);
                        wants.pixelCounts[ssrc] = number(count.value());
                    }
                }
            } else {
                std::uint32_t ssrc = 0;
                if (ssrcKey(it.key(), ssrc))
                    wants.bySsrc[ssrc] = number(it.value());
            }
        }
        return wants;
    }
};

struct SpeakingData : Core::JsonUtils::JsonObject
{
    Field<int> speaking;
    Field<int> delay;
    Field<std::uint32_t> ssrc;
    Field<Core::Snowflake, true> userId;

    static SpeakingData fromJson(const nlohmann::json &obj)
    {
        SpeakingData data;
        get(obj, "speaking", data.speaking);
        get(obj, "delay", data.delay);
        get(obj, "ssrc", data.ssrc);
        get(obj, "user_id", data.userId);
        return data;
    }

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "speaking", speaking);
        insert(obj, "delay", delay);
        insert(obj, "ssrc", ssrc);
        return obj;
    }
};

// VIDEO (opcode 12) from the server: the SSRCs one user sends on.
struct ClientConnectData : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake> userId;
    Field<std::uint32_t> audioSsrc;
    Field<std::uint32_t> videoSsrc;
    Field<std::vector<StreamDescription>, true> streams;

    static ClientConnectData fromJson(const nlohmann::json &obj)
    {
        ClientConnectData data;
        get(obj, "user_id", data.userId);
        get(obj, "audio_ssrc", data.audioSsrc);
        get(obj, "video_ssrc", data.videoSsrc);
        get(obj, "streams", data.streams);
        return data;
    }
};

struct VoiceIdentifyData : Core::JsonUtils::JsonObject
{
    Field<Core::Snowflake, true> channelId;
    Field<Core::Snowflake> serverId;
    Field<Core::Snowflake> userId;
    Field<std::string> sessionId;
    Field<std::string> token;
    Field<int> maxDaveProtocolVersion = 1;
    Field<bool, true> video;
    Field<std::vector<StreamDescription>, true> streams;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "server_id", serverId);
        insert(obj, "channel_id", channelId);
        insert(obj, "user_id", userId);
        insert(obj, "session_id", sessionId);
        insert(obj, "token", token);
        insert(obj, "max_dave_protocol_version", maxDaveProtocolVersion);
        insert(obj, "video", video);
        if (streams.hasValue()) {
            insert(obj, "streams", streams);
        } else if (video.hasValue() && video.get()) {
            // the one simulcast layer every Discord client asks for on a Go Live session
            Core::OrderedJson layer;
            layer.insert("type", "screen");
            layer.insert("rid", "100");
            layer.insert("quality", 100);
            Core::OrderedJson::Array s;
            s.append(layer);
            obj.insert("streams", s);
        }
        return obj;
    }
};

// Everything that distinguishes one RTC connection from another. A voice connection and a
// Go Live stream connection speak the same protocol and differ only in these values.
struct ConnectionParams
{
    std::string endpoint;
    // Discord's voice gateways are only reached over TLS. A test's stand-in server is not.
    std::string gatewayScheme = "wss";
    std::string token;
    Core::Snowflake serverId;
    Core::Snowflake channelId;
    Core::Snowflake userId;
    std::string sessionId;
    Core::ProxyConfig proxy;

    // What IDENTIFY declares about video. Left unset, the server treats the connection as
    // audio-only.
    std::optional<bool> video;
    std::vector<StreamDescription> streams;
    // The video codecs SELECT_PROTOCOL offers after Opus, in preference order. Incoming video is
    // told apart by the payload types given here.
    std::vector<Codec> videoCodecs;
    // A stream's viewer has no microphone on that connection and sends no audio at all, not
    // even the silence a voice connection opens with. Its owner sends the sound that is shared
    // with it, if any is.
    bool sendsAudio = true;
    // A Go Live stream's connection rather than a voice channel's. The same messages mean
    // slightly different things on the two.
    bool streamContext = false;
};

inline std::vector<StreamDescription> cameraStreams(bool withSecondLayer)
{
    StreamDescription full;
    full.type = "video";
    full.rid = "100";
    full.quality = 100;
    if (!withSecondLayer)
        return { full };

    StreamDescription second = full;
    second.rid = "50";
    second.quality = 50;
    return { full, second };
}


struct VoiceResumeData : Core::JsonUtils::JsonObject
{
    Field<std::string> token;
    Field<std::string> sessionId;
    Field<Core::Snowflake> serverId;
    Field<Core::Snowflake, true> channelId;
    Field<int> seqAck;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "token", token);
        insert(obj, "session_id", sessionId);
        insert(obj, "server_id", serverId);
        insert(obj, "channel_id", channelId);
        insert(obj, "seq_ack", seqAck);
        return obj;
    }
};

} // namespace Voice
} // namespace Discord
} // namespace Styx
