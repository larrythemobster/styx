#pragma once

#include "Entities.hpp"
#include "Core/Snowflake.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Styx::Discord {

struct MessageSearchQuery
{
    std::string content;
    std::vector<Core::Snowflake> authorIds;
    std::vector<Core::Snowflake> mentions;
    // Only applies to server searches.
    std::vector<Core::Snowflake> channelIds;
    // image, video, file, link, embed, sticker, sound, poll or snapshot; a leading "-" negates.
    std::vector<std::string> has;
    std::optional<bool> pinned;
    // Messages before or after these ids; invalid means no bound. Snowflake::fromUnixMs turns a
    // date into one.
    Core::Snowflake before;
    Core::Snowflake after;
    int offset = 0;
    bool oldestFirst = false;
};

struct MessageSearchResult
{
    // Discord is still indexing the server or DM; try again after retryAfterSeconds.
    bool indexNotReady = false;
    int retryAfterSeconds = 0;
    int totalResults = 0;
    std::vector<Message> messages;
    std::vector<Channel> threads;
};

// The query string for a search, in the order the official client sends it.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> searchParameters(const MessageSearchQuery &query, bool server);

// Reads a search response. A 202 means the index isn't ready yet.
[[nodiscard]] MessageSearchResult parseSearchResponse(int statusCode, const std::string &body);

} // namespace Styx::Discord
