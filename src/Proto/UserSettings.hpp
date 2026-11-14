#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Core/Snowflake.hpp"

namespace Styx {
namespace Proto {

class ProtoReader;

struct GuildFolder
{
    std::vector<Core::Snowflake> guildIds;
    std::optional<int64_t> id;
    std::optional<std::string> name;
    std::optional<uint64_t> color;

    static GuildFolder fromProto(ProtoReader &reader);
};

struct GuildFolders
{
    std::vector<GuildFolder> folders;
    std::vector<Core::Snowflake> guildPositions;

    static GuildFolders fromProto(ProtoReader &reader);
};

struct CustomStatus
{
    std::string text;
    uint64_t emojiId = 0;
    std::string emojiName;
    uint64_t expiresAtMs = 0; // 0 never expires
    uint64_t createdAtMs = 0;

    static CustomStatus fromProto(ProtoReader &reader);
    std::string toProto() const;
};

struct StatusSettings
{
    std::optional<std::string> status;
    std::optional<CustomStatus> customStatus;
    std::optional<bool> showCurrentGame;
    uint64_t statusExpiresAtMs = 0;
    std::string unknownFields; // Preserve untouched siblings when replacing the status message.

    static StatusSettings fromProto(std::span<const uint8_t> bytes);
};

struct LocalizationSettings
{
    std::optional<std::string> locale;

    static LocalizationSettings fromProto(ProtoReader &reader);
};

struct PrivacySettings
{
    bool defaultGuildsRestricted = false;
    bool allowAccessibilityDetection = false;
    std::vector<Core::Snowflake> restrictedGuildIds;
    std::vector<Core::Snowflake> activityRestrictedGuildIds;
    std::vector<Core::Snowflake> activityJoiningRestrictedGuildIds;
    std::vector<Core::Snowflake> messageRequestRestrictedGuildIds;
    std::optional<uint32_t> friendSourceFlags;
    std::optional<uint32_t> friendDiscoveryFlags;
    // Message requests for server members are off by default when set (field 17).
    std::optional<bool> defaultMessageRequestRestricted;
    static PrivacySettings fromProto(ProtoReader &reader);
};

struct AppearanceSettings
{
    uint32_t theme = 0;
    bool developerMode = false;
    static AppearanceSettings fromProto(ProtoReader &reader);
};

struct TextAndImagesSettings
{
    // DmSpamFilterV2: 0 unset (acts as NonFriends), 1 off, 2 non-friends, 3 everyone.
    enum DmSpamFilter : uint32_t { SpamFilterUnset = 0, SpamFilterOff = 1, SpamFilterNonFriends = 2, SpamFilterEveryone = 3 };
    uint32_t dmSpamFilter = SpamFilterUnset;
    // Sensitive media filters: explicit_content_settings (29) and gore_content_settings (32), each
    // holding an ExplicitContentRedaction for server channels, friend DMs and other DMs (fields 1-3).
    enum Redaction : uint32_t { RedactionUnset = 0, RedactionShow = 1, RedactionBlur = 2, RedactionBlock = 3 };
    enum ContentContext : uint32_t { ContextGuilds = 1, ContextFriendDms = 2, ContextOtherDms = 3 };
    static constexpr uint32_t ExplicitContentField = 29;
    static constexpr uint32_t GoreContentField = 32;
    std::array<uint32_t, 3> explicitContent{}; // indexed by ContentContext - 1
    std::array<uint32_t, 3> goreContent{};
    static TextAndImagesSettings fromProto(ProtoReader &reader);
};

struct PreloadedUserSettings
{
    std::optional<GuildFolders> guildFolders;
    std::optional<StatusSettings> status;
    std::optional<LocalizationSettings> localization;
    std::optional<PrivacySettings> privacy;
    std::optional<AppearanceSettings> appearance;
    std::optional<TextAndImagesSettings> textAndImages;

    static PreloadedUserSettings fromProto(ProtoReader &reader);
};

} // namespace Proto
} // namespace Styx
