#pragma once

#include <string>
#include "Core/Snowflake.hpp"
#include "Discord/Enums.hpp"
#include "Platform/Native.hpp"

namespace Styx::Discord::Cdn {

std::string assetExtension(const std::string &hash);
std::string defaultUserAvatar(Core::Snowflake userId, int size = 128);
std::string userAvatar(Core::Snowflake userId, const std::string &hash, int size = 128);
std::string guildMemberAvatar(Core::Snowflake guildId, Core::Snowflake userId, const std::string &hash, int size = 128);
std::string effectiveAvatar(Core::Snowflake guildId, Core::Snowflake userId, const std::string &memberHash, const std::string &userHash, int size = 128);
std::string userBanner(Core::Snowflake userId, const std::string &hash, int size = 600);
std::string guildIcon(Core::Snowflake guildId, const std::string &hash, int size = 64);
std::string guildBanner(Core::Snowflake guildId, const std::string &hash, int size = 480);
std::string guildSplash(Core::Snowflake guildId, const std::string &hash, int size = 480);
std::string roleIcon(Core::Snowflake roleId, const std::string &hash, int size = 64);
std::string applicationIcon(Core::Snowflake applicationId, const std::string &hash, int size = 128);
std::string channelIcon(Core::Snowflake channelId, const std::string &hash, int size = 64);
std::string badgeIcon(const std::string &hash, int size = 64);
std::string emoji(Core::Snowflake emojiId, int size = 48, bool animated = false);
bool isEmojiUrl(const std::string &url);
bool isAnimatedEmojiUrl(const std::string &url);
std::string stillEmojiUrl(const std::string &animatedUrl);
bool isStickerUrl(const std::string &url);
Core::Snowflake stickerIdFromUrl(const std::string &url);
[[nodiscard]] int stickerAssetPx(int logicalPx, double devicePixelRatio);
std::string stickerStill(Core::Snowflake stickerId, StickerFormatType format, int assetPx);
std::string stickerAnimated(Core::Snowflake stickerId, StickerFormatType format, int assetPx);
std::string connectionIcon(const std::string &type);
std::string activityAsset(Core::Snowflake applicationId, const std::string &key, int size);
bool isDiscordMediaUrl(const std::string &url);
bool isSigned(const std::string &url);
bool hasExpired(const std::string &url);
[[nodiscard]] bool isDiscordAssetUrl(const std::string &url);
[[nodiscard]] bool isAnimatedImage(const std::string &originalUrl, bool flaggedAnimated);
[[nodiscard]] std::string animatedImageUrl(const std::string &proxyUrl, bool flaggedAnimated, const Platform::Size &sourceSize, const Platform::Size &targetPixels);
[[nodiscard]] bool fitsLoopingVideoLimit(const Platform::Size &naturalSize);

} // namespace Styx::Discord::Cdn
