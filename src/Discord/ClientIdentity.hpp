#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "Objects.hpp"

namespace Styx {
namespace Discord {

struct ClientPropertiesBuildParams
{
    bool includeClientHeartbeatSessionId;
    std::optional<bool> isFastConnect;
    std::optional<std::string> gatewayConnectReasons;
};

struct HeartbeatSession
{
    std::string id;
    int64_t createdAtMs = 0;
    int64_t lastUsedAtMs = 0;
};

enum class HeartbeatSessionUpdate {
    Unchanged,
    Touched,
    Created,
};

class ClientIdentity
{
public:
    ClientIdentity();

    std::string clientLaunchId() const;

    std::optional<HeartbeatSession> heartbeatSession() const;
    void restoreHeartbeatSession(const std::optional<HeartbeatSession> &stored);
    HeartbeatSessionUpdate touchHeartbeatSession();

    void setDiscordLocale(const std::string &locale);
    std::string discordLocale() const;

    void setActivity(bool focused, bool rtcConnected);

    ClientProperties buildClientProperties(const ClientPropertiesBuildParams &params) const;

private:
    static std::string generateLaunchSignature();

    mutable std::mutex mutex;
    std::string launchId;
    std::string launchSignature;
    std::optional<HeartbeatSession> clientHeartbeatSession;
    std::string locale = "en-US";
    bool appFocused = true;
    bool rtcConnected = false;
};

} // namespace Discord
} // namespace Styx
