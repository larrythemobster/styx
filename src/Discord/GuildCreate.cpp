#include "GuildCreate.hpp"

namespace Styx::Discord {

Core::OrderedJson guildCreateBody(const std::string &name, const std::string &iconDataUri)
{
    Core::OrderedJson body;
    body.insert("name", name);
    if (iconDataUri.empty())
        body.insert("icon", nullptr);
    else
        body.insert("icon", iconDataUri);
    body.insert("channels", Core::OrderedJson::Array());
    body.insert("system_channel_id", nullptr);
    body.insert("guild_template_code", "2TffvPucqHkN");
    return body;
}

} // namespace Styx::Discord
