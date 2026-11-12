#pragma once

#include "Core/Snowflake.hpp"
#include "Platform/Dispatcher.hpp"
#include "Platform/Trackable.hpp"

#include <unordered_map>
#include <utility>
#include <vector>

namespace Styx {
namespace Core { class ClientInstance; }
namespace UI {

// Rings while a private call is ringing the user, and plays join and leave sounds for the voice
// channel they're in, for every connected account.
class CallSounds : public Platform::Trackable
{
public:
    CallSounds();
    ~CallSounds() override;
    void watch(Core::ClientInstance *client);
    void forget(Core::Snowflake accountId);

private:
    void updateRing(Core::ClientInstance *client, Core::Snowflake channelId);
    void setRinging(Core::Snowflake accountId, Core::Snowflake channelId, bool rung);
    void stopRinging();

    // (account, channel) pairs whose call is ringing the user.
    std::vector<std::pair<Core::Snowflake, Core::Snowflake>> ringing;
    std::unordered_map<Core::Snowflake, Core::Snowflake> voiceChannels;
    Platform::TimerId ringTimeout = 0;
};

} // namespace UI
} // namespace Styx
