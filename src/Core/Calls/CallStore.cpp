// Ported from ouwou/acheron upstream/video-streams (commit 71b4907) src/Core/Calls/CallStore.cpp
#include "CallStore.hpp"

#include "Discord/Events.hpp"

#include <vector>

namespace Styx {
namespace Core {

bool CallStore::shouldConnect(Snowflake channelId, bool force)
{
    auto it = calls.find(channelId);
    if (it != calls.end())
        return force;

    CallRecord placeholder;
    placeholder.channelId = channelId;
    calls.emplace(channelId, placeholder);
    return true;
}

void CallStore::handleCallCreate(const Discord::CallCreate &event)
{
    CallRecord record;
    record.channelId = event.channelId;
    if (event.messageId.hasValue())
        record.messageId = event.messageId.get();
    if (event.region.has_value())
        record.region = event.region->toStdString();
    for (auto it = event.ongoingRings.cbegin(); it != event.ongoingRings.cend(); ++it) {
        record.ongoingRings.emplace(it.key(), it.value());
    }

    calls[record.channelId] = record;
    callChanged.publish(record.channelId);
}

void CallStore::handleCallUpdate(const Discord::CallUpdate &event)
{
    CallRecord &record = calls[event.channelId.get()];
    record.channelId = event.channelId;
    record.messageId = event.messageId.hasValue() ? event.messageId.get() : Snowflake::Invalid;
    if (event.region.has_value())
        record.region = event.region->toStdString();
    else
        record.region.reset();
    record.ongoingRings.clear();
    for (auto it = event.ongoingRings.cbegin(); it != event.ongoingRings.cend(); ++it) {
        record.ongoingRings.emplace(it.key(), it.value());
    }

    callChanged.publish(record.channelId);
}

void CallStore::handleCallDelete(const Discord::CallDelete &event)
{
    const Snowflake channelId = event.channelId;
    const bool unavailable = event.unavailable.valueOr(false);

    auto it = calls.find(channelId);
    if (unavailable && it != calls.end()) {
        it->second.unavailable = true;
    } else {
        // The call is over, but the channel stays known so opening the DM does not ask again.
        CallRecord ended;
        ended.channelId = channelId;
        ended.unavailable = unavailable;
        calls[channelId] = ended;
    }

    callChanged.publish(channelId);
}

void CallStore::forget(Snowflake channelId)
{
    if (calls.erase(channelId) > 0)
        callChanged.publish(channelId);
}

void CallStore::reset()
{
    std::vector<Snowflake> channelIds;
    channelIds.reserve(calls.size());
    for (const auto &[id, _] : calls)
        channelIds.push_back(id);
    calls.clear();
    for (Snowflake channelId : channelIds)
        callChanged.publish(channelId);
}

std::optional<CallRecord> CallStore::call(Snowflake channelId) const
{
    auto it = calls.find(channelId);
    if (it == calls.end())
        return std::nullopt;
    return it->second;
}

bool CallStore::isActive(Snowflake channelId) const
{
    auto it = calls.find(channelId);
    return it != calls.end() && it->second.isActive();
}

bool CallStore::isRinging(Snowflake channelId, Snowflake userId) const
{
    auto it = calls.find(channelId);
    if (it == calls.end() || !it->second.isActive())
        return false;
    return it->second.ongoingRings.find(userId) != it->second.ongoingRings.end();
}

} // namespace Core
} // namespace Styx
