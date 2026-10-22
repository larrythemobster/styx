#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace Styx::Discord::Voice {

// One encoding sent by a video participant. Ported from ouwou/Acheron video-streams.
struct VideoLayer {
    std::uint32_t ssrc = 0;
    std::uint32_t rtxSsrc = 0;
    int quality = 100;
    bool operator==(const VideoLayer &) const = default;
};
struct VideoSender {
    std::uint64_t userId = 0;
    std::vector<VideoLayer> layers;
    bool wanted = true;
    int tilePixels = 0;
};
struct SinkWantsInput {
    std::vector<VideoSender> senders;
    std::uint64_t focusedUser = 0;
    bool oneToOne = false;
};
// The Qt-free payload representation for voice opcode 15. Serialization belongs at the
// gateway boundary, where it can use the protocol's JSON representation.
struct SinkWantsPayload {
    std::optional<int> any;
    std::map<std::uint32_t, int> bySsrc;
    bool operator==(const SinkWantsPayload &) const = default;
};
struct SinkWantsPlan {
    SinkWantsPayload wants;
    std::map<std::uint64_t, std::uint32_t> deliver;
};
[[nodiscard]] int sinkWantLevel(int videoSenders, int tilePixels = 0);
[[nodiscard]] SinkWantsPlan planSinkWants(const SinkWantsInput &input);

} // namespace Styx::Discord::Voice
