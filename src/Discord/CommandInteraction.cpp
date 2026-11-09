#include "CommandInteraction.hpp"

#include "Core/JsonUtils.hpp"
#include "Platform/Utf8.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace Styx::Discord {

namespace {

constexpr std::int64_t MaxSafeInteger = 9007199254740992LL;
constexpr double MaxSafeNumber = 9007199254740992.0;

// Bounds print as Discord shows them: whole numbers without a decimal point or exponent.
std::string formatBound(double bound)
{
    if (bound == std::trunc(bound) && std::abs(bound) <= MaxSafeNumber)
        return std::to_string(static_cast<std::int64_t>(bound));
    return std::format("{}", bound);
}

std::string checkNumericRange(double val, const std::optional<double> &minValue, const std::optional<double> &maxValue)
{
    if (minValue && val < *minValue)
        return "Must be at least " + formatBound(*minValue);
    if (maxValue && val > *maxValue)
        return "Must be at most " + formatBound(*maxValue);
    return {};
}

std::string characterCount(int count)
{
    return std::to_string(count) + (count == 1 ? " character" : " characters");
}

// std::from_chars doesn't take a leading '+', which people type for positive numbers.
std::string_view withoutPlus(std::string_view text)
{
    return text.size() > 1 && text.front() == '+' ? text.substr(1) : text;
}

} // namespace

Core::Result<nlohmann::json> commandOptionValue(const ApplicationCommandOption &option,
                                              std::string_view text)
{
    const auto trimmed = Platform::trim(text);
    if (trimmed.empty())
        return Core::Result<nlohmann::json>::makeError("Enter a value");

    if (!option.choices.empty()) {
        // A choice's label wins over another choice's value that happens to read the same.
        for (const auto &choice : option.choices) {
            if (choice.name == trimmed)
                return Core::Result<nlohmann::json>::makeOk(choice.value);
        }
        for (const auto &choice : option.choices) {
            const bool matchValue = choice.value.is_string()
                ? choice.value.get_ref<const std::string &>() == trimmed
                : choice.value.dump() == trimmed;
            if (matchValue)
                return Core::Result<nlohmann::json>::makeOk(choice.value);
        }
        return Core::Result<nlohmann::json>::makeError("Pick one of the listed choices");
    }

    switch (option.type) {
    case ApplicationCommandOptionType::STRING: {
        size_t codePoints = 0;
        for (const unsigned char c : trimmed) {
            if ((c & 0xC0) != 0x80)
                ++codePoints;
        }
        const int min = option.minLength.value_or(1);
        const int max = option.maxLength.value_or(6000);
        if (static_cast<int>(codePoints) < min)
            return Core::Result<nlohmann::json>::makeError("Must be at least " + characterCount(min));
        if (static_cast<int>(codePoints) > max)
            return Core::Result<nlohmann::json>::makeError("Must be at most " + characterCount(max));
        return Core::Result<nlohmann::json>::makeOk(std::string(trimmed));
    }
    case ApplicationCommandOptionType::INTEGER: {
        std::int64_t val = 0;
        const auto digits = withoutPlus(trimmed);
        const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), val);
        if (ec != std::errc{} || end != digits.data() + digits.size()
            || val < -MaxSafeInteger || val > MaxSafeInteger)
            return Core::Result<nlohmann::json>::makeError("Enter a whole number");
        const auto rangeError = checkNumericRange(static_cast<double>(val), option.minValue, option.maxValue);
        if (!rangeError.empty())
            return Core::Result<nlohmann::json>::makeError(rangeError);
        return Core::Result<nlohmann::json>::makeOk(val);
    }
    case ApplicationCommandOptionType::NUMBER: {
        double val = 0.0;
        const auto digits = withoutPlus(trimmed);
        const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), val);
        if (ec != std::errc{} || end != digits.data() + digits.size()
            || !std::isfinite(val) || std::abs(val) > MaxSafeNumber)
            return Core::Result<nlohmann::json>::makeError("Enter a number");
        const auto rangeError = checkNumericRange(val, option.minValue, option.maxValue);
        if (!rangeError.empty())
            return Core::Result<nlohmann::json>::makeError(rangeError);
        return Core::Result<nlohmann::json>::makeOk(val);
    }
    case ApplicationCommandOptionType::BOOLEAN: {
        if (Platform::equalsIgnoreCaseAscii(trimmed, "true"))
            return Core::Result<nlohmann::json>::makeOk(true);
        if (Platform::equalsIgnoreCaseAscii(trimmed, "false"))
            return Core::Result<nlohmann::json>::makeOk(false);
        return Core::Result<nlohmann::json>::makeError("Enter true or false");
    }
    case ApplicationCommandOptionType::USER:
    case ApplicationCommandOptionType::CHANNEL:
    case ApplicationCommandOptionType::ROLE:
    case ApplicationCommandOptionType::MENTIONABLE: {
        // Snowflake 0 parses but names nothing; send the canonical digits, not what was typed.
        const auto id = Core::JsonUtils::detail::snowflake(trimmed);
        if (!id.isValid() || id == Core::Snowflake(0))
            return Core::Result<nlohmann::json>::makeError("Enter an ID");
        return Core::Result<nlohmann::json>::makeOk(id.toString());
    }
    case ApplicationCommandOptionType::ATTACHMENT:
    case ApplicationCommandOptionType::SUB_COMMAND:
    case ApplicationCommandOptionType::SUB_COMMAND_GROUP:
    default:
        return Core::Result<nlohmann::json>::makeError("This option can't be filled in here");
    }
}

