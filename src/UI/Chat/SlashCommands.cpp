#include "UI/Chat/SlashCommands.hpp"

#include "Platform/Utf8.hpp"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Styx::UI {

namespace {

void flattenCommand(const Discord::ApplicationCommand &cmd,
                    const std::string &applicationName,
                    const std::vector<Discord::ApplicationCommandOption> &options,
                    std::vector<std::string> &path,
                    std::vector<SlashCommandEntry> &result)
{
    if (path.empty()) {
        const bool hasSubcommands = std::ranges::any_of(options, [](const auto &opt) {
            return opt.type == Discord::ApplicationCommandOptionType::SUB_COMMAND ||
                   opt.type == Discord::ApplicationCommandOptionType::SUB_COMMAND_GROUP;
        });
        if (!hasSubcommands) {
            result.push_back(SlashCommandEntry{
                .commandId = cmd.id,
                .applicationId = cmd.applicationId,
                .version = cmd.version,
                .name = cmd.name,
                .subcommandPath = {},
                .displayName = cmd.name,
                .description = cmd.description,
                .applicationName = applicationName,
                .options = cmd.options,
            });
            return;
        }
    }

    for (const auto &opt : options) {
        if (opt.type == Discord::ApplicationCommandOptionType::SUB_COMMAND_GROUP) {
            path.push_back(opt.name);
            flattenCommand(cmd, applicationName, opt.options, path, result);
            path.pop_back();
        } else if (opt.type == Discord::ApplicationCommandOptionType::SUB_COMMAND) {
            auto subcommandPath = path;
            subcommandPath.push_back(opt.name);

            std::string displayName = cmd.name;
            for (const auto &segment : subcommandPath) {
                displayName += ' ';
                displayName += segment;
            }

            result.push_back(SlashCommandEntry{
                .commandId = cmd.id,
                .applicationId = cmd.applicationId,
                .version = cmd.version,
                .name = cmd.name,
                .subcommandPath = std::move(subcommandPath),
                .displayName = std::move(displayName),
                .description = opt.description,
                .applicationName = applicationName,
                .options = opt.options,
            });
        }
    }
}

} // namespace

std::vector<SlashCommandEntry> availableSlashCommands(const Discord::ApplicationCommandIndex *scopeIndex,
                                                      const Discord::ApplicationCommandIndex *userIndex,
                                                      CommandContext context)
{
    std::unordered_map<Core::Snowflake, std::string> applicationNames;
    for (const auto *index : {scopeIndex, userIndex}) {
        if (!index)
            continue;
        for (const auto &app : index->applications)
            applicationNames.try_emplace(app.id, app.name);
    }

    std::unordered_set<Core::Snowflake> seenCommandIds;
    std::vector<SlashCommandEntry> result;

    for (const auto *index : {scopeIndex, userIndex}) {
        if (!index)
            continue;
        for (const auto &cmd : index->commands) {
            if (!seenCommandIds.insert(cmd.id).second)
                continue;
            if (cmd.type != Discord::ApplicationCommandType::CHAT_INPUT)
                continue;
            if (!cmd.contexts.empty()) {
                const int targetContext = static_cast<int>(context);
                if (std::ranges::find(cmd.contexts, targetContext) == cmd.contexts.end())
                    continue;
            }

            std::string applicationName;
            if (const auto it = applicationNames.find(cmd.applicationId); it != applicationNames.end())
                applicationName = it->second;

            std::vector<std::string> path;
            flattenCommand(cmd, applicationName, cmd.options, path, result);
        }
    }

    std::ranges::sort(result, [](const SlashCommandEntry &a, const SlashCommandEntry &b) {
        if (a.displayName != b.displayName)
            return a.displayName < b.displayName;
        return a.applicationName < b.applicationName;
    });

    return result;
}

std::vector<std::size_t> matchSlashCommands(const std::vector<SlashCommandEntry> &entries,
                                            std::string_view query, std::size_t limit)
{
    if (limit == 0)
        return {};

    const auto foldedQuery = Platform::caseFold(query);
    if (foldedQuery.empty()) {
        const auto count = std::min(entries.size(), limit);
        std::vector<std::size_t> result;
        result.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
            result.push_back(i);
        return result;
    }

    std::vector<std::size_t> result;
    result.reserve(std::min(entries.size(), limit));

    std::vector<std::string> foldedNames;
    foldedNames.reserve(entries.size());

    for (std::size_t i = 0; i < entries.size(); ++i) {
        foldedNames.push_back(Platform::caseFold(entries[i].displayName));
        if (foldedNames.back().starts_with(foldedQuery)) {
            result.push_back(i);
            if (result.size() == limit)
                return result;
        }
    }

    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto pos = foldedNames[i].find(foldedQuery);
        if (pos != std::string::npos && pos != 0) {
            result.push_back(i);
            if (result.size() == limit)
                return result;
        }
    }

    return result;
}

std::optional<std::string_view> slashQuery(std::string_view composerText)
{
    if (!composerText.starts_with('/') || composerText.find('\n') != std::string_view::npos)
        return std::nullopt;
    return composerText.substr(1);
}

} // namespace Styx::UI
