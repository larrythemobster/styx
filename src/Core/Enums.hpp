#pragma once

#include <QMetaType>

namespace Styx {
namespace Core {

enum class ConnectionState {
    Disconnecting,
    Disconnected,
    Connecting,
    Connected,
};

} // namespace Core
} // namespace Styx

Q_DECLARE_METATYPE(Styx::Core::ConnectionState)