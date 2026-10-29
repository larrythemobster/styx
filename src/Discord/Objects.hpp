#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <optional>

#include "Core/JsonUtils.hpp"
#include "Core/OrderedJson.hpp"

#ifdef compress // really bruh
#undef compress
#endif

namespace Styx {
namespace Discord {

struct ClientProperties : Styx::Core::JsonUtils::JsonObject
{
    Field<std::string> os;
    Field<std::string> browser;
    Field<std::string> device;
    Field<std::string> systemLocale;
    Field<bool> hasClientMods;
    Field<std::string> browserUserAgent;
    Field<std::string> browserVersion;
    Field<std::string> osVersion;
    Field<std::string> referrer;
    Field<std::string> referringDomain;
    Field<std::string> referrerCurrent;
    Field<std::string> referringDomainCurrent;
    Field<std::string> releaseChannel;
    Field<int> clientBuildNumber;
    Field<std::string, false, true> clientEventSource;
    Field<std::string> clientLaunchId;
    Field<std::string> launchSignature;
    Field<std::string> clientAppState;
    Field<bool, true> isFastConnect;
    Field<std::string, true> gatewayConnectReasons;
    Field<std::string, true> clientHeartbeatSessionId;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "os", os);
        insert(obj, "browser", browser);
        insert(obj, "device", device);
        insert(obj, "system_locale", systemLocale);
        insert(obj, "has_client_mods", hasClientMods);
        insert(obj, "browser_user_agent", browserUserAgent);
        insert(obj, "browser_version", browserVersion);
        insert(obj, "os_version", osVersion);
        insert(obj, "referrer", referrer);
        insert(obj, "referring_domain", referringDomain);
        insert(obj, "referrer_current", referrerCurrent);
        insert(obj, "referring_domain_current", referringDomainCurrent);
        insert(obj, "release_channel", releaseChannel);
        insert(obj, "client_build_number", clientBuildNumber);
        insert(obj, "client_event_source", clientEventSource);
        insert(obj, "client_launch_id", clientLaunchId);
        insert(obj, "launch_signature", launchSignature);
        insert(obj, "client_app_state", clientAppState);
        insert(obj, "is_fast_connect", isFastConnect);
        insert(obj, "gateway_connect_reasons", gatewayConnectReasons);
        insert(obj, "client_heartbeat_session_id", clientHeartbeatSessionId);

        return obj;
    }
};

struct UpdatePresence : Styx::Core::JsonUtils::JsonObject
{
    Field<std::string> status;
    Field<int> since;
    Field<bool> afk;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "status", status);
        insert(obj, "since", since);
        obj.insert("activities", nlohmann::json());
        insert(obj, "afk", afk);
        return obj;
    }
};

struct ClientState : Styx::Core::JsonUtils::JsonObject
{
    // Map guild_versions;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        obj.insert("guild_versions", Core::OrderedJson());
        return obj;
    }
};

} // namespace Discord
} // namespace Styx
