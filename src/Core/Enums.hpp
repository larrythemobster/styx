#pragma once

namespace Styx::Core {

enum class ConnectionState {
    Disconnecting,
    Disconnected,
    Connecting,
    Connected,
};

}
