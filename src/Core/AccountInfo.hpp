#pragma once

#include <format>
#include <string>

#include "Enums.hpp"
#include "ProxyConfig.hpp"
#include "Snowflake.hpp"

namespace Styx::Core {
struct AccountInfo
{
    Snowflake id;
    std::string username;
    std::string displayName;
    std::string token;
    std::string avatar;

    std::string gatewayUrl = "wss://gateway.discord.gg/?encoding=json&v=9&compress=zlib-stream";
    std::string restUrl = "https://discord.com/api/v9";
    std::string cdnUrl = "https://cdn.discordapp.com";

    ProxyConfig proxy;
    std::string proxyError;

    int displayOrder = 0;
    bool autoConnect = false;
    ConnectionState state = ConnectionState::Disconnected;

    [[nodiscard]] std::string getEntryString() const
    {
        if (displayName.empty())
            return username;
        return std::format("{} ({})", displayName, username);
    }
};
}
