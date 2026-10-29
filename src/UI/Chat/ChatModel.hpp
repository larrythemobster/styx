#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "Platform/Trackable.hpp"
#include "Platform/Native.hpp"
#include "Platform/Image.hpp"
#include "Core/Gifs/FavoriteGifRules.hpp"
#include "Core/Session.hpp"
#include "Core/MessageManager.hpp"


using Styx::Core::ClientInstance;

namespace Styx {

namespace Core {
class ImageManager;
} // namespace Core

struct AttachmentData
{
    Snowflake id;
    std::string proxyUrl;
    std::string originalUrl;
    std::string animatedUrl;
    Core::FavoriteGifCandidate favoriteGif;
    Platform::Size displaySize;
    Platform::ImageBuffer pixmap;
    bool isLoading;
    bool isImage;
    bool isVideo = false;
    bool isAudio = false;
    bool isVoiceMessage = false;
    int64_t durationMs = 0;
    std::string contentType;
    std::string filename;
    int64_t fileSizeBytes;
    bool isSpoiler;
    int64_t uploadSent = -1;
    int64_t uploadTotal = -1;

    [[nodiscard]] bool isMedia() const { return isImage || isVideo; }
};

struct EmbedFieldData
{
    std::string name;
    std::string value;
    std::string nameParsed;
    std::string valueParsed;
    bool isInline = false;
};

struct EmbedImageData
{
    std::string url;
    std::string originalUrl;
    std::string animatedUrl;
    Core::FavoriteGifCandidate favoriteGif;
    Platform::ImageBuffer pixmap;
    Platform::Size displaySize;
};

enum class EmbedType {
    AgeVerificationSystemNotification,
    ApplicationNews,
    Article,
    AutoModerationMessage,
    AutoModerationNotification,
    Gift,
    Gifv,
    Image,
    Link,
    PollResult,
    PostPreview,
    Rich,
    SafetyPolicyNotice,
    SafetySystemNotification,
    Video,
};

struct ReplyData
{
    enum class State {
        None, // not a reply
        Present, // referenced message is available
        Deleted, // referenced message was deleted
        Unknown, // backend didn't fetch referenced message
    };

    State state = State::None;
    Core::Snowflake referencedMessageId;
    std::string authorName;
    Platform::Color authorColor;
    Core::Snowflake authorId;
    std::string contentSnippet;
};

struct ForwardOriginData
{
    Core::Snowflake channelId;
    std::string text;
    std::string iconUrl;
    Platform::ImageBuffer icon;
};

struct ReactionData
{
    std::string emojiName;
    Core::Snowflake emojiId;
    std::string emojiUrl;
    bool emojiAnimated = false;
    int count = 0;
    bool me = false;
    bool isBurst = false;
    Platform::ImageBuffer emojiPixmap;
    bool isLoading = false;
    Platform::Color burstTintColor;

    [[nodiscard]] Discord::Emoji emoji() const
    {
        return emojiId.isValid() ? Discord::Emoji::custom(emojiId, emojiName, emojiAnimated) : Discord::Emoji::unicode(emojiName);
    }
};

struct StickerData
{
    static constexpr int DisplayPx = 160;

    Core::Snowflake id;
    std::string name;
    std::string stillUrl;
    std::string animatedUrl;
    Platform::ImageBuffer still;
    bool unavailable = false;

    [[nodiscard]] bool isAnimated() const { return !animatedUrl.empty(); }
    [[nodiscard]] bool hasRasterStill() const { return !stillUrl.empty(); }
};

struct EmbedData
{
    EmbedType type = EmbedType::Rich; // should this be default idk
    std::string title;
    std::string description;
    std::string titleParsed;
    std::string descriptionParsed;
    std::string url;
    Platform::DateTime timestamp;
    Platform::Color color;

    std::string authorName;
    std::string authorUrl;
    std::string authorIconUrl;
    Platform::ImageBuffer authorIcon;

    std::string footerText;
    std::string footerIconUrl;
    Platform::ImageBuffer footerIcon;

    std::string thumbnailUrl;
    std::string thumbnailOriginalUrl;
    std::string thumbnailAnimatedUrl;
    Platform::ImageBuffer thumbnail;
    Platform::Size thumbnailSize;

    std::vector<EmbedImageData> images;

    std::string videoThumbnailUrl;
    Platform::ImageBuffer videoThumbnail;
    Platform::Size videoThumbnailSize;

    std::string videoUrl;
    bool videoPlayable = false;
    std::string loopingVideoUrl;
    Core::FavoriteGifCandidate favoriteGif;

    std::string providerName;
    std::string providerUrl;

    std::vector<EmbedFieldData> fields;
};

namespace UI {

struct ChatRow {
    std::string content, username, html;
    Snowflake userId, messageId;
    Platform::ImageBuffer avatar;
    Platform::DateTime timestamp, editedTimestamp;
    Platform::Size cachedSize;
    bool showHeader = false, dateSeparator = false;
    std::vector<AttachmentData> attachments;
    std::vector<EmbedData> embeds;
    bool isPending = false, isErrored = false;
    Platform::Color usernameColor;
    ReplyData replyData;
    std::vector<ReactionData> reactions;
    bool isSystemMessage = false;
    Discord::MessageType messageType{};
    bool isForwarded = false;
    ForwardOriginData forwardOrigin;
    std::vector<StickerData> stickers;
};

class ChatModel : public Platform::Trackable
{
public:
    explicit ChatModel(Core::ImageManager *imageManager);

