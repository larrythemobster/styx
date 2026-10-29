// Ported from ouwou/acheron upstream/video-streams (commit 71b4907) src/Core/Calls/CallStore.hpp
#pragma once

#include <optional>
#include <string>
#include <unordered_map>

#include "Core/Snowflake.hpp"
#include "Platform/Signal.hpp"

namespace Styx {
namespace Discord {
struct CallCreate;
struct CallUpdate;
struct CallDelete;
} // namespace Discord

namespace Core {

// What the gateway has said about the call in one DM or group DM.
struct CallRecord {
    Snowflake channelId;
    Snowflake messageId;
    // Unset both before the gateway has answered and after the call has ended.
    std::optional<std::string> region;
    // Who is being rung, mapped to who rang them.
    std::unordered_map<Snowflake, Snowflake> ongoingRings;
    bool unavailable = false;

    [[nodiscard]] bool isActive() const { return !unavailable && region.has_value(); }
};

// The private calls this account's gateway session knows about, kept the way the official
// client's CallStore keeps them. Calls are never part of READY: they arrive as CALL_CREATE, on
// connect and in answer to CALL_CONNECT (opcode 13).
class CallStore {
public:
    CallStore() = default;

    // Whether to ask the gateway about a private channel's call now that the channel is being
    // looked at. True once per channel, or every time when forced (a fresh or resumed session);
    // the channel is remembered either way so that looking at it again does not ask again.
    [[nodiscard]] bool shouldConnect(Snowflake channelId, bool force);

    void handleCallCreate(const Discord::CallCreate &event);
    void handleCallUpdate(const Discord::CallUpdate &event);
    void handleCallDelete(const Discord::CallDelete &event);
    void forget(Snowflake channelId);
    void reset();

    [[nodiscard]] std::optional<CallRecord> call(Snowflake channelId) const;
    [[nodiscard]] bool isActive(Snowflake channelId) const;
    [[nodiscard]] bool isRinging(Snowflake channelId, Snowflake userId) const;

    Platform::Signal<Snowflake> callChanged;

private:
    std::unordered_map<Snowflake, CallRecord> calls;
};

} // namespace Core
} // namespace Styx
