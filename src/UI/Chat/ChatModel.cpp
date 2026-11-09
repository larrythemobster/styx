#include "ChatModel.hpp"
#include "Core/Markdown/Parser.hpp"
#include "Core/Theme/Manager.hpp"
#include "Core/Media/Player.hpp"
#include "Discord/CdnUrls.hpp"
#include "Discord/Enums.hpp"
#include <algorithm>
#include <cmath>
#include <format>

namespace Styx::UI {
namespace {
bool isSystemMessageType(Discord::MessageType type)
{
    return type != Discord::MessageType::DEFAULT && type != Discord::MessageType::REPLY
        && type != Discord::MessageType::CHAT_INPUT_COMMAND && type != Discord::MessageType::CONTEXT_MENU_COMMAND;
}
std::string animatedUrlFor(const std::string &proxy, const std::string &original, bool animated, Platform::Size source, Platform::Size display)
{
    if (!Discord::Cdn::isDiscordAssetUrl(proxy) || !Discord::Cdn::isAnimatedImage(original, animated)) return {};
    const auto scale = Platform::primaryDevicePixelRatio();
    return Discord::Cdn::animatedImageUrl(proxy, animated, source, {int(display.width * scale), int(display.height * scale)});
}
std::string klipyThumbnailUrl(const Discord::Embed &embed)
{
    if (!embed.provider.hasValue() || embed.provider->name.valueOr(std::string{}) != "Klipy" || !embed.thumbnail.hasValue()) return {};
    return embed.thumbnail->proxyUrl.valueOr(embed.thumbnail->url.valueOr(std::string{}));
}
Core::FavoriteGifCandidate favoriteEmbedImage(const Discord::Embed &embed, Platform::Size size)
{
    return {embed.image->url.get(), embed.image->proxyUrl.get(), klipyThumbnailUrl(embed), embed.video.hasValue() ? Proto::GifType::Video : Proto::GifType::Image, size};
}
EmbedType embedTypeFromString(const std::string &type)
{
    if (type == "age_verification_system_notification") return EmbedType::AgeVerificationSystemNotification;
    if (type == "application_news") return EmbedType::ApplicationNews;
    if (type == "article") return EmbedType::Article;
    if (type == "auto_moderation_message") return EmbedType::AutoModerationMessage;
    if (type == "auto_moderation_notification") return EmbedType::AutoModerationNotification;
    if (type == "gift") return EmbedType::Gift;
    if (type == "gifv") return EmbedType::Gifv;
    if (type == "image") return EmbedType::Image;
    if (type == "link") return EmbedType::Link;
    if (type == "poll_result") return EmbedType::PollResult;
    if (type == "post_preview") return EmbedType::PostPreview;
    if (type == "safety_policy_notice") return EmbedType::SafetyPolicyNotice;
    if (type == "safety_system_notification") return EmbedType::SafetySystemNotification;
    if (type == "video") return EmbedType::Video;
    return EmbedType::Rich;
}
Platform::Size imageSize(int width, int height) { return {width, height}; }
}
ChatModel::ChatModel(Core::ImageManager *manager) : imageManager(manager)
{
    track(manager->imageUnavailable.connect([this](const std::string &url, const Platform::Size &) {
        if (Discord::Cdn::isStickerUrl(url)) refreshRowsShowingSticker(url);
    }));
    track(manager->imageFetched.connect([this](const std::string &url, const Platform::Size &, const Platform::ImageBuffer &) {
        if (Discord::Cdn::isStickerUrl(url)) { refreshRowsShowingSticker(url); return; }
        const bool emoji = Discord::Cdn::isEmojiUrl(url);
        for (int i = 0; i < rowCount(); ++i) {
            const auto &message = messages[i];
            const auto &visible = message.contentMessage();
            bool changed = emoji;
            if (message.author.hasValue() && avatarUrlResolver && avatarUrlResolver(message.author.get(), currentGuildId) == url) changed = true;
            if (visible.attachments.hasValue()) for (const auto &attachment : visible.attachments.get()) if (attachment.proxyUrl.valueOr(std::string{}) == url) changed = true;
            if (visible.embeds.hasValue()) for (const auto &embed : visible.embeds.get()) {
                if (embed.image.hasValue() && embed.image->proxyUrl.valueOr(std::string{}) == url) changed = true;
                if (embed.thumbnail.hasValue() && embed.thumbnail->proxyUrl.valueOr(std::string{}) == url) changed = true;
                if (embed.video.hasValue() && embed.video->proxyUrl.valueOr(std::string{}) == url) changed = true;
                if (embed.author.hasValue() && embed.author->proxyIconUrl.valueOr(std::string{}) == url) changed = true;
                if (embed.footer.hasValue() && embed.footer->proxyIconUrl.valueOr(std::string{}) == url) changed = true;
            }
            if (auto it = forwardOriginCache.find(message.id); it != forwardOriginCache.end() && it->second.iconUrl == url) changed = true;
            if (changed) { embedCache.erase(message.id); sizeCache.erase(message.id); rowsChanged.publish(i, i); }
        }
    }));
}
void ChatModel::refreshRowsShowingSticker(const std::string &url)
{
    auto id = Discord::Cdn::stickerIdFromUrl(url);
    for (int i = 0; i < rowCount(); ++i) {
        const auto &stickers = messages[i].contentMessage().stickerItems;
        if (stickers.hasValue() && std::any_of(stickers->begin(), stickers->end(), [id](const auto &sticker) { return sticker.id.get() == id; })) rowsChanged.publish(i, i);
    }
}
void ChatModel::setAvatarUrlResolver(AvatarUrlResolver resolver) { avatarUrlResolver = std::move(resolver); }
void ChatModel::setDisplayNameResolver(DisplayNameResolver resolver) { displayNameResolver = std::move(resolver); }
void ChatModel::setRoleColorResolver(RoleColorResolver resolver) { roleColorResolver = std::move(resolver); }
void ChatModel::setChannelNameResolver(ChannelNameResolver resolver) { channelNameResolver = std::move(resolver); }
void ChatModel::setGuildInfoResolver(GuildInfoResolver resolver) { guildInfoResolver = std::move(resolver); }
std::string ChatModel::resolveAuthorName(const Discord::User &author) const
{
    if (displayNameResolver) { auto name = displayNameResolver(author, currentGuildId); if (!name.empty()) return name; }
    return author.getDisplayName();
}
Platform::Color ChatModel::resolveAuthorColor(const Discord::User &author) const
{
    return roleColorResolver && currentGuildId.isValid() ? roleColorResolver(author.id.get(), currentGuildId) : Platform::Color{};
}
ForwardOriginData ChatModel::forwardOrigin(const Discord::Message &message) const
{
    ForwardOriginData result;
    if (!message.messageReference.hasValue()) return result;
    const auto &reference = message.messageReference.get();
    result.channelId = reference.channelId.valueOr(Snowflake::Invalid);
    if (reference.guildId.hasValue() && reference.guildId.get() != currentGuildId && guildInfoResolver) {
        auto [name, icon] = guildInfoResolver(reference.guildId.get());
        if (!name.empty()) { result.text = name; result.iconUrl = icon; }
    }
    if (result.text.empty() && channelNameResolver) { auto name = channelNameResolver(result.channelId); if (!name.empty()) result.text = "#" + name; }
    if (result.text.empty()) return result;
    if (message.snapshotMessage && message.snapshotMessage->timestamp.hasValue()) {
        auto time = message.snapshotMessage->timestamp.get();
        result.text += "  •  " + time.format(time.sameDate(Platform::DateTime::now()) ? "%H:%M" : "%b %d, %Y %I:%M %p");
    }
    result.text += "  ›";
    return result;
}
int ChatModel::rowCount() const { return static_cast<int>(messages.size()); }
const Discord::Message *ChatModel::messageAt(int index) const
{
    if (index < 0 || index >= rowCount()) return nullptr;
    return &messages[index];
}
ChatRow ChatModel::row(int index) const
{
    ChatRow result;
    if (index < 0 || index >= rowCount()) return result;
    const auto &message = messages[index];
    const auto &visible = message.contentMessage();
    const bool isV2 = visible.flags.hasValue() && visible.flags->testFlag(Discord::MessageFlag::IS_COMPONENTS_V2);
    result.messageId = message.id;
    result.messageType = message.type.get();
    result.content = isV2 ? std::string{} : visible.content.valueOr(std::string{});
    result.timestamp = message.timestamp.get();
    result.editedTimestamp = message.editedTimestamp.valueOr(Platform::DateTime{});
    result.isSystemMessage = isSystemMessageType(result.messageType);
    result.isForwarded = message.isForwarded();
    result.dateSeparator = index == 0 || !messages[index - 1].timestamp->sameDate(result.timestamp);
    result.showHeader = !result.isSystemMessage && (result.messageType == Discord::MessageType::REPLY || index == 0
        || result.dateSeparator || messages[index - 1].author->id != message.author->id
        || isSystemMessageType(messages[index - 1].type.get())
        || std::abs(messages[index - 1].timestamp->secondsUntil(result.timestamp)) > 7 * 60);
    if (message.author.hasValue()) {
        result.username = resolveAuthorName(message.author.get());
        result.usernameColor = resolveAuthorColor(message.author.get());
        result.userId = message.author->id;
        if (avatarUrlResolver) result.avatar = imageManager->get(avatarUrlResolver(message.author.get(), currentGuildId), {32, 32}, currentAccountId, Core::PinGroup::ChatView);
        else result.avatar = imageManager->placeholder({32, 32});
    }
    if (auto it = sizeCache.find(message.id); it != sizeCache.end()) result.cachedSize = it->second;
    if (message.nonce.hasValue()) { result.isPending = pendingNonces.contains(message.nonce.get()); result.isErrored = erroredNonces.contains(message.nonce.get()); }
    if (!isV2) {
        result.html = visible.hidesLinkBehindItsEmbed() ? std::string{} : visible.parsedContentCached;
        if (result.messageType == Discord::MessageType::THREAD_CREATED) {
            auto name = Platform::htmlEscape(message.content.valueOr(std::string{}));
            auto id = message.messageReference.hasValue() ? message.messageReference->channelId.valueOr(Snowflake::Invalid) : Snowflake::Invalid;
            if (!id.isValid() && message.flags.hasValue() && message.flags->testFlag(Discord::MessageFlag::HAS_THREAD)) id = message.id;
            auto author = Platform::htmlEscape(result.username);
            auto authorHtml = result.usernameColor.valid ? "<span style='color:" + result.usernameColor.css() + ";font-weight:600'>" + author + "</span>" : "<b>" + author + "</b>";
            if (name.empty()) name = "a thread";
            result.html = authorHtml + " started a thread: " + (id.isValid() ? "<a href='styx://channel/" + id.toString() + "'>" + name + "</a>" : "<b>" + name + "</b>");
        } else if (result.messageType == Discord::MessageType::THREAD_STARTER_MESSAGE) {
            result.html = message.referencedMessage ? (message.referencedMessage->parsedContentCached.empty() ? Platform::htmlEscape(message.referencedMessage->content.valueOr(std::string{})) : message.referencedMessage->parsedContentCached)
                : "Sorry, we couldn't load the first message in this thread.";
        } else {
            if (message.flags.hasValue() && message.flags->testFlag(Discord::MessageFlag::HAS_THREAD)) result.html += (result.html.empty() ? "" : "<br/>") + std::string("<a href='styx://channel/") + message.id->toString() + "'>View Thread</a>";
            if (result.isForwarded) result.html = "<i>Forwarded</i>" + (result.html.empty() ? std::string{} : "<br/>" + result.html);
        }
    }
    auto fetch = [&](const std::string &url, Platform::Size size) { return suppressImageFetch ? imageManager->getIfCached(url, size) : imageManager->get(url, size, currentAccountId); };
    if (!isV2 && visible.attachments.hasValue()) {
        const std::vector<std::pair<int64_t, int64_t>> *progress = nullptr;
        if (message.nonce.hasValue()) if (auto it = uploadProgress.find(message.nonce.get()); it != uploadProgress.end()) progress = &it->second;
        for (const auto &attachment : visible.attachments.get()) {
            AttachmentData data{};
            data.id = attachment.id;
            data.originalUrl = attachment.url.valueOr(std::string{});
            data.proxyUrl = attachment.proxyUrl.valueOr(data.originalUrl);
            auto media = mediaFlagsFor(attachment, visible);
            data.isImage = media.isImage; data.isVideo = media.isVideo; data.isAudio = media.isAudio;
            data.isVoiceMessage = media.isVoiceMessage; data.durationMs = media.durationMs; data.contentType = media.contentType;
            data.filename = attachment.filename.valueOr(std::string("unknown")); data.fileSizeBytes = attachment.size.valueOr(0); data.isSpoiler = attachment.isSpoiler();
            if (progress && result.attachments.size() < progress->size()) { data.uploadSent = (*progress)[result.attachments.size()].first; data.uploadTotal = (*progress)[result.attachments.size()].second; }
            if (data.isMedia()) {
                Platform::Size original{attachment.width.valueOr(0), attachment.height.valueOr(0)};
                if (original.empty() && data.isVideo) {
                    auto it = videoNativeSizes.find(data.id);
                    original = it != videoNativeSizes.end() ? it->second : Platform::Size{1280, 720};
                }
                data.displaySize = Core::ImageManager::calculateDisplaySize(original);
                if (data.isImage) data.animatedUrl = animatedUrlFor(data.proxyUrl, data.originalUrl, attachment.isFlaggedAnimated(), original, data.displaySize);
                if (!data.animatedUrl.empty()) data.favoriteGif = {data.originalUrl, data.proxyUrl, {}, Proto::GifType::Image, original};
                if (!attachment.localPreview.empty()) { data.pixmap = previewPixmap(data.id, attachment.localPreview, data.displaySize); data.isLoading = data.pixmap.empty(); }
                else if (!Platform::localFilePath(data.proxyUrl).empty()) { data.pixmap = localPixmap(data.proxyUrl, data.displaySize); data.isLoading = data.pixmap.empty(); }
                else { data.pixmap = fetch(data.proxyUrl, data.displaySize); data.isLoading = !imageManager->isCached(data.proxyUrl, data.displaySize); }
            }
            result.attachments.push_back(std::move(data));
        }
    }
    if (!isV2 && visible.stickerItems.hasValue()) {
        const Platform::Size size{StickerData::DisplayPx, StickerData::DisplayPx};
        int asset = Discord::Cdn::stickerAssetPx(StickerData::DisplayPx, Platform::primaryDevicePixelRatio());
        for (const auto &sticker : visible.stickerItems.get()) {
            StickerData data;
            data.id = sticker.id; data.name = sticker.name;
            data.stillUrl = Discord::Cdn::stickerStill(sticker.id.get(), sticker.formatType.get(), asset);
            data.animatedUrl = Discord::Cdn::stickerAnimated(sticker.id.get(), sticker.formatType.get(), asset);
            if (data.hasRasterStill()) data.still = pixmapOrNullWhileLoading(data.stillUrl, size);
            data.unavailable = imageManager->isUnavailable({data.hasRasterStill() ? data.stillUrl : data.animatedUrl, size});
            result.stickers.push_back(std::move(data));
        }
    }
    if (auto it = isV2 ? embedCache.end() : embedCache.find(message.id); it != embedCache.end()) result.embeds = it->second;
    else if (!isV2 && visible.embeds.hasValue()) {
        std::unordered_map<std::string, size_t> merged;
        Core::Markdown::Parser parser;
        auto parse = [&](const std::string &text, bool links) {
            Core::Markdown::ParseState state; state.isInline = true;
            if (!links) state.excludedRules.insert("link");
            return text.empty() ? std::string{} : parser.toHtml(parser.parse(text, state));
        };
        auto imageFor = [&](const Discord::Embed &embed) {
            EmbedImageData image;
            image.url = embed.image->proxyUrl.get(); image.originalUrl = embed.image->url.valueOr(image.url);
            Platform::Size original{embed.image->width.valueOr(0), embed.image->height.valueOr(0)};
            image.displaySize = Core::ImageManager::calculateDisplaySize(original);
            image.animatedUrl = animatedUrlFor(image.url, image.originalUrl, embed.image->isFlaggedAnimated(), original, image.displaySize);
            if (!image.animatedUrl.empty()) image.favoriteGif = favoriteEmbedImage(embed, original);
            image.pixmap = fetch(image.url, image.displaySize);
            return image;
        };
        for (const auto &embed : visible.embeds.get()) {
            const auto url = embed.url.valueOr(std::string{});
            bool hasImage = embed.image.hasValue() && embed.image->proxyUrl.hasValue() && embed.image->contentType.hasValue() && embed.image->contentType->starts_with("image/");
            if (!url.empty() && hasImage) if (auto it = merged.find(url); it != merged.end()) {
                if (result.embeds[it->second].images.size() < 4) result.embeds[it->second].images.push_back(imageFor(embed));
                continue;
            }
            EmbedData data;
            bool anything = embed.title.hasValue() || embed.description.hasValue() || embed.timestamp.hasValue() || embed.color.hasValue() || embed.author.hasValue() || embed.footer.hasValue() || hasImage;
            data.type = embedTypeFromString(embed.type.valueOr(std::string{}));
            data.title = embed.title.valueOr(std::string{}); data.description = embed.description.valueOr(std::string{}); data.url = url;
            data.timestamp = embed.timestamp.valueOr(Platform::DateTime{});
            data.color = embed.color.hasValue() ? Platform::Color::rgb(embed.color.get()) : Core::Theme::Manager::instance().color(Core::Theme::Token::EmbedDefault);
            data.titleParsed = parse(data.title, false); data.descriptionParsed = parse(data.description, true);
            if (embed.author.hasValue()) {
                data.authorName = embed.author->name.valueOr(std::string{}); data.authorUrl = embed.author->url.valueOr(std::string{});
                data.authorIconUrl = embed.author->proxyIconUrl.valueOr(std::string{});
                if (!data.authorIconUrl.empty()) data.authorIcon = fetch(data.authorIconUrl, {24, 24});
            }
            if (embed.footer.hasValue()) {
                data.footerText = embed.footer->text.valueOr(std::string{}); data.footerIconUrl = embed.footer->proxyIconUrl.valueOr(std::string{});
                if (!data.footerIconUrl.empty()) data.footerIcon = fetch(data.footerIconUrl, {20, 20});
            }
            if (embed.provider.hasValue()) { data.providerName = embed.provider->name.valueOr(std::string{}); data.providerUrl = embed.provider->url.valueOr(std::string{}); }
            if (embed.thumbnail.hasValue() && embed.thumbnail->proxyUrl.hasValue() && embed.thumbnail->width.valueOr(0) > 0) {
                anything = true;
                data.thumbnailUrl = embed.thumbnail->proxyUrl.get(); data.thumbnailOriginalUrl = embed.thumbnail->url.valueOr(data.thumbnailUrl);
                Platform::Size original{embed.thumbnail->width.valueOr(0), embed.thumbnail->height.valueOr(0)};
                data.thumbnailSize = data.type == EmbedType::Gifv || data.type == EmbedType::Image ? Core::ImageManager::calculateDisplaySize(original) : original.empty() ? Platform::Size{80, 80} : Platform::fitWithin(original, {80, 80});
                if (data.type != EmbedType::Gifv) data.thumbnailAnimatedUrl = animatedUrlFor(data.thumbnailUrl, data.thumbnailOriginalUrl, embed.thumbnail->isFlaggedAnimated(), original, data.thumbnailSize);
                if (data.type == EmbedType::Image && !data.thumbnailAnimatedUrl.empty()) data.favoriteGif = {data.thumbnailOriginalUrl, data.thumbnailUrl, {}, Proto::GifType::Image, original};
                data.thumbnail = fetch(data.thumbnailUrl, data.thumbnailSize);
            }
            if (hasImage) data.images.push_back(imageFor(embed));
            if (embed.video.hasValue()) {
                auto mediaUrl = embed.video->proxyUrl.valueOr(std::string{});
                auto contentType = embed.video->contentType.valueOr(std::string{});
                Platform::Size natural{embed.video->width.valueOr(0), embed.video->height.valueOr(0)};
                if (data.type == EmbedType::Gifv) {
                    if (Core::Media::isSupported() && !data.thumbnailUrl.empty() && Discord::Cdn::isDiscordAssetUrl(mediaUrl) && Discord::Cdn::fitsLoopingVideoLimit(natural)) {
                        data.loopingVideoUrl = mediaUrl; data.favoriteGif = {url, mediaUrl, klipyThumbnailUrl(embed), Proto::GifType::Video, natural};
                    }
                } else if (!mediaUrl.empty() && Core::Media::canPlay(contentType, mediaUrl)) { data.videoUrl = mediaUrl; data.videoPlayable = true; anything = true; }
                std::string poster;
                Platform::Size posterSize;
                if (embed.thumbnail.hasValue() && embed.thumbnail->proxyUrl.valueOr(std::string{}).starts_with("https://")) {
                    poster = embed.thumbnail->proxyUrl.get(); posterSize = {embed.thumbnail->width.valueOr(0), embed.thumbnail->height.valueOr(0)};
                } else if (data.videoPlayable) { poster = mediaUrl; posterSize = natural; }
                if (!poster.empty()) {
                    anything = true; data.videoThumbnailUrl = poster;
                    data.videoThumbnailSize = Core::ImageManager::calculateDisplaySize(posterSize);
                    data.videoThumbnail = fetch(poster, data.videoThumbnailSize);
                }
                if (data.videoPlayable && data.videoThumbnailSize.empty()) data.videoThumbnailSize = Core::ImageManager::calculateDisplaySize(natural);
            }
            if (embed.fields.hasValue()) for (const auto &field : embed.fields.get()) {
                anything = true;
                EmbedFieldData value;
                value.name = field.name.valueOr(std::string{}); value.value = field.value.valueOr(std::string{}); value.isInline = field.isInline.valueOr(false);
                value.nameParsed = parse(value.name, false); value.valueParsed = parse(value.value, true);
                data.fields.push_back(std::move(value));
            }
            if (anything) { if (!url.empty()) merged[url] = result.embeds.size(); result.embeds.push_back(std::move(data)); }
        }
        if (!suppressImageFetch) embedCache[message.id] = result.embeds;
    }
    if (message.reactions.hasValue()) for (const auto &reaction : message.reactions.get()) {
        Platform::ImageBuffer pixels;
        Snowflake emojiId;
        std::string url;
        bool loading = false;
        if (!reaction.emoji->isUnicode()) {
            emojiId = reaction.emoji->id;
            url = reaction.emoji->getImageUrl(reaction.emoji->isAnimated());
            pixels = fetch(url, {16, 16}); loading = !imageManager->isCached(url, {16, 16});
        }
        auto add = [&](int count, bool burst) {
            if (count <= 0) return;
            ReactionData data;
            data.emojiName = reaction.emoji->name; data.emojiId = emojiId; data.emojiUrl = url; data.emojiAnimated = reaction.emoji->isAnimated();
            data.count = count; data.me = burst ? reaction.meBurst.valueOr(false) : reaction.me.get(); data.isBurst = burst;
            data.emojiPixmap = pixels; data.isLoading = loading;
            if (burst) data.burstTintColor = reaction.getBrightestBurstColor();
            result.reactions.push_back(std::move(data));
        };
        add(reaction.superCount(), true); add(reaction.normalCount(), false);
    }
    if (result.isForwarded && message.messageReference.hasValue() && message.messageReference->channelId.hasValue()) {
        auto [it, inserted] = forwardOriginCache.try_emplace(message.id);
        if (inserted) it->second = forwardOrigin(message);
        result.forwardOrigin = it->second;
        if (!result.forwardOrigin.iconUrl.empty()) result.forwardOrigin.icon = fetch(result.forwardOrigin.iconUrl, {16, 16});
    }
    if (result.messageType == Discord::MessageType::REPLY) {
        auto &reply = result.replyData;
        if (!message.referencedMessage) {
            reply.state = message.referencedMessageNull ? ReplyData::State::Deleted : ReplyData::State::Unknown;
            if (message.messageReference.hasValue()) reply.referencedMessageId = message.messageReference->messageId.valueOr(Snowflake::Invalid);
        } else {
            const auto &reference = *message.referencedMessage;
            reply.state = ReplyData::State::Present; reply.referencedMessageId = reference.id; reply.authorId = reference.author->id;
            const auto &content = reference.contentMessage(); reply.contentSnippet = content.content.valueOr(std::string{});
            if (reply.contentSnippet.empty() && content.stickerItems.hasValue() && !content.stickerItems->empty()) reply.contentSnippet = "Click to see sticker";
            reply.authorColor = resolveAuthorColor(reference.author.get()); reply.authorName = resolveAuthorName(reference.author.get());
        }
    }
    return result;
}
bool ChatModel::setCachedSize(int row, Platform::Size size) { if (row < 0 || row >= rowCount()) return false; sizeCache[messages[row].id] = size; return true; }
Snowflake ChatModel::getOldestMessageId() const { return messages.empty() ? Snowflake::Invalid : messages.front().id.get(); }
Snowflake ChatModel::getNewestMessageId() const { return messages.empty() ? Snowflake::Invalid : messages.back().id.get(); }
int ChatModel::rowForMessage(Snowflake id) const { for (int i = 0; i < rowCount(); ++i) if (messages[i].id == id) return i; return -1; }
Snowflake ChatModel::getActiveChannelId() const { return currentChannelId; }
Snowflake ChatModel::getActiveGuildId() const { return currentGuildId; }
void ChatModel::setAtLatest(bool value) { if (atLatest == value) return; atLatest = value; atLatestChanged.publish(value); }
void ChatModel::resetMessages(const std::vector<Discord::Message> &incoming)
{
    sizeCache.clear(); embedCache.clear(); forwardOriginCache.clear(); pendingNonces.clear(); erroredNonces.clear(); uploadProgress.clear();
    localPixmapCache.clear(); previewPixmapCache.clear(); mediaFlagsCache.clear();
    messages = incoming;
    modelReset.publish();
}
void ChatModel::appendMessages(const std::vector<Discord::Message> &incoming)
{
    if (incoming.empty()) return;
    int first = rowCount(); messages.insert(messages.end(), incoming.begin(), incoming.end()); rowsInserted.publish(first, rowCount() - 1);
}
void ChatModel::handleIncomingMessages(const Core::MessageRequestResult &result)
{
    if (!result.success || result.channelId != currentChannelId) return;
    auto incoming = result.messages;
    std::sort(incoming.begin(), incoming.end(), [](const auto &a, const auto &b) { return a.id.get() < b.id.get(); });
    using Type = Discord::Client::MessageLoadType;
    switch (result.type) {
    case Type::Latest: resetMessages(incoming); setAtLatest(true); break;
    case Type::Jump: if (!incoming.empty()) { resetMessages(incoming); setAtLatest(result.reachedLatest); } break;
    case Type::Future:
        if (result.anchorId == getNewestMessageId()) { appendMessages(incoming); if (result.reachedLatest) setAtLatest(true); }
        break;
    case Type::History:
        if (!incoming.empty() && result.anchorId == getOldestMessageId()) {
            auto anchor = getOldestMessageId();
            int count = static_cast<int>(incoming.size()); messages.insert(messages.begin(), incoming.begin(), incoming.end());
            rowsInserted.publish(0, count - 1); sizeCache.erase(anchor); rowsChanged.publish(count, count);
        }
        break;
    case Type::Created:
        for (const auto &message : incoming) {
            int row = rowForMessage(message.id);
            if (row < 0 && message.nonce.hasValue()) for (int i = 0; i < rowCount(); ++i)
                if (messages[i].nonce.hasValue() && messages[i].nonce.get() == message.nonce.get()) { row = i; break; }
            if (row >= 0) {
                auto previous = messages[row].id.get();
                prunePreviewCaches(messages[row]);
                if (messages[row].nonce.hasValue()) { pendingNonces.erase(messages[row].nonce.get()); uploadProgress.erase(messages[row].nonce.get()); }
                messages[row] = message; sizeCache.erase(previous); embedCache.erase(previous); forwardOriginCache.erase(previous);
                rowsChanged.publish(row, std::min(row + 1, rowCount() - 1));
            } else if (atLatest) {
                if (message.isPendingOutbound && message.nonce.hasValue()) pendingNonces.insert(message.nonce.get());
                int at = rowCount(); messages.push_back(message); rowsInserted.publish(at, at);
            }
        }
        break;
    default: break;
    }
}
void ChatModel::handleMessageDeleted(Snowflake channel, Snowflake id)
{
    if (channel != currentChannelId) return;
    int at = rowForMessage(id);
    if (at < 0) return;
    if (messages[at].nonce.hasValue()) { auto nonce = messages[at].nonce.get(); pendingNonces.erase(nonce); erroredNonces.erase(nonce); uploadProgress.erase(nonce); }
    prunePreviewCaches(messages[at]);
    sizeCache.erase(id); embedCache.erase(id); forwardOriginCache.erase(id);
    messages.erase(messages.begin() + at); rowsRemoved.publish(at, at);
    if (at < rowCount()) { sizeCache.erase(messages[at].id); rowsChanged.publish(at, at); }
}
void ChatModel::handleMessageErrored(const std::string &nonce)
{
    pendingNonces.erase(nonce); erroredNonces.insert(nonce); uploadProgress.erase(nonce);
    for (int i = 0; i < rowCount(); ++i) if (messages[i].nonce.hasValue() && messages[i].nonce.get() == nonce) { rowsChanged.publish(i, i); break; }
}
void ChatModel::handleUploadProgress(const std::string &nonce, int file, int64_t sent, int64_t total)
{
    if (file < 0 || !pendingNonces.contains(nonce)) return;
    auto &progress = uploadProgress[nonce];
    if (progress.size() <= static_cast<size_t>(file)) progress.resize(static_cast<size_t>(file) + 1, {-1, -1});
    progress[file] = {sent, total};
    for (int i = 0; i < rowCount(); ++i) if (messages[i].nonce.hasValue() && messages[i].nonce.get() == nonce) { rowsChanged.publish(i, i); break; }
}
Platform::ImageBuffer ChatModel::localPixmap(const std::string &url, const Platform::Size &size) const
{
    if (auto it = localPixmapCache.find(url); it != localPixmapCache.end()) return it->second;
    auto decoded = Platform::Image::decodeFile(Platform::localFilePath(url));
    auto image = decoded ? previewPixmap(Snowflake::Invalid, *decoded, size) : Platform::ImageBuffer{};
    localPixmapCache[url] = image;
    previewPixmapCache.erase(Snowflake::Invalid);
    return image;
}
Platform::ImageBuffer ChatModel::pixmapOrNullWhileLoading(const std::string &url, const Platform::Size &size) const
{
    auto image = suppressImageFetch ? imageManager->getIfCached(url, size) : imageManager->get(url, size, currentAccountId);
    return imageManager->isCached(url, size) ? image : Platform::ImageBuffer{};
}
Platform::ImageBuffer ChatModel::previewPixmap(Snowflake id, const Platform::ImageBuffer &image, const Platform::Size &size) const
{
    if (auto it = previewPixmapCache.find(id); it != previewPixmapCache.end()) return it->second;
    auto result = image;
    if (!image.empty() && !size.empty()) {
        auto scale = Platform::primaryDevicePixelRatio();
        auto target = Platform::fitWithin({image.width, image.height}, {int(size.width * scale), int(size.height * scale)});
        if (!target.empty()) result = Platform::Image::scale(image, target.width, target.height);
    }
    previewPixmapCache[id] = result;
    return result;
}
void ChatModel::prunePreviewCaches(const Discord::Message &message)
{
    if (message.attachments.hasValue()) for (const auto &attachment : message.attachments.get()) {
        previewPixmapCache.erase(attachment.id); if (attachment.proxyUrl.hasValue()) localPixmapCache.erase(attachment.proxyUrl.get()); mediaFlagsCache.erase(attachment.id);
    }
}
void ChatModel::setAccount(Snowflake id) { if (currentAccountId == id) return; currentAccountId = id; currentChannelId = Snowflake::Invalid; resetMessages({}); }
void ChatModel::setActiveChannel(Snowflake channel, Snowflake guild)
{
    if (currentChannelId == channel && currentGuildId == guild) return;
    imageManager->unpinGroup(Core::PinGroup::ChatView);
    currentChannelId = channel; currentGuildId = guild; revealedSpoilers.clear(); videoNativeSizes.clear(); setAtLatest(true); resetMessages({});
}
void ChatModel::refreshUsersInView(const std::vector<Snowflake> &users)
{
    for (int i = 0; i < rowCount(); ++i) if (messages[i].author.hasValue() && (users.empty() || std::find(users.begin(), users.end(), messages[i].author->id.get()) != users.end())) rowsChanged.publish(i, i);
}
void ChatModel::revealSpoiler(Snowflake id)
{
    if (!revealedSpoilers.insert(id).second) return;
    for (int i = 0; i < rowCount(); ++i) {
        auto &attachments = messages[i].contentMessage().attachments;
        if (attachments.hasValue()) for (const auto &attachment : attachments.get()) if (attachment.id.get() == id) { sizeCache.erase(messages[i].id); rowsChanged.publish(i, i); return; }
    }
}
bool ChatModel::isSpoilerRevealed(Snowflake id) const { return revealedSpoilers.contains(id); }
ChatModel::MediaFlags ChatModel::mediaFlagsFor(const Discord::Attachment &attachment, const Discord::Message &message) const
{
    if (auto it = mediaFlagsCache.find(attachment.id); it != mediaFlagsCache.end()) return it->second;
    MediaFlags result;
    result.isImage = attachment.isImage(); result.contentType = attachment.contentType.valueOr(std::string{});
    auto url = attachment.url.valueOr(std::string{});
    bool voice = message.flags.hasValue() && message.flags->testFlag(Discord::MessageFlag::IS_VOICE_MESSAGE);
    result.isVideo = !result.isImage && Core::Media::canPlay(result.contentType, url);
    result.isVoiceMessage = !message.isPendingOutbound && Core::Media::isSupported() && !result.isImage && !result.isVideo && voice && (result.contentType.empty() || result.contentType.starts_with("audio/"));
    result.isAudio = result.isVoiceMessage || (!message.isPendingOutbound && !result.isImage && !result.isVideo && Core::Media::canPlayAudio(result.contentType, url));
    if (attachment.durationSecs.hasValue() && std::isfinite(attachment.durationSecs.get()) && attachment.durationSecs.get() > 0) result.durationMs = static_cast<int64_t>(std::min(attachment.durationSecs.get(), 86400.0) * 1000);
    if (!message.isPendingOutbound) mediaFlagsCache[attachment.id] = result;
    return result;
}
void ChatModel::setVideoNativeSize(Snowflake id, const Platform::Size &size)
{
    if (size.empty()) return;
    if (auto it = videoNativeSizes.find(id); it != videoNativeSizes.end() && it->second == size) return;
    videoNativeSizes[id] = size;
    for (int i = 0; i < rowCount(); ++i) {
        const auto &attachments = messages[i].contentMessage().attachments;
        if (attachments.hasValue()) for (const auto &attachment : attachments.get()) if (attachment.id.get() == id) { sizeCache.erase(messages[i].id); rowsChanged.publish(i, i); return; }
    }
}
void ChatModel::invalidateLayout() { sizeCache.clear(); if (!messages.empty()) rowsChanged.publish(0, rowCount() - 1); }
}
