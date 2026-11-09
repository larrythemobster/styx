#include "ComponentInteraction.hpp"

#include <utility>

namespace Styx::Discord {

Core::Result<nlohmann::json> componentInteractionBody(const ComponentInteractionRequest &request,
                                                      const std::string &sessionId)
{
    if (sessionId.empty())
        return Core::Result<nlohmann::json>::makeError("Session ID is required");
    if (request.nonce.empty())
        return Core::Result<nlohmann::json>::makeError("Nonce is required");
    if (request.customId.empty())
        return Core::Result<nlohmann::json>::makeError("Custom ID is required");
    if (!request.applicationId.isValid())
        return Core::Result<nlohmann::json>::makeError("Application ID is required");
    if (!request.channelId.isValid())
        return Core::Result<nlohmann::json>::makeError("Channel ID is required");
    if (!request.messageId.isValid())
        return Core::Result<nlohmann::json>::makeError("Message ID is required");

    bool isSelect = false;
    switch (request.componentType) {
    case MessageComponentType::BUTTON:
        break;
    case MessageComponentType::STRING_SELECT:
    case MessageComponentType::USER_SELECT:
    case MessageComponentType::ROLE_SELECT:
    case MessageComponentType::MENTIONABLE_SELECT:
    case MessageComponentType::CHANNEL_SELECT:
        isSelect = true;
        break;
    default:
        return Core::Result<nlohmann::json>::makeError("Unsupported component type");
    }

    nlohmann::json body;
    body["type"] = 3;
    body["application_id"] = request.applicationId.toString();
    body["channel_id"] = request.channelId.toString();
    body["message_id"] = request.messageId.toString();
    body["session_id"] = sessionId;
    body["nonce"] = request.nonce;
    body["message_flags"] = request.messageFlags;
    if (request.guildId.isValid())
        body["guild_id"] = request.guildId.toString();

    nlohmann::json data;
    data["component_type"] = static_cast<int>(request.componentType);
    data["custom_id"] = request.customId;
    if (isSelect)
        data["values"] = request.values;
    body["data"] = std::move(data);

    return Core::Result<nlohmann::json>::makeOk(std::move(body));
}

} // namespace Styx::Discord
