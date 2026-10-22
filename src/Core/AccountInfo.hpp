#pragma once

#include <QString>
#include <QPixmap>
#include <QMetaType>

#include "Enums.hpp"
#include "ProxyConfig.hpp"
#include "Snowflake.hpp"

namespace Styx {
namespace Core {
struct AccountInfo
{
    Snowflake id;
    QString username;
    QString displayName;
    QString token;
    QString avatar;

    QString gatewayUrl = "wss://gateway.discord.gg/?encoding=json&v=9&compress=zlib-stream";
    QString restUrl = "https://discord.com/api/v9";
    QString cdnUrl = "https://cdn.discordapp.com";

    ProxyConfig proxy;

    int displayOrder = 0;
    bool autoConnect = false;

    ConnectionState state = ConnectionState::Disconnected;

    [[nodiscard]] QString getEntryString() const
    {
        if (displayName.isEmpty())
            return username;

        return QString("%1 (%2)").arg(displayName).arg(username);
    }
};
} // namespace Core
} // namespace Styx

Q_DECLARE_METATYPE(Styx::Core::AccountInfo)
