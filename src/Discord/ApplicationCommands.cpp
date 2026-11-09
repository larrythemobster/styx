#include "ApplicationCommands.hpp"

#include "Core/JsonUtils.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace Styx::Discord {

namespace {

std::string_view stringAt(const nlohmann::json &obj, const char *key)
{
    const auto it = obj.find(key);
    if (it != obj.end() && it->is_string())
        return it->get_ref<const std::string &>();
    return {};
}

Core::Snowflake snowflakeAt(const nlohmann::json &obj, const char *key)
{
    const auto it = obj.find(key);
    if (it != obj.end() && it->is_string())
        return Core::JsonUtils::detail::snowflake(it->get_ref<const std::string &>());
    return Core::Snowflake::Invalid;
}

template <typename T>
std::optional<T> optionalNumber(const nlohmann::json &obj, const char *key)
{
    const auto it = obj.find(key);
    if (it != obj.end() && it->is_number())
        return it->get<T>();
    return std::nullopt;
}

bool boolOr(const nlohmann::json &obj, const char *key, bool fallback = false)
{
    const auto it = obj.find(key);
    if (it != obj.end() && it->is_boolean())
        return it->get<bool>();
    return fallback;
}

std::vector<int> intArrayAt(const nlohmann::json &obj, const char *key)
{
    std::vector<int> result;
    const auto it = obj.find(key);
    if (it != obj.end() && it->is_array()) {
        for (const auto &item : *it) {
            if (item.is_number_integer())
                result.push_back(item.get<int>());
        }
    }
    return result;
}

std::vector<ApplicationCommandOption> parseOptions(const nlohmann::json &array)
{
    std::vector<ApplicationCommandOption> options;
    if (!array.is_array())
        return options;

    for (const auto &optJson : array) {
        if (!optJson.is_object())
            continue;

        ApplicationCommandOption opt;
        opt.type = static_cast<ApplicationCommandOptionType>(
            optionalNumber<int>(optJson, "type").value_or(static_cast<int>(ApplicationCommandOptionType::STRING)));
        opt.name = stringAt(optJson, "name");
        opt.description = stringAt(optJson, "description");
        opt.required = boolOr(optJson, "required", false);
        opt.autocomplete = boolOr(optJson, "autocomplete", false);
        opt.channelTypes = intArrayAt(optJson, "channel_types");
        opt.minValue = optionalNumber<double>(optJson, "min_value");
        opt.maxValue = optionalNumber<double>(optJson, "max_value");
        opt.minLength = optionalNumber<int>(optJson, "min_length");
        opt.maxLength = optionalNumber<int>(optJson, "max_length");

        const auto choicesIt = optJson.find("choices");
        if (choicesIt != optJson.end() && choicesIt->is_array()) {
            for (const auto &choiceJson : *choicesIt) {
                if (!choiceJson.is_object())
                    continue;
                const auto choiceName = stringAt(choiceJson, "name");
                const auto valIt = choiceJson.find("value");
                if (choiceName.empty() || valIt == choiceJson.end() || valIt->is_null())
                    continue;
                ApplicationCommandChoice choice;
                choice.name = choiceName;
                choice.value = *valIt;
                opt.choices.push_back(std::move(choice));
            }
        }

        const auto nestedIt = optJson.find("options");
        if (nestedIt != optJson.end())
            opt.options = parseOptions(*nestedIt);

        options.push_back(std::move(opt));
    }
    return options;
}

} // namespace

ApplicationCommandIndex parseApplicationCommandIndex(const nlohmann::json &document)
{
    ApplicationCommandIndex index;
    if (!document.is_object())
        return index;

    index.version = snowflakeAt(document, "version");
    const auto appsIt = document.find("applications");
    const auto cmdsIt = document.find("application_commands");
    const nlohmann::json empty = nlohmann::json::array();
    const auto &apps = appsIt != document.end() && appsIt->is_array() ? *appsIt : empty;
    const auto &cmds = cmdsIt != document.end() && cmdsIt->is_array() ? *cmdsIt : empty;

    for (const auto &appJson : apps) {
        if (!appJson.is_object())
            continue;
        const auto id = snowflakeAt(appJson, "id");
        const auto name = stringAt(appJson, "name");
        if (!id.isValid() || name.empty())
            continue;

        CommandIndexApplication app;
        app.id = id;
        app.name = name;
        app.description = stringAt(appJson, "description");
        app.icon = stringAt(appJson, "icon");
        app.botId = snowflakeAt(appJson, "bot_id");
        index.applications.push_back(std::move(app));
    }

    for (const auto &cmdJson : cmds) {
        if (!cmdJson.is_object())
            continue;
        const auto id = snowflakeAt(cmdJson, "id");
        const auto name = stringAt(cmdJson, "name");
        const auto applicationId = snowflakeAt(cmdJson, "application_id");
        if (!id.isValid() || name.empty() || !applicationId.isValid())
            continue;

        ApplicationCommand cmd;
        cmd.id = id;
        cmd.type = static_cast<ApplicationCommandType>(
            optionalNumber<int>(cmdJson, "type").value_or(1));
        cmd.applicationId = applicationId;
        cmd.guildId = snowflakeAt(cmdJson, "guild_id");
        cmd.name = name;
        cmd.description = stringAt(cmdJson, "description");
        cmd.version = snowflakeAt(cmdJson, "version");
        cmd.nsfw = boolOr(cmdJson, "nsfw", false);
        cmd.contexts = intArrayAt(cmdJson, "contexts");

        const auto optionsIt = cmdJson.find("options");
        if (optionsIt != cmdJson.end())
            cmd.options = parseOptions(*optionsIt);

        index.commands.push_back(std::move(cmd));
    }

    return index;
}

} // namespace Styx::Discord
