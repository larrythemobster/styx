#pragma once

#include "Core/Result.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/ApplicationCommands.hpp"

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace Styx::Discord {

// A filled-in option of the command being sent.
struct CommandOptionValue
{
    std::string name;
    ApplicationCommandOptionType type = ApplicationCommandOptionType::STRING;
    nlohmann::json value;
};

// Turns what the user typed for an option into the value Discord expects, or an error
// to show next to it:
// - choices: the text must equal a choice's name or (as text) its value; the choice's
//   own value is used, whatever its type;
// - STRING: min/max_length in Unicode code points (default 1 to 6000);
// - INTEGER: a whole number within min/max_value and +-2^53;
// - NUMBER: a finite number within min/max_value and +-2^53;
// - BOOLEAN: "true" or "false", ignoring case;
// - USER, CHANNEL, ROLE, MENTIONABLE: a snowflake, sent as a string;
// - ATTACHMENT and subcommands: refused.
// Surrounding whitespace is trimmed first; empty text is an error.
[[nodiscard]] Core::Result<nlohmann::json> commandOptionValue(const ApplicationCommandOption &option,
                                                              std::string_view text);

// The leaf command being run and its filled-in options.
struct ApplicationCommandRequest
{
    Core::Snowflake applicationId;
    Core::Snowflake guildId; // invalid outside servers
    Core::Snowflake channelId;
    Core::Snowflake commandId;
    Core::Snowflake commandVersion;
    std::string commandName;
    // Group and/or subcommand names, outermost first: one name is a subcommand, two are a
    // group and its subcommand.
    std::vector<std::string> subcommandPath;
    // The leaf's declared options, to check required ones and reject unknown names.
    std::vector<ApplicationCommandOption> declaredOptions;
    std::vector<CommandOptionValue> values;
    std::string nonce;
};

// The POST /interactions body for an application command (type 2): data carries id,
// type 1, name, version and options, with the values nested inside the subcommand and
// group options. Fails when a required option has no value, a value names an option the
// leaf doesn't declare or repeats one, the path is longer than two, or the session,
// nonce, application ID, channel ID, command ID, name or version is missing.
[[nodiscard]] Core::Result<nlohmann::json> applicationCommandBody(const ApplicationCommandRequest &request,
                                                                  const std::string &sessionId);

} // namespace Styx::Discord
