#pragma once

#include "Core/Result.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Enums.hpp"

#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace Styx::Discord {

struct ComponentInteractionRequest
{
    Core::Snowflake applicationId;
    Core::Snowflake guildId;
    Core::Snowflake channelId;
    Core::Snowflake messageId;
    std::uint64_t messageFlags = 0;
    MessageComponentType componentType = MessageComponentType::BUTTON;
    std::string customId;
    std::string nonce;
    std::vector<std::string> values;
};

[[nodiscard]] Core::Result<nlohmann::json> componentInteractionBody(
    const ComponentInteractionRequest &request,
    const std::string &sessionId);

} // namespace Styx::Discord
