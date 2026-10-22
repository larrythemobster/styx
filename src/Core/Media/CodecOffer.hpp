// Ported from ouwou upstream/video-streams (71b4907); native codec-offer value.
#pragma once

#include <optional>
#include <string>

namespace Styx::Core::Media {
// SELECT_PROTOCOL codec offer. All fields are populated for video offers.
struct CodecOffer {
    std::string name;
    std::string type;
    int priority = 0;
    int payloadType = 0;
    std::optional<int> rtxPayloadType;
    std::optional<bool> encode;
    std::optional<bool> decode;
};
} // namespace Styx::Core::Media
