#include "MessageReactors.hpp"

#include <algorithm>

#include <utility>

#include "Discord/Client.hpp"
#include "UserManager.hpp"

namespace Styx {
namespace Core {

bool ReactionRef::sameReaction(const ReactionRef &other) const
{
    return messageId == other.messageId && isBurst == other.isBurst && emoji.sameEmoji(other.emoji);
}

MessageReactors::MessageReactors(Discord::Client *client, UserManager *users)
    : client(client), users(users)
{
    track(client->ready.connect([this] { onReady(); }));
}

void MessageReactors::onReady()
{
    knownByReaction.clear();
}

MessageReactors::Key MessageReactors::keyOf(const ReactionRef &reaction)
{
    if (reaction.emoji.isUnicode())
        return { reaction.messageId, Snowflake::Invalid, reaction.emoji.name.get(), reaction.isBurst };
    return { reaction.messageId, reaction.emoji.id.get(), {}, reaction.isBurst };
}

std::vector<Discord::User> MessageReactors::reactors(Snowflake channelId, const ReactionRef &reaction, int firstReadLimit)
{
    Known &known = knownByReaction[keyOf(reaction)];
    const bool firstRead = !std::exchange(known.askedApi, true);

    std::vector<Discord::User> resolved;
    for (Snowflake userId : std::as_const(known.reactorIds)) {
        if (const auto user = users->getUser(userId))
            resolved.push_back(*user);
    }

    if (firstRead)
        fetch(channelId, reaction, firstReadLimit, std::nullopt, {});
    return resolved;
}

void MessageReactors::fetchPage(Snowflake channelId, const ReactionRef &reaction, std::optional<Snowflake> after, PageFetched onFetched)
{
    fetch(channelId, reaction, PageSize, after, std::move(onFetched));
}

void MessageReactors::fetch(Snowflake channelId, const ReactionRef &reaction, int limit, std::optional<Snowflake> after, PageFetched onFetched)
{
    const Platform::Guard<MessageReactors> self(this);
    client->fetchReactors(channelId, reaction.messageId, reaction.emoji.reactionKey(), reaction.isBurst, limit, after, [self, reaction, onFetched](const Result<std::vector<Discord::User>> &page) {
        if (!self)
            return;
        if (!page.success()) {
            if (onFetched)
                onFetched(Result<std::vector<Snowflake>>::makeError(page.error, page.code));
            return;
        }

        self->users->saveUsers(*page.value);
        Known &known = self->knownByReaction[keyOf(reaction)];
        std::vector<Snowflake> pageReactorIds;
        for (const Discord::User &user : *page.value) {
            pageReactorIds.push_back(user.id.get());
            if (std::find(known.reactorIds.begin(), known.reactorIds.end(), user.id.get()) == known.reactorIds.end())
                known.reactorIds.push_back(user.id.get());
        }
        self->reactorsChanged(reaction.messageId);

        if (onFetched)
            onFetched(Result<std::vector<Snowflake>>::makeOk(pageReactorIds));
    });
}

void MessageReactors::add(const ReactionRef &reaction, Snowflake userId)
{
    Known &known = knownByReaction[keyOf(reaction)];
    if (std::find(known.reactorIds.begin(), known.reactorIds.end(), userId) != known.reactorIds.end())
        return;
    known.reactorIds.push_back(userId);
    reactorsChanged(reaction.messageId);
}

void MessageReactors::remove(const ReactionRef &reaction, Snowflake userId)
{
    const auto known = knownByReaction.find(keyOf(reaction));
    if (known != knownByReaction.end() && std::erase(known->second.reactorIds, userId) > 0)
        reactorsChanged(reaction.messageId);
}

void MessageReactors::forgetReactors(Snowflake messageId)
{
    for (auto it = knownByReaction.begin(); it != knownByReaction.end(); ++it) {
        if (it->first.messageId == messageId)
            it->second.reactorIds.clear();
    }
    reactorsChanged(messageId);
}

void MessageReactors::forgetReactors(Snowflake messageId, const Discord::Emoji &emoji)
{
    for (const bool isBurst : { false, true }) {
        const auto known = knownByReaction.find(keyOf({ messageId, emoji, isBurst }));
        if (known != knownByReaction.end())
            known->second.reactorIds.clear();
    }
    reactorsChanged(messageId);
}

} // namespace Core
} // namespace Styx
