#include "ClientIdentity.hpp"
#include "CurlUtils.hpp"

#include "Platform/Native.hpp"

#include <chrono>

namespace Styx {
namespace Discord {

namespace {
constexpr int64_t HeartbeatSessionIdleMs = 30 * 60 * 1000;

int64_t nowMillis()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool isExpired(const HeartbeatSession &session, int64_t nowMs)
{
    return nowMs - session.lastUsedAtMs >= HeartbeatSessionIdleMs;
}
} // namespace

ClientIdentity::ClientIdentity()
{
    launchId = Platform::randomUuid();
    launchSignature = generateLaunchSignature();
}

std::string ClientIdentity::clientLaunchId() const
{
    return launchId;
}

std::optional<HeartbeatSession> ClientIdentity::heartbeatSession() const
{
    std::lock_guard locker(mutex);
    if (!clientHeartbeatSession || isExpired(*clientHeartbeatSession, nowMillis()))
        return std::nullopt;
    return clientHeartbeatSession;
}

void ClientIdentity::restoreHeartbeatSession(const std::optional<HeartbeatSession> &stored)
{
    std::lock_guard locker(mutex);
    clientHeartbeatSession = stored;
}

HeartbeatSessionUpdate ClientIdentity::touchHeartbeatSession()
{
    std::lock_guard locker(mutex);
    int64_t now = nowMillis();
    bool expired = !clientHeartbeatSession || isExpired(*clientHeartbeatSession, now);

    if (!appFocused && !rtcConnected) {
        if (expired)
            clientHeartbeatSession.reset();

        return HeartbeatSessionUpdate::Unchanged;
    }

    if (expired) {
        HeartbeatSession session;
        session.id = Platform::randomUuid();
        session.createdAtMs = now;
        session.lastUsedAtMs = now;
        clientHeartbeatSession = session;
        return HeartbeatSessionUpdate::Created;
    }

    clientHeartbeatSession->lastUsedAtMs = now;
    return HeartbeatSessionUpdate::Touched;
}

void ClientIdentity::setDiscordLocale(const std::string &newLocale)
{
    std::lock_guard locker(mutex);
    if (!newLocale.empty())
        locale = newLocale;
}

std::string ClientIdentity::discordLocale() const
{
    std::lock_guard locker(mutex);
    return locale;
}

void ClientIdentity::setActivity(bool focused, bool newRtcConnected)
{
    std::lock_guard locker(mutex);
    appFocused = focused;
    rtcConnected = newRtcConnected;
}

std::string ClientIdentity::generateLaunchSignature()
{
    std::string uuid = Platform::randomUuid();
    static constexpr unsigned char mask[16] = {
        0xff, 0x7f, 0xef, 0xef, 0xf7, 0xef, 0xf7, 0xff,
        0xdf, 0x7e, 0xff, 0xbf, 0xfe, 0xff, 0xf7, 0xff
    };
    static constexpr char hex[] = "0123456789abcdef";
    size_t byte = 0;
    for (size_t i = 0; i + 1 < uuid.size();) {
        if (uuid[i] == '-') { ++i; continue; }
        auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        unsigned value = ((digit(uuid[i]) << 4) | digit(uuid[i + 1])) & mask[byte++];
        uuid[i] = hex[value >> 4];
        uuid[i + 1] = hex[value & 15];
        i += 2;
    }
    return uuid;
}

ClientProperties ClientIdentity::buildClientProperties(
        const ClientPropertiesBuildParams &params) const
{
    std::string userAgent = CurlUtils::getUserAgent();
    CurlUtils::UserAgentProps props = CurlUtils::getUserAgentProps();

    std::lock_guard locker(mutex);

    ClientProperties properties;
    properties.os = props.os;
    properties.browser = props.browser;
    properties.device = "";
    properties.systemLocale = CurlUtils::getSystemLocale();
    properties.hasClientMods = false;
    properties.browserUserAgent = userAgent;
    properties.browserVersion = props.browserVersion;
    properties.osVersion = props.osVersion;
    properties.referrer = "";
    properties.referringDomain = "";
    properties.referrerCurrent = "";
    properties.referringDomainCurrent = "";
    properties.releaseChannel = "stable";
    properties.clientBuildNumber = CurlUtils::getBuildNumber();
    properties.clientEventSource = nullptr;
    properties.clientLaunchId = launchId;
    properties.launchSignature = launchSignature;
    properties.clientAppState = appFocused ? "focused" : "unfocused";

    if (params.isFastConnect.has_value())
        properties.isFastConnect = params.isFastConnect.value();

    if (params.gatewayConnectReasons.has_value())
        properties.gatewayConnectReasons = params.gatewayConnectReasons.value();

    if (params.includeClientHeartbeatSessionId &&
        clientHeartbeatSession &&
        !isExpired(*clientHeartbeatSession, nowMillis()))
        properties.clientHeartbeatSessionId = clientHeartbeatSession->id;

    return properties;
}

} // namespace Discord
} // namespace Styx