Core::Result<nlohmann::json> applicationCommandBody(const ApplicationCommandRequest &request,
                                                    const std::string &sessionId)
{
    if (sessionId.empty())
        return Core::Result<nlohmann::json>::makeError("Session ID is required");
    if (request.nonce.empty())
        return Core::Result<nlohmann::json>::makeError("Nonce is required");
    if (!request.applicationId.isValid())
        return Core::Result<nlohmann::json>::makeError("Application ID is required");
    if (!request.channelId.isValid())
        return Core::Result<nlohmann::json>::makeError("Channel ID is required");
    if (!request.commandId.isValid())
        return Core::Result<nlohmann::json>::makeError("Command ID is required");
    if (request.commandName.empty())
        return Core::Result<nlohmann::json>::makeError("Command name is required");
    if (!request.commandVersion.isValid())
        return Core::Result<nlohmann::json>::makeError("Command version is required");
    if (request.subcommandPath.size() > 2)
        return Core::Result<nlohmann::json>::makeError("Subcommand path is too deep");

    const auto declares = [&request](std::string_view name) {
        return std::ranges::any_of(request.declaredOptions,
                                   [name](const ApplicationCommandOption &opt) { return opt.name == name; });
    };
    const bool isAutocomplete = !request.focusedOption.empty();
    if (isAutocomplete && !declares(request.focusedOption))
        return Core::Result<nlohmann::json>::makeError("Unknown option " + request.focusedOption);

    std::unordered_map<std::string_view, const CommandOptionValue *> valueMap;
    for (const auto &val : request.values) {
        if (isAutocomplete && val.name == request.focusedOption)
            continue;
        if (!declares(val.name))
            return Core::Result<nlohmann::json>::makeError("Unknown option " + val.name);
        if (!valueMap.emplace(val.name, &val).second)
            return Core::Result<nlohmann::json>::makeError("Option " + val.name + " given twice");
    }

    if (!isAutocomplete) {
        for (const auto &opt : request.declaredOptions) {
            if (opt.required && !valueMap.contains(opt.name))
                return Core::Result<nlohmann::json>::makeError(opt.name + " is required");
        }
    }

    auto leaf = nlohmann::json::array();
    for (const auto &opt : request.declaredOptions) {
        if (isAutocomplete && opt.name == request.focusedOption) {
            leaf.push_back({
                { "type", static_cast<int>(opt.type) },
                { "name", opt.name },
                { "value", request.focusedText },
                { "focused", true }
            });
        } else if (const auto it = valueMap.find(opt.name); it != valueMap.end()) {
            leaf.push_back({
                { "type", static_cast<int>(opt.type) },
                { "name", opt.name },
                { "value", it->second->value }
            });
        }
    }

    nlohmann::json options;
    if (request.subcommandPath.empty()) {
        options = std::move(leaf);
    } else if (request.subcommandPath.size() == 1) {
        options = nlohmann::json::array({
            {
                { "type", 1 },
                { "name", request.subcommandPath[0] },
                { "options", std::move(leaf) }
            }
        });
    } else {
        options = nlohmann::json::array({
            {
                { "type", 2 },
                { "name", request.subcommandPath[0] },
                {
                    "options", nlohmann::json::array({
                        {
                            { "type", 1 },
                            { "name", request.subcommandPath[1] },
                            { "options", std::move(leaf) }
                        }
                    })
                }
            }
        });
    }

    nlohmann::json data;
    data["id"] = request.commandId.toString();
    data["type"] = 1;
    data["name"] = request.commandName;
    data["version"] = request.commandVersion.toString();
    data["options"] = std::move(options);

    nlohmann::json body;
    body["type"] = isAutocomplete ? 4 : 2;
    body["application_id"] = request.applicationId.toString();
    body["channel_id"] = request.channelId.toString();
    if (request.guildId.isValid())
        body["guild_id"] = request.guildId.toString();
    body["session_id"] = sessionId;
    body["nonce"] = request.nonce;
    body["data"] = std::move(data);

    return Core::Result<nlohmann::json>::makeOk(std::move(body));
}

} // namespace Styx::Discord
