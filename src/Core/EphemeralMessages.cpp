#include "EphemeralMessages.hpp"

#include <utility>

namespace Styx {
namespace Core {

bool EphemeralMessages::put(Discord::Message message)
{
    if (!message.id.get().isValid() || !message.channelId.get().isValid() || !message.isEphemeral() || isDismissed(message.id))
        return false;

    const Snowflake id = message.id;
    messages.insert_or_assign(id, std::move(message));
    return true;
}

const Discord::Message *EphemeralMessages::find(Snowflake id) const
{
    const auto it = messages.find(id);
    if (it == messages.end())
        return nullptr;
    return &it->second;
}

bool EphemeralMessages::isDismissed(Snowflake id) const
{
    return dismissed.contains(id);
}

bool EphemeralMessages::dismiss(Snowflake channelId, Snowflake messageId)
{
    const auto it = messages.find(messageId);
    if (it == messages.end() || it->second.channelId.get() != channelId)
        return false;

    messages.erase(it);
    dismissed.insert(messageId);
    return true;
}

void EphemeralMessages::appendForChannel(Snowflake channelId, std::vector<Discord::Message> &out) const
{
    for (const auto &[id, msg] : messages) {
        if (msg.channelId.get() == channelId)
            out.push_back(msg);
    }
}

} // namespace Core
} // namespace Styx
