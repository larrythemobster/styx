// Ported from ouwou upstream/video-streams (71b4907).
#include "StreamKey.hpp"

#include <charconv>
#include <string_view>
#include <vector>

namespace Styx::Discord {
namespace {
std::optional<Core::Snowflake> parseId(std::string_view text)
{
    uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value == 0)
        return std::nullopt;
    return Core::Snowflake(value);
}
}

StreamKey StreamKey::forChannel(Core::Snowflake guildId, Core::Snowflake channelId, Core::Snowflake ownerId)
{
    StreamKey key;
    key.type = guildId.isValid() ? Type::Guild : Type::Call;
    if (guildId.isValid()) key.guildId = guildId;
    key.channelId = channelId;
    key.ownerId = ownerId;
    return key;
}

std::optional<StreamKey> StreamKey::parse(const std::string &key)
{
    std::vector<std::string_view> parts;
    std::string_view rest(key);
    for (;;) {
        const auto pos = rest.find(':');
        parts.push_back(rest.substr(0, pos));
        if (pos == std::string_view::npos) break;
        rest.remove_prefix(pos + 1);
    }
    StreamKey parsed;
    size_t index = 1;
    if (parts.size() == 4 && parts[0] == "guild") {
        parsed.type = Type::Guild;
        const auto id = parseId(parts[index++]);
        if (!id) return std::nullopt;
        parsed.guildId = *id;
    } else if (parts.size() == 3 && parts[0] == "call") {
        parsed.type = Type::Call;
    } else return std::nullopt;
    const auto channel = parseId(parts[index++]);
    const auto owner = parseId(parts[index]);
    if (!channel || !owner) return std::nullopt;
    parsed.channelId = *channel;
    parsed.ownerId = *owner;
    return parsed;
}

std::string StreamKey::toString() const
{
    if (type == Type::Guild)
        return "guild:" + guildId.toString() + ":" + channelId.toString() + ":" + ownerId.toString();
    return "call:" + channelId.toString() + ":" + ownerId.toString();
}

std::string StreamKey::typeName() const
{
    return type == Type::Guild ? "guild" : "call";
}
}
