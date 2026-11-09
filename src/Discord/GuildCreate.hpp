#pragma once

#include "Core/OrderedJson.hpp"

#include <string>

namespace Styx::Discord {

// The POST /guilds body the official client sends for "Create My Own": the blank server
// template, no channels of its own, and the icon as a data URI or null.
[[nodiscard]] Core::OrderedJson guildCreateBody(const std::string &name, const std::string &iconDataUri);

} // namespace Styx::Discord
