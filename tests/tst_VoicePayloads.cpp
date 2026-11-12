// Faithful port of ouwou upstream/video-streams (71b4907) tests/tst_VoicePayloads.cpp to Catch2.
#include <catch2/catch_test_macros.hpp>

#include "Discord/Enums.hpp"
#include "Discord/Outbound.hpp"
#include "Discord/Voice/VoiceEntities.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using namespace Styx;
using namespace Styx::Discord::Voice;

namespace {

nlohmann::json parse(const char *json)
{
    return nlohmann::json::parse(json);
}

StreamDescription offeredStream(const std::string &type, const std::string &rid, int quality)
{
    StreamDescription stream;
    stream.type = type;
    stream.rid = rid;
    stream.quality = quality;
    return stream;
}

VoiceIdentifyData identifyFor(uint64_t serverId, uint64_t channelId)
{
    VoiceIdentifyData identify;
    identify.serverId = Core::Snowflake(serverId);
    identify.channelId = Core::Snowflake(channelId);
    identify.userId = Core::Snowflake(300);
    identify.sessionId = std::string("session");
    identify.token = std::string("token");
    return identify;
}

Codec videoCodec(const std::string &name, int index, bool encode, bool decode)
{
    Codec codec;
    codec.name = name;
    codec.type = std::string("video");
    codec.priority = (index + 1) * 1000;
    codec.payloadType = 101 + 2 * index;
    codec.rtxPayloadType = 102 + 2 * index;
    codec.encode = encode;
    codec.decode = decode;
    return codec;
}

} // namespace

TEST_CASE("Voice heartbeat acknowledgements accept current and legacy timestamps", "[voice][heartbeat]")
{
    constexpr uint64_t timestamp = 1794443049135ULL;
    REQUIRE(HeartbeatAck::fromJson(nlohmann::json{{"t", timestamp}}).nonce == timestamp);
    REQUIRE(HeartbeatAck::fromJson(nlohmann::json(timestamp)).nonce == timestamp);
    REQUIRE(HeartbeatAck::fromJson(nlohmann::json(static_cast<double>(timestamp))).nonce == timestamp);
    constexpr uint64_t precise = 9007199254740993ULL;
    REQUIRE(HeartbeatAck::fromJson(nlohmann::json{{"t", precise}, {"seq_ack", 0}}).nonce == precise);
    REQUIRE(HeartbeatAck::fromJson(nlohmann::json(0)).nonce == 0);
}

TEST_CASE("Malformed voice heartbeat acknowledgements have no timestamp", "[voice][heartbeat]")
{
    for (const auto &text : {"null", "{}", R"({"t":null})", R"({"t":"123"})",
                             R"({"t":true})", R"({"t":{}})", "[]", "-1", "1.5",
                             "18446744073709551616.0"}) {
        INFO(text);
        REQUIRE_FALSE(HeartbeatAck::fromJson(parse(text)).nonce.has_value());
    }
}

TEST_CASE("identifyWithoutVideoOmitsVideoKeys", "[voice][payloads]")
{
    REQUIRE(identifyFor(100, 200).toJson().toBytes() ==
            std::string(R"({"server_id":"100","channel_id":"200","user_id":"300","session_id":"session","token":"token","max_dave_protocol_version":1})"));
}

TEST_CASE("identifyVoiceConnectionOffersOneCameraLayer", "[voice][payloads]")
{
    VoiceIdentifyData identify = identifyFor(100, 200);
    identify.video = true;
    identify.streams = std::vector<StreamDescription>{ offeredStream("video", "100", 100) };

    REQUIRE(identify.toJson().toBytes() ==
            std::string(R"({"server_id":"100","channel_id":"200","user_id":"300","session_id":"session","token":"token","max_dave_protocol_version":1,"video":true,"streams":[{"type":"video","rid":"100","quality":100}]})"));
}

TEST_CASE("selectProtocolAudioOnly", "[voice][payloads]")
{
    SelectProtocolData protocol;
    protocol.address = std::string("203.0.113.7");
    protocol.port = 50123;
    protocol.mode = std::string("aead_aes256_gcm_rtpsize");
    protocol.codecs = std::vector<Codec>{ Codec::opus() };

    REQUIRE(protocol.toJson().toBytes() ==
            std::string(R"({"protocol":"udp","data":{"address":"203.0.113.7","port":50123,"mode":"aead_aes256_gcm_rtpsize"},"address":"203.0.113.7","port":50123,"mode":"aead_aes256_gcm_rtpsize","codecs":[{"name":"opus","type":"audio","priority":1000,"payload_type":120}]})"));
}

