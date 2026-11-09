#pragma once

#include "Core/Snowflake.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Styx::Discord {

enum class ApplicationCommandType : int {
    CHAT_INPUT = 1,
    USER = 2,
    MESSAGE = 3,
    PRIMARY_ENTRY_POINT = 4,
};

enum class ApplicationCommandOptionType : int {
    SUB_COMMAND = 1,
    SUB_COMMAND_GROUP = 2,
    STRING = 3,
    INTEGER = 4,
    BOOLEAN = 5,
    USER = 6,
    CHANNEL = 7,
    ROLE = 8,
    MENTIONABLE = 9,
    NUMBER = 10,
    ATTACHMENT = 11,
};

struct ApplicationCommandChoice
{
    std::string name;
    // A string, integer or float, sent back to the bot unchanged.
    nlohmann::json value;
};

struct ApplicationCommandOption
{
    ApplicationCommandOptionType type = ApplicationCommandOptionType::STRING;
    std::string name;
    std::string description;
    bool required = false;
    bool autocomplete = false;
    std::vector<ApplicationCommandChoice> choices;
    std::vector<ApplicationCommandOption> options;
    std::vector<int> channelTypes;
    std::optional<double> minValue;
    std::optional<double> maxValue;
    std::optional<int> minLength;
    std::optional<int> maxLength;
};

struct ApplicationCommand
{
    Core::Snowflake id;
    ApplicationCommandType type = ApplicationCommandType::CHAT_INPUT;
    Core::Snowflake applicationId;
    // Invalid for global commands.
    Core::Snowflake guildId;
    std::string name;
    std::string description;
    std::vector<ApplicationCommandOption> options;
    Core::Snowflake version;
    bool nsfw = false;
    // Interaction contexts (0 guild, 1 bot DM, 2 private channel); empty when the bot
    // didn't restrict them.
    std::vector<int> contexts;
};

struct CommandIndexApplication
{
    Core::Snowflake id;
    std::string name;
    std::string description;
    std::string icon; // empty when the application has no icon
    Core::Snowflake botId;
};

struct ApplicationCommandIndex
{
    std::vector<CommandIndexApplication> applications;
    std::vector<ApplicationCommand> commands;
    Core::Snowflake version;
};

// Reads an application command index response. Commands or applications missing an ID,
// a name or (for commands) an application ID are left out; unknown fields are ignored.
[[nodiscard]] ApplicationCommandIndex parseApplicationCommandIndex(const nlohmann::json &document);

// APPLICATION_COMMAND_AUTOCOMPLETE_RESPONSE: the bot's suggestions for the interaction
// with this nonce (empty when Discord sent null). Choices without a name or value are
// left out, as in the index.
struct AutocompleteResponse
{
    std::vector<ApplicationCommandChoice> choices;
    std::string nonce;
};
[[nodiscard]] AutocompleteResponse parseAutocompleteResponse(const nlohmann::json &data);

} // namespace Styx::Discord
