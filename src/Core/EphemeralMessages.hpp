#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"

namespace Styx {
namespace Core {

class EphemeralMessages
{
public:
    bool put(Discord::Message message);
    [[nodiscard]] const Discord::Message *find(Snowflake id) const;
    [[nodiscard]] bool isDismissed(Snowflake id) const;
    bool dismiss(Snowflake channelId, Snowflake messageId);
    void appendForChannel(Snowflake channelId, std::vector<Discord::Message> &out) const;

private:
    std::unordered_map<Snowflake, Discord::Message> messages;
    std::unordered_set<Snowflake> dismissed;
};

} // namespace Core
} // namespace Styx
