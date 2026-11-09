#pragma once

#include "Core/Snowflake.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Styx::UI {

// Which component interactions the chat is waiting on. A request is keyed by the nonce
// Styx sends; the Gateway's create event links that nonce to the interaction ID, and the
// success and failure events may carry either one (success sends a null nonce at times).
class InteractionTracker
{
public:
    // False when the message already has a pending interaction: one click at a time.
    bool begin(const std::string &nonce, Core::Snowflake messageId, std::string componentPath);
    void created(Core::Snowflake interactionId, const std::string &nonce);

    // Each returns the message whose components need re-rendering, or nothing when the
    // event belongs to no tracked request.
    std::optional<Core::Snowflake> succeeded(Core::Snowflake interactionId, const std::string &nonce);
    std::optional<Core::Snowflake> failed(Core::Snowflake interactionId, const std::string &nonce);
    // The REST request itself was refused, before any Gateway event.
    std::optional<Core::Snowflake> rejected(const std::string &nonce);

    // Empty when nothing on the message is pending.
    [[nodiscard]] const std::string &pendingPath(Core::Snowflake messageId) const;
    [[nodiscard]] bool hasFailed(Core::Snowflake messageId) const;

    void clear();

private:
    struct Request
    {
        Core::Snowflake messageId;
        std::string componentPath;
    };

    std::optional<Core::Snowflake> finish(Core::Snowflake interactionId, const std::string &nonce, bool failure);

    std::unordered_map<std::string, Request> byNonce;
    std::unordered_map<Core::Snowflake, std::string> nonceById;
    std::unordered_map<Core::Snowflake, std::string> nonceByMessage;
    std::unordered_set<Core::Snowflake> failedMessages;
};

} // namespace Styx::UI