TEST_CASE("selectProtocolWithVideoCodecs", "[voice][payloads]")
{
    SelectProtocolData protocol;
    protocol.address = std::string("203.0.113.7");
    protocol.port = 50123;
    protocol.mode = std::string("aead_xchacha20_poly1305_rtpsize");
    protocol.codecs = std::vector<Codec>{ Codec::opus(), videoCodec("H264", 0, false, true), videoCodec("VP8", 1, false, true) };
    protocol.rtcConnectionId = std::string("0b0f3c2e-8f5c-4d5e-9a57-3c1f6a2d9e10");
    protocol.experiments = std::vector<std::string>{ std::string("fixed_keyframe_interval") };

    REQUIRE(protocol.toJson().toBytes() ==
            std::string(R"({"protocol":"udp","data":{"address":"203.0.113.7","port":50123,"mode":"aead_xchacha20_poly1305_rtpsize"},"address":"203.0.113.7","port":50123,"mode":"aead_xchacha20_poly1305_rtpsize","codecs":[{"name":"opus","type":"audio","priority":1000,"payload_type":120},{"name":"H264","type":"video","priority":1000,"payload_type":101,"rtx_payload_type":102,"encode":false,"decode":true},{"name":"VP8","type":"video","priority":2000,"payload_type":103,"rtx_payload_type":104,"encode":false,"decode":true}],"rtc_connection_id":"0b0f3c2e-8f5c-4d5e-9a57-3c1f6a2d9e10","experiments":["fixed_keyframe_interval"]})"));
}

TEST_CASE("localVideoStateKeyOrder", "[voice][payloads]")
{
    StreamResolution resolution;
    resolution.type = std::string("fixed");
    resolution.width = 1280;
    resolution.height = 720;

    StreamDescription stream;
    stream.type = std::string("video");
    stream.rid = std::string("100");
    stream.ssrc = 4001u;
    stream.active = true;
    stream.quality = 100;
    stream.rtxSsrc = 4002u;
    stream.maxBitrate = 2500000;
    stream.maxFramerate = 30;
    stream.maxResolution = resolution;

    LocalVideoState state;
    state.audioSsrc = 4000u;
    state.videoSsrc = 4001u;
    state.rtxSsrc = 4002u;
    state.streams = std::vector<StreamDescription>{ stream };

    REQUIRE(state.toJson().toBytes() ==
            std::string(R"({"audio_ssrc":4000,"video_ssrc":4001,"rtx_ssrc":4002,"streams":[{"type":"video","rid":"100","ssrc":4001,"active":true,"quality":100,"rtx_ssrc":4002,"max_bitrate":2500000,"max_framerate":30,"max_resolution":{"type":"fixed","width":1280,"height":720}}]})"));
}

TEST_CASE("localVideoStateCameraOff", "[voice][payloads]")
{
    LocalVideoState state;
    state.audioSsrc = 4000u;
    state.videoSsrc = 0u;
    state.rtxSsrc = 0u;

    REQUIRE(state.toJson().toBytes() == std::string(R"({"audio_ssrc":4000,"video_ssrc":0,"rtx_ssrc":0,"streams":[]})"));
}

TEST_CASE("sinkWantsPutSsrcsBeforeAny", "[voice][payloads]")
{
    MediaSinkWants wants;
    wants.any = 50;
    wants.bySsrc.emplace(9000, 100);
    wants.bySsrc.emplace(2000, 0);

    REQUIRE(wants.toJson().toBytes() == std::string(R"({"2000":0,"9000":100,"any":50})"));
}

TEST_CASE("sinkWantsForStreamViewer", "[voice][payloads]")
{
    MediaSinkWants wants;
    wants.bySsrc.emplace(7001, 100);
    wants.pixelCounts.emplace(7001, 1920 * 1080);
    REQUIRE(wants.toJson().toBytes() == std::string(R"({"7001":100,"pixelCounts":{"7001":2073600}})"));

    MediaSinkWants hidden;
    hidden.bySsrc.emplace(7001, 0);
    REQUIRE(hidden.toJson().toBytes() == std::string(R"({"7001":0})"));
}

TEST_CASE("sinkWantsParse", "[voice][payloads]")
{
    const MediaSinkWants wants = MediaSinkWants::fromJson(parse(R"({"any":70,"4001":100,"4003":0,"pixelCounts":{"4001":921600}})"));

    REQUIRE(wants.any.value_or(-1) == 70);
    REQUIRE(wants.bySsrc.size() == 2);
    REQUIRE(wants.bySsrc.at(4001) == 100);
    REQUIRE(wants.bySsrc.at(4003) == 0);
    REQUIRE(wants.pixelCounts.at(4001) == 921600);

    const MediaSinkWants bare = MediaSinkWants::fromJson(parse(R"({"4001":30})"));
    REQUIRE(!bare.any.has_value());
    REQUIRE(bare.bySsrc.at(4001) == 30);
    REQUIRE(bare.pixelCounts.empty());
}

