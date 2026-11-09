#pragma once

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

struct PreloadedUserSettings
{
    std::optional<GuildFolders> guildFolders;
    std::optional<StatusSettings> status;
    std::optional<LocalizationSettings> localization;

    static PreloadedUserSettings fromProto(ProtoReader &reader);
};

} // namespace Proto
} // namespace Styx
