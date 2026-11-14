#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Result.hpp"
#include "Core/Snowflake.hpp"
#include "UserSettings.hpp"

namespace Styx::Proto {

struct GuildPrivacyEdit
{
    enum class Kind : uint32_t { DirectMessages = 3, Activity = 13, ActivityJoining = 15, MessageRequests = 16 };
    Core::Snowflake guildId;
    bool restricted = false;
    Kind kind = Kind::DirectMessages;
};

struct GuildFolderMove
{
    enum class Kind { Folder, First, Last };
    Core::Snowflake guildId;
    std::optional<int64_t> folderId;
    Kind kind = Kind::Folder;
};

// Edit one explicit-content or gore filter context. RedactionUnset restores Discord's default.
// Unsupported filter fields, contexts and redactions are rejected by preloadedSettingsPatch.
struct ContentFilterEdit
{
    uint32_t filterField = 0;
    uint32_t context = 0;
    uint32_t redaction = 0;
};

struct PreloadedSettingsEdit
{
    std::optional<std::string> presenceStatus;
    std::optional<std::optional<CustomStatus>> customStatus; // outer unset means keep, inner unset means remove
    std::optional<bool> showCurrentGame;
    std::optional<uint64_t> statusExpiresAtMs;
    std::optional<uint32_t> theme;
    std::optional<bool> developerMode;
    std::optional<bool> defaultGuildsRestricted;
    std::optional<bool> allowAccessibilityDetection;
    std::optional<std::vector<Core::Snowflake>> restrictedGuildIds;
    std::optional<uint32_t> friendSourceFlags;
    std::optional<uint32_t> friendDiscoveryFlags;
    std::optional<bool> defaultMessageRequestRestricted;
    std::optional<uint32_t> dmSpamFilter; // TextAndImagesSettings::DmSpamFilter
    std::optional<ContentFilterEdit> contentFilter;
    std::optional<std::vector<GuildFolder>> guildFolders;
    std::optional<std::string> locale;
    std::optional<std::vector<Core::Snowflake>> guildPositions;
    std::optional<GuildPrivacyEdit> guildPrivacy;
    std::optional<GuildFolderMove> guildFolderMove;

    bool empty() const;
};

class PreloadedSettingsSnapshot
{
public:
    PreloadedSettingsSnapshot() = default;

    static Core::Result<PreloadedSettingsSnapshot> fromProto(std::string_view bytes);
    const std::string &bytes() const;
    std::optional<uint32_t> dataVersion() const;
    bool hasGroup(uint32_t fieldNumber) const;
    void mergeFrom(const PreloadedSettingsSnapshot &partial); // replace complete top-level fields present in partial, preserve omitted fields

private:
    std::string wireBytes;
    std::optional<uint32_t> version;
};

Core::Result<std::string> preloadedSettingsPatch(const PreloadedSettingsSnapshot &current, const PreloadedSettingsEdit &edit);

} // namespace Styx::Proto
