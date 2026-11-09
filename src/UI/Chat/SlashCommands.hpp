#pragma once

#include "Core/Snowflake.hpp"
#include "Discord/ApplicationCommands.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Styx::UI {

// Discord's interaction context types: where a command is being used.
enum class CommandContext : int {
    Guild = 0,
    BotDm = 1,
    PrivateChannel = 2,
};

// One runnable chat command: a top-level command without subcommands, or one leaf
// subcommand, with the options the user fills in.
struct SlashCommandEntry
{
    Core::Snowflake commandId;
    Core::Snowflake applicationId;
    Core::Snowflake version;
    std::string name;                         // the top-level command
    std::vector<std::string> subcommandPath;  // group and/or subcommand names, outermost first
    std::string displayName;                  // name and path joined with spaces: "ban user"
    std::string description;                  // the leaf's description
    std::string applicationName;              // empty when the index doesn't list the app
    std::vector<Discord::ApplicationCommandOption> options; // the leaf's own options
};

// The CHAT_INPUT commands usable in a channel, from its server or DM index and the
// user's installed apps (either may be null). A command whose contexts list doesn't
// include `context` is left out; an empty list allows every context. A command present
// in both indexes appears once. Subcommand groups and subcommands are flattened into
// leaves. Sorted by displayName, then application name.
[[nodiscard]] std::vector<SlashCommandEntry> availableSlashCommands(const Discord::ApplicationCommandIndex *scopeIndex,
                                                                    const Discord::ApplicationCommandIndex *userIndex,
                                                                    CommandContext context);

// Indexes into `entries` whose displayName contains `query` ignoring case, those starting
// with it first, each group in list order, at most `limit`. An empty query keeps the
// first `limit`.
[[nodiscard]] std::vector<std::size_t> matchSlashCommands(const std::vector<SlashCommandEntry> &entries,
                                                          std::string_view query, std::size_t limit);

// The command name typed so far when the composer text is a command in progress: the
// text after a leading "/", or nothing when it doesn't start with "/" or spans lines.
[[nodiscard]] std::optional<std::string_view> slashQuery(std::string_view composerText);

} // namespace Styx::UI
