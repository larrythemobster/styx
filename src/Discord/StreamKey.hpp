// Ported from ouwou upstream/video-streams (71b4907).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include "Core/Snowflake.hpp"

namespace Styx::Discord {
struct StreamKey {
    enum class Type { Guild, Call };
    Type type = Type::Guild;
    Core::Snowflake guildId;
    Core::Snowflake channelId;
    Core::Snowflake ownerId;
    [[nodiscard]] static StreamKey forChannel(Core::Snowflake guildId, Core::Snowflake channelId, Core::Snowflake ownerId);
    [[nodiscard]] static std::optional<StreamKey> parse(const std::string &key);
    [[nodiscard]] std::string toString() const;
    [[nodiscard]] std::string typeName() const;
    bool operator==(const StreamKey &other) const = default;
};
struct StreamPreviewLookup {
    std::string url;
    int statusCode = 0;
    int64_t retryAfterMs = 0;
};
} // namespace Styx::Discord
