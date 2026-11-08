#include "CallSounds.hpp"

#include "Core/Audio/Sounds.hpp"
#include "Core/Calls/CallStore.hpp"
#include "Core/ClientInstance.hpp"
#ifndef STYX_NO_VOICE
#include "Core/Audio/VoiceManager.hpp"
#endif

#include <algorithm>
#include <chrono>

namespace Styx::UI {
namespace {
// Styx can't decline a call yet, so a ring the server never clears stops on its own.
constexpr auto RING_LIMIT = std::chrono::seconds(60);
using Core::Sounds::Sound;
}

CallSounds::~CallSounds()
{
    stopRinging();
}

void CallSounds::watch(Core::ClientInstance *client)
{
    track(client->calls()->callChanged.connect([this, client](Core::Snowflake channelId) { updateRing(client, channelId); }));
    track(client->voiceStateChanged.connect([this, client](Core::Snowflake channelId, Core::Snowflake) {
        auto &previous = voiceChannels[client->accountId()];
        if (channelId != previous)
            Core::Sounds::play(channelId.isValid() ? Sound::Join : Sound::Leave);
        previous = channelId;
        // Joining a call that's ringing answers it.
        if (channelId.isValid())
            setRinging(client->accountId(), channelId, false);
    }));
#ifndef STYX_NO_VOICE
    if (auto *voice = client->voice()) {
        track(voice->someoneJoined.connect([](Core::Snowflake) { Core::Sounds::play(Sound::Join); }));
        track(voice->someoneLeft.connect([](Core::Snowflake) { Core::Sounds::play(Sound::Leave); }));
    }
#endif
}

void CallSounds::forget(Core::Snowflake accountId)
{
    voiceChannels.erase(accountId);
    std::erase_if(ringing, [accountId](const auto &entry) { return entry.first == accountId; });
    if (ringing.empty())
        stopRinging();
}

void CallSounds::updateRing(Core::ClientInstance *client, Core::Snowflake channelId)
{
    // Do Not Disturb silences incoming calls, as the official client does.
    const bool rung = client->calls()->isRinging(channelId, client->accountId()) && client->voiceChannelId() != channelId &&
                      client->presences()->selfStatus() != Discord::StatusType::DND;
    setRinging(client->accountId(), channelId, rung);
}

void CallSounds::setRinging(Core::Snowflake accountId, Core::Snowflake channelId, bool rung)
{
    const auto entry = std::make_pair(accountId, channelId);
    const auto it = std::find(ringing.begin(), ringing.end(), entry);
    if (rung == (it != ringing.end()))
        return;
    if (!rung) {
        ringing.erase(it);
        if (ringing.empty())
            stopRinging();
        return;
    }
    ringing.push_back(entry);
    if (ringing.size() > 1)
        return;
    Core::Sounds::startLoop(Sound::Ring);
    ringTimeout = Platform::Dispatcher::main().setTimer(std::chrono::duration_cast<std::chrono::milliseconds>(RING_LIMIT), false, [this] {
        ringTimeout = 0;
        ringing.clear();
        stopRinging();
    });
}

void CallSounds::stopRinging()
{
    if (ringTimeout)
        Platform::Dispatcher::main().cancelTimer(ringTimeout);
    ringTimeout = 0;
    Core::Sounds::stopLoop(Sound::Ring);
}

} // namespace Styx::UI
