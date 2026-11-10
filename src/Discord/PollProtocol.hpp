#pragma once

#include "Core/Result.hpp"
#include "Core/Snowflake.hpp"
#include "Entities.hpp"

#include <nlohmann/json.hpp>
#include <vector>

namespace Styx::Discord {

struct HttpResponse;

// Answer IDs are integers, not snowflakes. An empty list clears the current user's votes.
[[nodiscard]] nlohmann::json pollVoteBody(const std::vector<int> &answerIds);

// Voting succeeds only on HTTP 204. Preserve Discord's API error code and message.
[[nodiscard]] Core::Result<void> pollVoteResult(const HttpResponse &response);

// Expiring a poll returns HTTP 200 with its message. Reject malformed responses and
// messages for a different channel or message ID before a caller updates its cache.
[[nodiscard]] Core::Result<Message> endPollResult(const HttpResponse &response,
                                                 Core::Snowflake channelId, Core::Snowflake messageId);

} // namespace Styx::Discord
