#include "MessageManager.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include "Platform/Dispatcher.hpp"
#include "Platform/Image.hpp"
#include "Platform/Native.hpp"

#include "Discord/ApiError.hpp"
#include "Discord/Client.hpp"
#include "Emoji/EmojiManager.hpp"
#include "Markdown/Parser.hpp"
#include "Logging.hpp"
#include "UserManager.hpp"
#include "Core/PollVotes.hpp"

namespace Styx {
namespace Core {

static std::uint64_t parseSnowflake(const std::string &text)
{
    std::uint64_t value = 0;
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? value : 0;
}

static std::string resolveUserJoinMessage(const Discord::Message &msg, const std::string &author)
{
    std::int64_t ms = msg.timestamp->unixMillis();
    switch (ms % 13) {
    case 0:
        return author + " joined the party.";
    case 1:
        return author + " is here.";
    case 2:
        return "Welcome, " + author + ". We hope you brought pizza.";
    case 3:
        return "A wild " + author + " appeared.";
    case 4:
        return author + " just landed.";
    case 5:
        return author + " just slid into the server.";
    case 6:
        return author + " just showed up!";
    case 7:
        return "Welcome " + author + ". Say hi!";
    case 8:
        return author + " hopped into the server.";
    case 9:
        return "Everyone welcome " + author + "!";
    case 10:
        return "Glad you're here, " + author + ".";
    case 11:
        return "Good to see you, " + author + ".";
    case 12:
        return "Yay you made it, " + author + "!";
    default:
        return author + " joined the party.";
    }
}

static std::string resolveSystemMessageContent(const Discord::Message &msg, const std::function<std::string()> &authorName)
{
    switch (static_cast<Discord::MessageType>(msg.type.get())) {
    case Discord::MessageType::CALL:
        return authorName() + " started a call.";
    case Discord::MessageType::USER_JOIN:
        return resolveUserJoinMessage(msg, authorName());
    default:
        return msg.content;
    }
}

MessageManager::MessageManager(Snowflake accountId, Discord::Client *client,
                               UserManager *userManager)
    : repo(accountId), client(client), userManager(userManager), reactorStore(std::make_unique<MessageReactors>(client, userManager)), parser(std::make_unique<Markdown::Parser>())
{
    parser->setUserResolver([this](Snowflake userId, Snowflake sourceChannelId) {
        return this->userManager->getDisplayName(userId, this->client->getGuildIdForChannel(sourceChannelId));
    });

}

MessageManager::~MessageManager() = default;

Discord::Message *MessageManager::MessageCache::object(Snowflake id)
{
    auto it = index.find(id);
    if (it == index.end()) return nullptr;
    entries.splice(entries.begin(), entries, it->second);
    return &it->second->second;
}

void MessageManager::MessageCache::insert(Snowflake id, const Discord::Message &message)
{
    remove(id);
    entries.emplace_front(id, message);
    index[id] = entries.begin();
    if (entries.size() > Capacity) {
        index.erase(entries.back().first);
        entries.pop_back();
    }
}

void MessageManager::MessageCache::remove(Snowflake id)
{
    auto it = index.find(id);
    if (it == index.end()) return;
    entries.erase(it->second);
    index.erase(it);
}

void MessageManager::parseMessageContent(Discord::Message &msg)
{
    auto authorName = [&] {
        return userManager->getAuthorDisplayName(msg.author.get(), client->getGuildIdForChannel(msg.channelId));
    };
    cacheParsedContent(msg, resolveSystemMessageContent(msg, authorName), msg.channelId);

    if (msg.type.hasValue() && msg.type.get() == Discord::MessageType::THREAD_STARTER_MESSAGE &&
        msg.referencedMessage && msg.referencedMessage->content.hasValue() &&
        msg.referencedMessage->parsedContentCached.empty())
        cacheParsedContent(*msg.referencedMessage, msg.referencedMessage->content.get(), msg.referencedMessage->channelId);

    if (msg.snapshotMessage && msg.snapshotMessage->content.hasValue() &&
        msg.snapshotMessage->parsedContentCached.empty())
        cacheParsedContent(*msg.snapshotMessage, msg.snapshotMessage->content.get(), msg.channelId);
}

static const std::unordered_set<std::string> OfficialTopicExcludedRules = { "inlineCode", "br" };
static const std::unordered_set<std::string> OfficialBioExcludedRules = { "br", "user", "link" };

static Markdown::ParseState inlineParseState(Snowflake channelId)
{
    Markdown::ParseState state;
    state.isInline = true;
    state.customState["channelId"] = std::uint64_t(channelId);
    return state;
}

void MessageManager::cacheParsedContent(Discord::Message &msg, const std::string &content, Snowflake channelId) const
{
    auto ast = parser->parse(content, inlineParseState(channelId));
    msg.parsedContentCached = parser->toHtml(ast, Markdown::Parser::isEmojiOnly(ast));
    msg.contentIsSingleLink = Markdown::Parser::isSingleLink(ast);
}

std::string MessageManager::channelTopicHtml(const std::string &topic, Snowflake channelId) const
{
    Markdown::ParseState state = inlineParseState(channelId);
    state.excludedRules = OfficialTopicExcludedRules;
    return parser->toHtml(parser->parse(topic, state));
}

std::string MessageManager::profileBioHtml(const std::string &bio) const
{
    Markdown::ParseState state = inlineParseState(Snowflake::Invalid);
    state.excludedRules = OfficialBioExcludedRules;
    return parser->toHtml(parser->parse(bio, state));
}

void MessageManager::setChannelLinkResolver(Markdown::ChannelLinkResolverFn resolver)
{
    parser->setChannelLinkResolver(std::move(resolver));
}

void MessageManager::setChannelResolver(std::function<std::string(Snowflake)> resolver)
{
    parser->setChannelResolver([resolver](const std::string &channelId) {
        Snowflake id(parseSnowflake(channelId));
        return resolver(id);
    });
}

void MessageManager::setEmojiManager(EmojiManager *manager)
{
    emojiManager = manager;
}

void MessageManager::requestLoadChannel(Snowflake channelId)
{
    if (pagesInFlight.contains({ channelId, LoadType::Latest }))
        return;

    if (fetchedChannels.contains(channelId)) {
        if (const auto *tail = segments[channelId].tail()) {
            int count = static_cast<int>(tail->ids.size());
            if (auto slice = cachedSlice(channelId, *tail, std::max(0, count - PageSize), count)) {
                publishMessages(MessageRequestResult{ .success = true,
                                        .type = LoadType::Latest,
                                        .channelId = channelId,
                                        .messages = std::move(*slice),
                                        .reachedLatest = true });
                return;
            }
        }
    }

    fetchPage(LoadType::Latest, channelId, Snowflake::Invalid, [this, channelId](Discord::Client::MessagesCallback done) {
        client->fetchLatestMessages(channelId, PageSize, std::move(done));
    });
}

void MessageManager::requestLoadHistory(Snowflake channelId, Snowflake beforeId)
{
    if (pagesInFlight.contains({ channelId, LoadType::History }))
        return;

    auto start = channelStartId.find(channelId);
    if (start != channelStartId.end() && start->second >= beforeId) {
        messagesReceived.publish(MessageRequestResult{ .success = true,
                                .type = LoadType::History,
                                .channelId = channelId,
                                .anchorId = beforeId });
        return;
    }

    if (auto pos = segments[channelId].find(beforeId); pos.run && pos.index > 0) {
        if (auto slice = cachedSlice(channelId, *pos.run, std::max(0, pos.index - PageSize), pos.index)) {
            messagesReceived.publish(MessageRequestResult{ .success = true,
                                    .type = LoadType::History,
                                    .channelId = channelId,
                                    .messages = *slice,
                                    .anchorId = beforeId });
            return;
        }
    }

    fetchPage(LoadType::History, channelId, beforeId, [this, channelId, beforeId](Discord::Client::MessagesCallback done) {
        client->fetchHistory(channelId, beforeId, PageSize, std::move(done));
    });
}

void MessageManager::requestLoadFuture(Snowflake channelId, Snowflake afterId)
{
    if (pagesInFlight.contains({ channelId, LoadType::Future }))
        return;

    if (auto pos = segments[channelId].find(afterId); pos.run) {
        int count = static_cast<int>(pos.run->ids.size());
        bool knowsWhatFollows = pos.index + 1 < count || pos.run->isTail;
        if (knowsWhatFollows) {
            int end = std::min(count, pos.index + 1 + PageSize);
            if (auto slice = cachedSlice(channelId, *pos.run, pos.index + 1, end)) {
                messagesReceived.publish(MessageRequestResult{ .success = true,
                                        .type = LoadType::Future,
                                        .channelId = channelId,
                                        .messages = *slice,
                                        .reachedLatest = pos.run->isTail && end == count,
                                        .anchorId = afterId });
                return;
            }
        }
    }

    fetchPage(LoadType::Future, channelId, afterId, [this, channelId, afterId](Discord::Client::MessagesCallback done) {
        client->fetchMessagesAfter(channelId, afterId, PageSize, std::move(done));
    });
}

void MessageManager::requestLoadAround(Snowflake channelId, Snowflake messageId)
{
    if (pagesInFlight.contains({ channelId, LoadType::Jump }))
        return;

    if (auto pos = segments[channelId].find(messageId); pos.run) {
        int count = static_cast<int>(pos.run->ids.size());
        int from = std::max(0, pos.index - JumpWindow / 2);
        int to = std::min(count, from + JumpWindow);
        if (auto slice = cachedSlice(channelId, *pos.run, from, to)) {
            publishMessages(MessageRequestResult{ .success = true,
                                    .type = LoadType::Jump,
                                    .channelId = channelId,
                                    .messages = std::move(*slice),
                                    .reachedLatest = pos.run->isTail && to == count });
            return;
        }
    }

    fetchPage(LoadType::Jump, channelId, messageId, [this, channelId, messageId](Discord::Client::MessagesCallback done) {
        client->fetchMessagesAround(channelId, messageId, JumpWindow, std::move(done));
    });
}

void MessageManager::fetchPage(LoadType type, Snowflake channelId, Snowflake anchorId, const PageFetcher &fetch)
{
    pagesInFlight.insert({ channelId, type });

    Platform::Guard<MessageManager> guard(this);
    fetch([this, guard, type, channelId, anchorId](const Result<std::vector<Discord::Message>> &result) {
        if (!guard)
            return;

        pagesInFlight.erase({ channelId, type });

        if (!result.success()) {
            log(LogLevel::Warning, LogCore) << "Failed to load" << type << "page in" << channelId << result.error;
            publishMessages(MessageRequestResult{ .success = false,
                                    .type = type,
                                    .channelId = channelId,
                                    .anchorId = anchorId });
            return;
        }
        onApiMessagesReceived(result.value.value(), type, channelId, anchorId);
    });
}

std::optional<std::vector<Discord::Message>> MessageManager::cachedSlice(Snowflake channelId,
                                                                   const MessageSegments::Run &run,
                                                                   int from, int to)
{
    std::vector<Discord::Message> result;
    if (from >= to)
        return result;

    result.reserve(to - from);
    for (int i = from; i < to; i++) {
        auto *msg = messageCache.object(run.ids[i]);
        if (!msg) {
            result.clear();
            break;
        }
        result.push_back(*msg);
    }
    if (!result.empty())
        return result;

    std::vector<Discord::Message> fromDisk = repo.getMessagesInRange(channelId, run.ids[from], run.ids[to - 1]);
    if (fromDisk.empty())
        return std::nullopt;

    for (auto &msg : fromDisk)
        parseMessageContent(msg);
    return fromDisk;
}

static std::vector<Snowflake> idsOf(const std::vector<Discord::Message> &messages)
{
    std::vector<Snowflake> ids;
    ids.reserve(messages.size());
    for (const auto &msg : messages)
        ids.push_back(msg.id.get());
    return ids;
}

static bool sameNickAvatarAndRoles(const Discord::Member &a, const Discord::Member &b)
{
    return a.nick.valueOr() == b.nick.valueOr() &&
           a.avatar.valueOr() == b.avatar.valueOr() &&
           a.roles.valueOr() == b.roles.valueOr();
}

void MessageManager::cacheGatewayMembers(const Discord::Message &msg)
{
    if (!msg.guildId.hasValue())
        return;
    Snowflake guildId = msg.guildId.get();
    std::vector<Snowflake> changedUserIds;

    auto cache = [&](const Discord::User &user, Discord::Member member) {
        Snowflake userId = user.id.get();
        auto known = userManager->getMember(guildId, userId);
        if (known && sameNickAvatarAndRoles(*known, member))
            return;
        member.user = user;
        userManager->saveMemberWithUser(guildId, member);
        changedUserIds.push_back(userId);
    };

    if (msg.member.hasValue() && msg.author.hasValue())
        cache(msg.author.get(), msg.member.get());

    if (msg.mentions.hasValue()) {
        for (const auto &mentioned : msg.mentions.get())
            if (mentioned.member.hasValue())
                cache(mentioned, mentioned.member.get());
    }

    if (!changedUserIds.empty())
        membersLearnedFromMessage.publish(guildId, changedUserIds);
}

bool MessageManager::dismissEphemeralMessage(Snowflake channelId, Snowflake messageId)
{
    if (!ephemeralMessages.dismiss(channelId, messageId))
        return false;

    messageDeleted.publish(channelId, messageId);
    return true;
}

void MessageManager::onMessageCreated(const Discord::Message &message)
{
    if (ephemeralMessages.isDismissed(message.id))
        return;

    if (message.isEphemeral()) {
        receiveEphemeralMessage(message);
        return;
    }

    if (message.guildId.hasValue())
        client->registerChannelGuild(message.channelId, message.guildId.get());

    cacheGatewayMembers(message);
    onApiMessagesReceived({ message }, Discord::Client::MessageLoadType::Created,
                          message.channelId);
}

void MessageManager::onMessageUpdated(const Discord::Message &message)
{
    applyMessageUpdate(message, false);
}

void MessageManager::onPollEnded(const Discord::Message &message)
{
    applyMessageUpdate(message, true);
}

void MessageManager::applyMessageUpdate(const Discord::Message &message, bool updatesOnly)
{
    if (ephemeralMessages.isDismissed(message.id))
        return;

    if (const auto *priv = ephemeralMessages.find(message.id)) {
        Discord::Message merged = *priv;
        merged.applyUpdate(message);
        merged.flags.get().setFlag(Discord::MessageFlag::EPHEMERAL, true);
        receiveEphemeralMessage(std::move(merged));
        return;
    }

    cacheGatewayMembers(message);

    Discord::Message merged;
    bool haveBaseline = false;

    if (auto *cached = messageCache.object(message.id)) {
        merged = *cached;
        haveBaseline = true;
    } else if (auto existing = repo.getMessage(message.id)) {
        merged = *existing;
        haveBaseline = true;
    }

    if (haveBaseline) {
        merged.applyUpdate(message);
    } else {
        if (message.isEphemeral() || !message.presentKeys.contains("content"))
            return;
        merged = message;
    }

    if (merged.isEphemeral()) {
        messageCache.remove(merged.id);
        if (segments.contains(merged.channelId))
            segments[merged.channelId].remove(merged.id);

        repo.updateMessageContent(merged);
        receiveEphemeralMessage(std::move(merged));
        return;
    }

    parseMessageContent(merged);

    repo.updateMessageContent(merged);

    if (message.presentKeys.contains("reactions"))
        repo.updateReactionsJson(merged.id, merged.reactionsJson);

    publishMessageUpdate(merged, updatesOnly);
}

void MessageManager::onMessageDeleted(const Discord::MessageDelete &event)
{
    Snowflake channelId = event.channelId.get();
    Snowflake messageId = event.id.get();

    if (dismissEphemeralMessage(channelId, messageId))
        return;

    messageCache.remove(messageId);

    if (segments.contains(channelId))
        segments[channelId].remove(messageId);

    repo.markMessageDeleted(messageId);

    messageDeleted.publish(channelId, messageId);
}

void MessageManager::onMessageSendFailed(const std::string &nonce, const std::string &error)
{
    log(LogLevel::Warning, LogCore) << "Message send failed for nonce" << nonce << ":" << error;

    messageErrored.publish(nonce);
}

static bool hasOwnReactionOfOtherType(const Discord::Message &msg, const Discord::Emoji &emoji, bool isBurst)
{
    if (!msg.reactions.hasValue())
        return false;

    for (const Discord::Reaction &reaction : msg.reactions.get()) {
        if (reaction.emoji->sameEmoji(emoji))
            return isBurst ? reaction.me.get() : reaction.meBurst.valueOr(false);
    }
    return false;
}

void MessageManager::addReaction(Snowflake channelId, Snowflake messageId, const Discord::Emoji &emoji, bool isBurst, ReactionLocation location)
{
    const Discord::Message *cached = messageCache.object(messageId);
    if (cached && hasOwnReactionOfOtherType(*cached, emoji, isBurst)) {
        reactionRejected.publish(ReactionRejection::AlreadyReactedWithOtherType);
        return;
    }

    sendReactionChange(ReactionOp::Add, { channelId, messageId, emoji, isBurst, location }, ReactionAttempt::First);
}

void MessageManager::removeReaction(Snowflake channelId, Snowflake messageId, const Discord::Emoji &emoji, bool isBurst, ReactionLocation location)
{
    sendReactionChange(ReactionOp::Remove, { channelId, messageId, emoji, isBurst, location }, ReactionAttempt::First);
}

void MessageManager::removeReactionOf(Snowflake reactorId, Snowflake channelId, const ReactionRef &reaction)
{
    sendReactionChange(ReactionOp::Remove, { channelId, reaction.messageId, reaction.emoji, reaction.isBurst, ReactionLocation::Message, reactorId }, ReactionAttempt::First);
}

void MessageManager::sendReactionChange(ReactionOp op, const ReactionChange &change, ReactionAttempt attempt)
{
    const Platform::Guard<MessageManager> self(this);
    const auto onFinished = [self, op, change, attempt](const Discord::Client::ReactionResult &result) {
        if (self && !result.success)
            self->onReactionChangeFailed(op, change, attempt, result);
    };

    applyOptimistically(op, change);

    const std::string reactionKey = change.emoji.reactionKey();
    if (op == ReactionOp::Add) {
        if (emojiManager)
            emojiManager->trackReaction(reactionKey);
        client->addReaction(change.channelId, change.messageId, reactionKey, change.isBurst, change.location, onFinished);
    } else {
        client->removeReaction(change.channelId, change.messageId, reactionKey, change.isBurst, change.location, change.reactorUnlessMe, onFinished);
    }
}

void MessageManager::onReactionChangeFailed(ReactionOp op, const ReactionChange &change, ReactionAttempt attempt, const Discord::Client::ReactionResult &result)
{
    constexpr int Forbidden = 403;
    constexpr int TooManyRequests = 429;

    const bool canRetry = attempt == ReactionAttempt::First;
    const auto retry = [this, op, change]() { sendReactionChange(op, change, ReactionAttempt::Retry); };

    if (result.statusCode == TooManyRequests) {
        if (canRetry) {
            Platform::Dispatcher::main().setTimer(std::chrono::seconds(result.retryAfterSeconds), false, [guard = Platform::Guard<MessageManager>(this), retry] { if (guard) retry(); });
            return;
        }
    } else if (result.statusCode == Forbidden) {
        if (result.errorCode == Discord::ApiError::TooManyReactions)
            reactionRejected.publish(ReactionRejection::TooManyReactions);
    } else if (canRetry) {
        retry();
        return;
    }

    const ReactionOp undo = op == ReactionOp::Add ? ReactionOp::Remove : ReactionOp::Add;
    applyOptimistically(undo, change);
}

void MessageManager::sendMessage(Snowflake channelId, const std::string &content,
                                 Snowflake replyToMessageId,
                                 const std::vector<PendingAttachment> &attachments)
{
    if (emojiManager)
        emojiManager->trackMessageEmojis(content);

    Snowflake nonceId = Snowflake::generateNonce();
    std::string nonce = nonceId.toString();

    const auto &outgoing = attachments;

    Discord::Message preview;
    preview.id = nonceId; // temporary id, will be overwritten
    preview.nonce = nonce;
    preview.channelId = channelId;
    preview.content = content;
    preview.timestamp = Platform::DateTime::now();
    preview.author = client->getMe();
    preview.flags = Discord::MessageFlags(0);
    preview.isPendingOutbound = true;

    if (!outgoing.empty()) {
        std::vector<Discord::Attachment> previewAttachments;
        previewAttachments.reserve(outgoing.size());
        for (std::size_t i = 0; i < outgoing.size(); i++) {
            const auto &att = outgoing[i];
            Discord::Attachment a;
            a.id = Snowflake(nonceId + i + 1);
            a.filename = att.filename;
            a.size = att.size;
            a.contentType = att.mimeType;
            if (!att.image.empty()) {
                a.localPreview = att.image;
                a.width = att.image.width;
                a.height = att.image.height;
            } else if (att.mimeType.starts_with("image/") && !att.filePath.empty()) {
                std::string localUrl = Platform::fileUrl(std::filesystem::u8path(att.filePath));
                a.url = localUrl;
                a.proxyUrl = localUrl;
                auto image = Platform::Image::sizeFile(std::filesystem::u8path(att.filePath));
                if (image) {
                    a.width = image->width;
                    a.height = image->height;
                }
            }
            if (att.isSpoiler)
                a.flags = Discord::AttachmentFlags(Discord::AttachmentFlag::IS_SPOILER);
            previewAttachments.push_back(std::move(a));
        }
        preview.attachments = previewAttachments;
    }

    if (replyToMessageId.isValid()) {
        preview.type = Discord::MessageType::REPLY;
        Discord::MessageReference ref;
        ref.messageId = replyToMessageId;
        ref.channelId = channelId;
        preview.messageReference = ref;
    } else {
        preview.type = Discord::MessageType::DEFAULT;
    }

    auto ast = parser->parse(content, inlineParseState(channelId));
    bool jumbo = Markdown::Parser::isEmojiOnly(ast);
    preview.parsedContentCached = parser->toHtml(ast, jumbo);

    // get our fake preview in
    messagesReceived.publish(
            MessageRequestResult{ true, Discord::Client::MessageLoadType::Created, channelId, { preview } });

    for (const auto &att : outgoing) {
        if (att.data.empty() && att.filePath.empty()) {
            messageErrored.publish(nonce);
            return;
        }
    }

    client->sendMessage(channelId, content, nonce, replyToMessageId, outgoing);
}

void MessageManager::cancelSend(Snowflake channelId, const std::string &nonce)
{
    if (!client->cancelMessageSend(nonce))
        return;

    Snowflake nonceId(parseSnowflake(nonce));
    messageCache.remove(nonceId);
    if (segments.contains(channelId))
        segments[channelId].remove(nonceId);
    messageDeleted.publish(channelId, nonceId);
}

static std::string reactionsToJson(const std::vector<Discord::Reaction> &reactions)
{
    if (reactions.empty()) return {};
    auto arr = nlohmann::json::array();
    for (const auto &r : reactions) {
        nlohmann::json emoji = {{"id", r.emoji->isUnicode() ? nlohmann::json(nullptr) : nlohmann::json(r.emoji->id->toString())}, {"name", *r.emoji->name}};
        if (r.emoji->animated.hasValue()) emoji["animated"] = *r.emoji->animated;
        nlohmann::json obj = {{"emoji", emoji}, {"count", *r.count}, {"me", *r.me}};
        if (r.countDetails.hasValue()) obj["count_details"] = {{"burst", *r.countDetails->burst}, {"normal", *r.countDetails->normal}};
        if (r.meBurst.hasValue()) obj["me_burst"] = *r.meBurst;
        if (r.burstCount.hasValue()) obj["burst_count"] = *r.burstCount;
        if (r.burstColors.hasValue()) obj["burst_colors"] = *r.burstColors;
        arr.push_back(std::move(obj));
    }
    return arr.dump();
}

static void rebuildReactionsJson(Discord::Message &msg)
{
    if (!msg.reactions.hasValue()) {
        msg.reactionsJson.clear();
        return;
    }
    msg.reactionsJson = reactionsToJson(*msg.reactions);
}

static std::vector<Discord::Reaction> reactionsFromJson(const std::string &json)
{
    std::vector<Discord::Reaction> reactions;
    auto doc = nlohmann::json::parse(json, nullptr, false);
    if (doc.is_array())
        for (const auto &value : doc)
            if (value.is_object()) reactions.push_back(Discord::Reaction::fromJson(value));
    return reactions;
}

std::vector<Discord::Reaction> MessageManager::reactionsOf(Snowflake messageId)
{
    if (const Discord::Message *cached = messageCache.object(messageId))
        return cached->reactions.valueOr();
    return reactionsFromJson(repo.getReactionsJson(messageId));
}

void MessageManager::publishMessageUpdate(const Discord::Message &message, bool updatesOnly)
{
    messageCache.insert(message.id, message);
    messagesReceived.publish(MessageRequestResult{
        .success = true,
        .type = LoadType::Created,
        .channelId = message.channelId,
        .messages = { message },
        .updatesOnly = updatesOnly,
    });
}

void MessageManager::emitReactionUpdate(Discord::Message &msg)
{
    rebuildReactionsJson(msg);
    repo.saveMessages({ msg });
    publishMessageUpdate(msg, false);
    reactionsChanged.publish(msg.id);
}

static bool addToReactions(std::vector<Discord::Reaction> &reactions,
                           const Discord::Emoji &emoji, bool isBurst, bool isMe,
                           const std::vector<std::string> &burstColors = {})
{
    bool found = false;
    for (auto &r : reactions) {
        if (r.emoji->sameEmoji(emoji)) {
            if (isMe && (isBurst ? r.meBurst.valueOr(false) : r.me.get()))
                return false;
            r.count = *r.count + 1;
            if (r.countDetails.hasValue()) {
                if (isBurst)
                    r.countDetails->burst = *r.countDetails->burst + 1;
                else
                    r.countDetails->normal = *r.countDetails->normal + 1;
            }
            if (isMe) {
                if (isBurst)
                    r.meBurst = true;
                else
                    r.me = true;
            }
            if (isBurst && !burstColors.empty())
                r.burstColors = burstColors;
            found = true;
            break;
        }
    }

    if (!found) {
        Discord::Reaction newReaction;
        newReaction.emoji = emoji;
        newReaction.count = 1;
        newReaction.me = !isBurst && isMe;
        newReaction.meBurst = isBurst && isMe;
        newReaction.burstCount = isBurst ? 1 : 0;

        Discord::ReactionCountDetails details;
        details.burst = isBurst ? 1 : 0;
        details.normal = isBurst ? 0 : 1;
        newReaction.countDetails = details;

        if (isBurst && !burstColors.empty())
            newReaction.burstColors = burstColors;

        reactions.push_back(newReaction);
    }
    return true;
}

void MessageManager::updateReactions(Snowflake messageId, const std::function<bool(std::vector<Discord::Reaction> &)> &mutate)
{
    auto *cached = messageCache.object(messageId);
    if (!cached) {
        std::vector<Discord::Reaction> reactions = reactionsFromJson(repo.getReactionsJson(messageId));
        if (mutate(reactions)) {
            repo.updateReactionsJson(messageId, reactionsToJson(reactions));
            reactionsChanged.publish(messageId);
        }
        return;
    }

    Discord::Message msg = *cached;

    if (!msg.reactions.hasValue())
        msg.reactions = std::vector<Discord::Reaction>();

    if (mutate(*msg.reactions))
        emitReactionUpdate(msg);
}

void MessageManager::onReactionAdd(const Discord::MessageReactionAdd &event)
{
    bool isBurst = event.type.hasValue() && *event.type == 1;
    bool isMe = event.userId.get() == client->getMe().id.get();
    reactorStore->add({ event.messageId, event.emoji, isBurst }, event.userId);
    updateReactions(event.messageId, [&](std::vector<Discord::Reaction> &reactions) {
        return addToReactions(reactions, event.emoji, isBurst, isMe, event.burstColors.valueOr({}));
    });
}

static void applyReactionAddMany(std::vector<Discord::Reaction> &reactions,
                                 const Discord::MessageReactionAddMany &event, Snowflake myId)
{
    for (const auto &debounced : *event.reactions) {
        bool isMe = false;
        for (const auto &uid : *debounced.users) {
            if (uid == myId) {
                isMe = true;
                break;
            }
        }

        int addCount = debounced.users->size();
        bool found = false;
        for (auto &r : reactions) {
            if (r.emoji->sameEmoji(debounced.emoji)) {
                const bool ownAlreadyApplied = isMe && r.me.get();
                if (ownAlreadyApplied)
                    addCount--;
                r.count = *r.count + addCount;
                if (r.countDetails.hasValue())
                    r.countDetails->normal = *r.countDetails->normal + addCount;
                if (isMe)
                    r.me = true;
                found = true;
                break;
            }
        }

        if (!found) {
            Discord::Reaction newReaction;
            newReaction.emoji = debounced.emoji;
            newReaction.count = addCount;
            newReaction.me = isMe;
            newReaction.meBurst = false;
            newReaction.burstCount = 0;

            Discord::ReactionCountDetails details;
            details.burst = 0;
            details.normal = addCount;
            newReaction.countDetails = details;

            reactions.push_back(newReaction);
        }
    }
}

void MessageManager::onReactionAddMany(const Discord::MessageReactionAddMany &event)
{
    Snowflake myId = client->getMe().id;
    updateReactions(event.messageId, [&](std::vector<Discord::Reaction> &reactions) {
        applyReactionAddMany(reactions, event, myId);
        return true;
    });
}

static bool removeFromReactions(std::vector<Discord::Reaction> &reactions,
                                const Discord::Emoji &emoji, bool isBurst, bool isMe)
{
    for (int i = 0; i < reactions.size(); ++i) {
        auto &r = reactions[i];
        if (r.emoji->sameEmoji(emoji)) {
            if (isMe && !(isBurst ? r.meBurst.valueOr(false) : r.me.get()))
                return false;
            r.count = *r.count - 1;
            if (r.countDetails.hasValue()) {
                if (isBurst)
                    r.countDetails->burst = std::max(0, *r.countDetails->burst - 1);
                else
                    r.countDetails->normal = std::max(0, *r.countDetails->normal - 1);
            }
            if (isMe) {
                if (isBurst)
                    r.meBurst = false;
                else
                    r.me = false;
            }

            if (*r.count <= 0)
                reactions.erase(reactions.begin() + i);

            return true;
        }
    }
    return false;
}

void MessageManager::onReactionRemove(const Discord::MessageReactionRemove &event)
{
    bool isBurst = event.type.hasValue() && *event.type == 1;
    bool isMe = event.userId.get() == client->getMe().id.get();
    reactorStore->remove({ event.messageId, event.emoji, isBurst }, event.userId);
    updateReactions(event.messageId, [&](std::vector<Discord::Reaction> &reactions) {
        return removeFromReactions(reactions, event.emoji, isBurst, isMe);
    });
}

void MessageManager::applyOptimistically(ReactionOp op, const ReactionChange &change)
{
    const Snowflake myId = client->getMe().id.get();
    const Snowflake reactorId = change.reactorUnlessMe.value_or(myId);
    const ReactionRef reaction{ change.messageId, change.emoji, change.isBurst };
    if (op == ReactionOp::Add)
        reactorStore->add(reaction, reactorId);
    else
        reactorStore->remove(reaction, reactorId);

    const bool countsWaitForGateway = reactorId != myId;
    if (countsWaitForGateway)
        return;

    constexpr bool isMe = true;
    updateReactions(change.messageId, [&](std::vector<Discord::Reaction> &reactions) {
        return op == ReactionOp::Add ? addToReactions(reactions, change.emoji, change.isBurst, isMe) : removeFromReactions(reactions, change.emoji, change.isBurst, isMe);
    });
}

void MessageManager::onReactionRemoveAll(const Discord::MessageReactionRemoveAll &event)
{
    reactorStore->forgetReactors(event.messageId);

    auto *cached = messageCache.object(event.messageId);
    if (!cached) {
        repo.updateReactionsJson(event.messageId, {});
        reactionsChanged.publish(event.messageId);
        return;
    }

    Discord::Message msg = *cached;
    msg.reactions = std::vector<Discord::Reaction>();

    emitReactionUpdate(msg);
}

void MessageManager::onReactionRemoveEmoji(const Discord::MessageReactionRemoveEmoji &event)
{
    reactorStore->forgetReactors(event.messageId, event.emoji);

    auto *cached = messageCache.object(event.messageId);
    if (!cached) {
        std::vector<Discord::Reaction> reactions =
                reactionsFromJson(repo.getReactionsJson(event.messageId));
        for (int i = 0; i < reactions.size(); ++i) {
            if (reactions[i].emoji->sameEmoji(event.emoji)) {
                reactions.erase(reactions.begin() + i);
                break;
            }
        }
        repo.updateReactionsJson(event.messageId, reactionsToJson(reactions));
        reactionsChanged.publish(event.messageId);
        return;
    }

    Discord::Message msg = *cached;

    if (!msg.reactions.hasValue())
        return;

    for (int i = 0; i < msg.reactions->size(); ++i) {
        if ((*msg.reactions)[i].emoji->sameEmoji(event.emoji)) {
            msg.reactions->erase(msg.reactions->begin() + i);
            break;
        }
    }

    emitReactionUpdate(msg);
}

void MessageManager::onPollVote(const Discord::MessagePollVote &event, bool added)
{
    Discord::Message message;
    if (auto *cached = messageCache.object(event.messageId.get())) {
        message = *cached;
    } else if (auto existing = repo.getMessage(event.messageId.get())) {
        message = std::move(*existing);
    } else {
        return;
    }

    if (!applyPollVote(message, event, added, client->getMe().id.get()))
        return;

    repo.updateMessageContent(message);
    publishMessageUpdate(message, true);
}

void MessageManager::receiveEphemeralMessage(Discord::Message message)
{
    parseMessageContent(message);
    const Snowflake channelId = message.channelId;
    const Snowflake messageId = message.id;
    if (ephemeralMessages.put(std::move(message))) {
        if (const auto *stored = ephemeralMessages.find(messageId)) {
            messagesReceived.publish(MessageRequestResult{
                .success = true,
                .type = Discord::Client::MessageLoadType::Created,
                .channelId = channelId,
                .messages = { *stored },
                .reachedLatest = false,
                .anchorId = Snowflake::Invalid
            });
        }
    }
}

void MessageManager::publishMessages(MessageRequestResult result)
{
    if (result.success && (result.type == LoadType::Latest || (result.type == LoadType::Jump && result.reachedLatest))) {
        const std::size_t beforeCount = result.messages.size();
        ephemeralMessages.appendForChannel(result.channelId, result.messages);
        if (result.messages.size() > beforeCount) {
            std::sort(result.messages.begin(), result.messages.end(),
                      [](const auto &a, const auto &b) { return a.id.get() < b.id.get(); });
        }
    }
    messagesReceived.publish(result);
}

void MessageManager::onApiMessagesReceived(const std::vector<Discord::Message> &messages, LoadType type,
                                           Snowflake channelId, Snowflake anchorId)
{
    const std::size_t rawCount = messages.size();

    std::vector<Discord::Message> publicMessages;
    publicMessages.reserve(messages.size());

    for (const auto &msg : messages) {
        if (ephemeralMessages.isDismissed(msg.id))
            continue;

        if (msg.isEphemeral()) {
            Discord::Message copy = msg;
            parseMessageContent(copy);
            ephemeralMessages.put(std::move(copy));
        } else {
            publicMessages.push_back(msg);
        }
    }

    repo.saveMessages(publicMessages);

    std::sort(publicMessages.begin(), publicMessages.end(),
              [](const auto &a, const auto &b) { return a.id.get() < b.id.get(); });

    bool shortPage = rawCount < PageSize;
    if (type == LoadType::History && shortPage)
        channelStartId[channelId] = publicMessages.empty() ? anchorId : publicMessages.front().id.get();
    else if (type == LoadType::Latest && shortPage && !publicMessages.empty())
        channelStartId[channelId] = publicMessages.front().id.get();

    for (auto &msg : publicMessages)
        parseMessageContent(msg);

    for (const auto &msg : publicMessages) {
        messageCache.insert(msg.id, msg);
    }

    auto &segs = segments[channelId];
    std::vector<Snowflake> ids = idsOf(publicMessages);
    bool reachedLatest = false;

    switch (type) {
    case LoadType::Latest:
        fetchedChannels.insert(channelId);
        segs.setTail(ids);
        reachedLatest = true;
        break;
    case LoadType::Created:
        segs.merge(ids, Snowflake::Invalid, true);
        break;
    case LoadType::History:
        segs.merge(ids, anchorId, false);
        break;
    case LoadType::Future:
    case LoadType::Jump: {
        bool coversPresent = false;
        if (type == LoadType::Future) {
            coversPresent = rawCount < PageSize;
        } else {
            int atOrAfterTarget = std::count_if(ids.cbegin(), ids.cend(),
                                                [&](Snowflake id) { return id >= anchorId; });
            coversPresent = atOrAfterTarget < JumpWindow / 2;
        }
        segs.merge(ids, anchorId, coversPresent);

        Snowflake newestLoaded = ids.empty() ? anchorId : ids.back();
        auto pos = segs.find(newestLoaded);

        reachedLatest = pos.run ? pos.run->isTail : coversPresent;
        log(LogLevel::Debug, LogCore) << "Loaded" << ids.size() << "messages around/after" << anchorId << "in"
                         << channelId << "coversPresent" << coversPresent << "reachedLatest" << reachedLatest;

        if (pos.run && reachedLatest) {
            for (int i = pos.index + 1; i < static_cast<int>(pos.run->ids.size()); ++i) {
                auto *cached = messageCache.object(pos.run->ids[i]);
                if (!cached)
                    break;
                publicMessages.push_back(*cached);
            }
        }
        break;
    }
    }

    publishMessages(MessageRequestResult{ .success = true,
                            .type = type,
                            .channelId = channelId,
                            .messages = std::move(publicMessages),
                            .reachedLatest = reachedLatest,
                            .anchorId = anchorId });
}

} // namespace Core
} // namespace Styx