    void setAccount(Snowflake accountId);
    [[nodiscard]] Snowflake getAccountId() const { return currentAccountId; }

    using AvatarUrlResolver = std::function<std::string(const Discord::User &author, Snowflake guildId)>;
    void setAvatarUrlResolver(AvatarUrlResolver resolver);

    using DisplayNameResolver = std::function<std::string(const Discord::User &author, Snowflake guildId)>;
    void setDisplayNameResolver(DisplayNameResolver resolver);

    using RoleColorResolver = std::function<Platform::Color(Snowflake userId, Snowflake guildId)>;
    void setRoleColorResolver(RoleColorResolver resolver);

    using ChannelNameResolver = std::function<std::string(Snowflake channelId)>;
    void setChannelNameResolver(ChannelNameResolver resolver);

    using GuildInfoResolver = std::function<std::pair<std::string, std::string>(Snowflake guildId)>;
    void setGuildInfoResolver(GuildInfoResolver resolver);

    [[nodiscard]] int rowCount() const;
    [[nodiscard]] ChatRow row(int index) const;
    bool setCachedSize(int index, Platform::Size size);

    [[nodiscard]] Snowflake getOldestMessageId() const;
    [[nodiscard]] Snowflake getNewestMessageId() const;
    [[nodiscard]] int rowForMessage(Snowflake messageId) const;
    [[nodiscard]] Snowflake getActiveChannelId() const;
    [[nodiscard]] Snowflake getActiveGuildId() const;
    [[nodiscard]] bool isSpoilerRevealed(Snowflake attachmentId) const;

    [[nodiscard]] bool isAtLatest() const { return atLatest; }

    void setVideoNativeSize(Snowflake attachmentId, const Platform::Size &size);

    void invalidateLayout();
    Platform::Signal<bool> atLatestChanged;
    Platform::Signal<int, int> rowsChanged, rowsInserted, rowsRemoved;
    Platform::Signal<> modelReset;

    void setActiveChannel(Snowflake channelId, Snowflake guildId = Snowflake::Invalid);
    void handleIncomingMessages(const Core::MessageRequestResult &result);
    void handleMessageDeleted(Snowflake channelId, Snowflake messageId);
    void handleMessageErrored(const std::string &nonce);
    void handleUploadProgress(const std::string &nonce, int fileIndex, int64_t sent, int64_t total);
    void refreshUsersInView(const std::vector<Snowflake> &userIds);
    void revealSpoiler(Snowflake attachmentId);

    void triggerResize(int row) { if (row >= 0 && row < rowCount()) rowsChanged.publish(row, row); }

private:
    void resetMessages(const std::vector<Discord::Message> &sortedIncoming);
    void appendMessages(const std::vector<Discord::Message> &sortedIncoming);
    void setAtLatest(bool value);
    std::string resolveAuthorName(const Discord::User &author) const;
    Platform::Color resolveAuthorColor(const Discord::User &author) const;
    ForwardOriginData forwardOrigin(const Discord::Message &msg) const;
    Platform::ImageBuffer localPixmap(const std::string &url, const Platform::Size &displaySize) const;
    Platform::ImageBuffer pixmapOrNullWhileLoading(const std::string &url, const Platform::Size &displaySize) const;
    Platform::ImageBuffer previewPixmap(Snowflake attachmentId, const Platform::ImageBuffer &image, const Platform::Size &displaySize) const;
    void prunePreviewCaches(const Discord::Message &msg);
    void refreshRowsShowingSticker(const std::string &stickerUrl);

    Core::ImageManager *imageManager;
    std::vector<Discord::Message> messages;
    mutable std::unordered_map<Snowflake, Platform::Size> sizeCache;
    mutable std::unordered_map<Snowflake, std::vector<EmbedData>> embedCache;
    mutable std::unordered_map<Snowflake, ForwardOriginData> forwardOriginCache;

    Snowflake currentChannelId = Snowflake::Invalid;
    Snowflake currentGuildId = Snowflake::Invalid;
    Snowflake currentAccountId;
    bool atLatest = true;

    AvatarUrlResolver avatarUrlResolver;
    DisplayNameResolver displayNameResolver;
    RoleColorResolver roleColorResolver;
    ChannelNameResolver channelNameResolver;
    GuildInfoResolver guildInfoResolver;

    std::unordered_set<std::string> pendingNonces;
    std::unordered_set<std::string> erroredNonces;
    std::unordered_map<std::string, std::vector<std::pair<int64_t, int64_t>>> uploadProgress; // by nonce
    mutable std::unordered_map<std::string, Platform::ImageBuffer> localPixmapCache; // file previews
    mutable std::unordered_map<Snowflake, Platform::ImageBuffer> previewPixmapCache; // pasted bitmap previews by attachment id
    mutable std::unordered_set<Snowflake> revealedSpoilers;
    std::unordered_map<Snowflake, Platform::Size> videoNativeSizes;

    struct MediaFlags
    {
        bool isImage = false;
        bool isVideo = false;
        bool isAudio = false;
        bool isVoiceMessage = false;
        int64_t durationMs = 0;
        std::string contentType;
    };

    [[nodiscard]] MediaFlags mediaFlagsFor(const Discord::Attachment &attachment, const Discord::Message &message) const;

    mutable std::unordered_map<Snowflake, MediaFlags> mediaFlagsCache;
    mutable bool suppressImageFetch = false;

};
} // namespace UI
} // namespace Styx

