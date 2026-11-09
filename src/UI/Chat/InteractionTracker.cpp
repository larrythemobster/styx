#include "UI/Chat/InteractionTracker.hpp"

#include <utility>

namespace Styx::UI {

bool InteractionTracker::begin(const std::string &nonce, Core::Snowflake messageId, std::string componentPath)
{
    if (nonce.empty() || !messageId.isValid() || nonceByMessage.contains(messageId))
        return false;

    byNonce[nonce] = Request{messageId, std::move(componentPath)};
    nonceByMessage[messageId] = nonce;
    failedMessages.erase(messageId);
    return true;
}

void InteractionTracker::created(Core::Snowflake interactionId, const std::string &nonce)
{
    if (!byNonce.contains(nonce))
        return;

    nonceById[interactionId] = nonce;
}

std::optional<Core::Snowflake> InteractionTracker::succeeded(Core::Snowflake interactionId, const std::string &nonce)
{
    return finish(interactionId, nonce, false);
}

std::optional<Core::Snowflake> InteractionTracker::failed(Core::Snowflake interactionId, const std::string &nonce)
{
    return finish(interactionId, nonce, true);
}

std::optional<Core::Snowflake> InteractionTracker::rejected(const std::string &nonce)
{
    return finish(Core::Snowflake::Invalid, nonce, true);
}

const std::string &InteractionTracker::pendingPath(Core::Snowflake messageId) const
{
    static const std::string empty;
    const auto it = nonceByMessage.find(messageId);
    if (it == nonceByMessage.end())
        return empty;

    return byNonce.at(it->second).componentPath;
}

bool InteractionTracker::hasFailed(Core::Snowflake messageId) const
{
    return failedMessages.contains(messageId);
}

void InteractionTracker::clear()
{
    byNonce.clear();
    nonceById.clear();
    nonceByMessage.clear();
    failedMessages.clear();
}

std::optional<Core::Snowflake> InteractionTracker::finish(Core::Snowflake interactionId, const std::string &nonce, bool failure)
{
    auto requestIt = byNonce.find(nonce);
    if (requestIt == byNonce.end()) {
        const auto idIt = nonceById.find(interactionId);
        if (idIt == nonceById.end())
            return std::nullopt;
        requestIt = byNonce.find(idIt->second);
    }

    const std::string targetNonce = requestIt->first;
    const auto messageId = requestIt->second.messageId;
    byNonce.erase(requestIt);
    nonceByMessage.erase(messageId);
    std::erase_if(nonceById, [&](const auto &entry) { return entry.second == targetNonce; });

    if (failure)
        failedMessages.insert(messageId);
    else
        failedMessages.erase(messageId);
    return messageId;
}

} // namespace Styx::UI
