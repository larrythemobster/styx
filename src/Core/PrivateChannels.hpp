#pragma once

#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"

namespace Styx::Core {

[[nodiscard]] Discord::Channel mergePrivateChannel(Discord::Channel baseline, const Discord::Channel &update);
[[nodiscard]] Discord::User mergePrivateUser(Discord::User baseline, const Discord::User &update);
bool applyPrivateRecipient(Discord::Channel &channel, Snowflake userId, bool added);

} // namespace Styx::Core