TEST_CASE("videoFromServerParsesUserSsrcs", "[voice][payloads]")
{
    const ClientConnectData video = ClientConnectData::fromJson(parse(R"({"user_id":"123456789012345678","audio_ssrc":5000,"video_ssrc":5001,"streams":[{"type":"video","rid":"100","ssrc":5001,"rtx_ssrc":5002,"active":true,"quality":100,"max_bitrate":2500000,"max_framerate":30,"max_resolution":{"type":"fixed","width":1280,"height":720}}]})"));

    REQUIRE(static_cast<uint64_t>(video.userId.get()) == 123456789012345678ULL);
    REQUIRE(video.audioSsrc.get() == 5000u);
    REQUIRE(video.videoSsrc.get() == 5001u);
    REQUIRE(video.streams.hasValue());
    REQUIRE(video.streams->size() == 1);

    const StreamDescription &stream = video.streams->at(0);
    REQUIRE(stream.active.get() == true);
    REQUIRE(stream.maxBitrate.get() == 2500000);
    REQUIRE(stream.maxFramerate.get() == 30);
    REQUIRE(stream.maxResolution.hasValue());
    REQUIRE(stream.maxResolution->type.get() == std::string("fixed"));
    REQUIRE(stream.maxResolution->width.get() == 1280);
    REQUIRE(stream.maxResolution->height.get() == 720);

    // Camera off: the same message with a zero video SSRC and no streams.
    const ClientConnectData off = ClientConnectData::fromJson(parse(R"({"user_id":"123456789012345678","audio_ssrc":5000,"video_ssrc":0})"));
    REQUIRE(off.videoSsrc.get() == 0u);
    REQUIRE(!off.streams.hasValue());
}

TEST_CASE("sessionUpdateParsesOnlyWhatIsPresent", "[voice][payloads]")
{
    const SessionUpdate codecChange = SessionUpdate::fromJson(parse(R"({"video_codec":"VP8"})"));
    REQUIRE(codecChange.videoCodec.get() == std::string("VP8"));
    REQUIRE(!codecChange.audioCodec.hasValue());
    REQUIRE(!codecChange.mediaSessionId.hasValue());
    REQUIRE(!codecChange.keyframeInterval.hasValue());

    const SessionUpdate interval = SessionUpdate::fromJson(parse(R"({"keyframe_interval":5000})"));
    REQUIRE(interval.keyframeInterval.get() == 5000);
    REQUIRE(!interval.videoCodec.hasValue());
}

TEST_CASE("voiceStateUpdateSaysWhetherTheCameraIsOn", "[voice][payloads]")
{
    using Discord::voiceStateUpdate;
    const Core::Snowflake guild(100);
    const Core::Snowflake channel(200);

    REQUIRE(voiceStateUpdate(guild, channel, false, false, true).toJson().toBytes() ==
            std::string(R"({"op":4,"d":{"guild_id":"100","channel_id":"200","self_mute":false,"self_deaf":false,"self_video":true}})"));
    // Off is said as well, not left out.
    REQUIRE(voiceStateUpdate(guild, channel, true, true, false).toJson().toBytes() ==
            std::string(R"({"op":4,"d":{"guild_id":"100","channel_id":"200","self_mute":true,"self_deaf":true,"self_video":false}})"));
}

TEST_CASE("voiceStateUpdateForAPrivateCallAndForLeaving", "[voice][payloads]")
{
    using Discord::voiceStateUpdate;

    REQUIRE(voiceStateUpdate(Core::Snowflake::Invalid, Core::Snowflake(200), false, false, true).toJson().toBytes() ==
            std::string(R"({"op":4,"d":{"guild_id":null,"channel_id":"200","self_mute":false,"self_deaf":false,"self_video":true}})"));
    REQUIRE(voiceStateUpdate(Core::Snowflake(100), Core::Snowflake::Invalid, false, false, false).toJson().toBytes() ==
            std::string(R"({"op":4,"d":{"guild_id":"100","channel_id":null,"self_mute":false,"self_deaf":false,"self_video":false}})"));
}

TEST_CASE("voiceStateUpdateKeyOrderWithFlags", "[voice][payloads]")
{
    Discord::UpdateVoiceState update = Discord::voiceStateUpdate(Core::Snowflake(100), Core::Snowflake(200), false, false, true);
    update.flags = Discord::VoiceFlags(Discord::VoiceFlag::ALLOW_VOICE_RECORDINGS);

    REQUIRE(update.toJson().toBytes() ==
            std::string(R"({"op":4,"d":{"guild_id":"100","channel_id":"200","self_mute":false,"self_deaf":false,"self_video":true,"flags":2}})"));
}
