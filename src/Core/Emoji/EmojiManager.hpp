#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Platform/Trackable.hpp"
#include "Platform/Timer.hpp"

#include <cstdint>
#include <optional>

#include "Core/Emoji/EmojiMatch.hpp"
#include "Core/Emoji/FrecencyTracker.hpp"
#include "Core/Emoji/PickerEmoji.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"
#include "Discord/Events.hpp"

namespace Styx {

namespace Discord {
class Client;
}

namespace Proto {
struct FrecencyUserSettings;
}

namespace Core {

struct UnicodeEmoji;
class ClientInstance;

class EmojiManager : public Platform::Trackable
{
public:
    EmojiManager(Discord::Client *client, ClientInstance *instance);

    void setGuildEmojis(Snowflake guildId, const std::vector<Discord::Emoji> &emojis);
    void removeGuild(Snowflake guildId);

    [[nodiscard]] std::vector<EmojiMatch> search(const std::string &query, Snowflake channelId, int maxResults = 10);

    [[nodiscard]] std::vector<PickerSection> pickerSections(Snowflake channelId, EmojiIntention intention);
    [[nodiscard]] std::vector<PickerEmoji> searchPicker(const std::string &query, Snowflake channelId, EmojiIntention intention);
    [[nodiscard]] std::vector<PickerEmoji> frequentlyUsed(Snowflake channelId, EmojiIntention intention);
    [[nodiscard]] std::vector<PickerEmoji> quickReactions(Snowflake channelId, int count);

    void trackMessageEmojis(const std::string &content);
    void trackReaction(const std::string &reactionEmoji);

    // write back to settings-proto/2
    void savePendingUsage();

public:
    void onReady();
    void onGuildEmojisUpdated(const Discord::GuildEmojisUpdate &event);
    void onUserSettingsProtoUpdated(const Discord::UserSettingsProtoUpdate &event);
    void onFrecencySettingsReceived(const Proto::FrecencyUserSettings &settings);

private:
    struct CustomEmoji
    {
        Discord::Emoji emoji;
        Snowflake guildId;
        std::string originalName;
        // `name~1`, `name~2`
        std::string disambiguatedName;
        std::string normalizedName;
    };

    struct UnicodeCandidate
    {
        const UnicodeEmoji *emoji = nullptr;
        std::vector<std::string> normalizedNames;
    };

    struct PendingUsage
    {
        std::string key;
        uint64_t timestamp = 0;
        bool reaction = false;
    };

    // for querying emoji usability
    struct UsabilityContext
    {
        Snowflake channelId;
        Snowflake channelGuildId;
        Snowflake selfId;
        bool premium = false;
        std::unordered_map<Snowflake, std::unordered_set<Snowflake>> rolesByGuild;
        std::optional<bool> externalEmojisAllowed;
    };

    struct RankedEmoji
    {
        const UnicodeEmoji *unicode = nullptr;
        const CustomEmoji *custom = nullptr;
    };

    void fetchSettings();
    void adoptSettings(const Proto::FrecencyUserSettings &settings, bool partial);
    void replayPendingUsages();
    void seedDefaultsIfEmpty();

    void trackUsage(const std::string &key, bool reaction);
    void trackSurrogates(const std::string &content);

    void onSyncTimer();
    void scheduleSync(int milliseconds);

    const std::vector<CustomEmoji> &customEmojis();
    const std::vector<UnicodeCandidate> &unicodeCandidates();
    bool isUsable(const CustomEmoji &custom, UsabilityContext &context) const;
    [[nodiscard]] UsabilityContext usabilityIn(Snowflake channelId) const;
    [[nodiscard]] std::vector<RankedEmoji> rankedMatches(const std::string &query, Snowflake channelId, FrecencyTracker &tracker);
    [[nodiscard]] std::vector<Snowflake> guildsInSidebarOrder() const;
    [[nodiscard]] FrecencyTracker &trackerFor(EmojiIntention intention);

    [[nodiscard]] static PickerEmoji toPickerEmoji(const UnicodeEmoji &emoji);
    [[nodiscard]] static PickerEmoji toPickerEmoji(const CustomEmoji &custom);
    [[nodiscard]] static std::string unicodeCategoryTitle(const std::string &categoryId);

    [[nodiscard]] static uint64_t nowMs();
    [[nodiscard]] static std::string unicodeKey(const std::string &surrogates);

    Discord::Client *client;
    ClientInstance *instance;

    std::unordered_map<Snowflake /*guildId*/, std::vector<Discord::Emoji>> guildEmojis;
    std::vector<CustomEmoji> customCandidates;
    std::unordered_map<Snowflake, int> customCandidateIndex;
    bool customCandidatesDirty = true;
    std::vector<UnicodeCandidate> unicodeCandidateCache;

    FrecencyTracker chatTracker;
    FrecencyTracker reactionTracker;

    std::vector<PendingUsage> pendingUsages;
    std::optional<uint32_t> dataVersion;
    bool loaded = false;
    bool saveInFlight = false;

    Platform::Timer syncTimer;
};

} // namespace Core
} // namespace Styx
