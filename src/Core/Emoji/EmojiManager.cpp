#include "Core/Emoji/EmojiManager.hpp"

#include <algorithm>

#include <charconv>
#include <chrono>
#include <random>
#include <regex>
#include <unordered_map>
#include <unordered_set>

#include "Platform/Base64.hpp"
#include "Platform/Native.hpp"

#include "Core/ClientInstance.hpp"
#include "Core/Emoji/UnicodeEmojiIndex.hpp"
#include "Core/EmojiSegmenter.hpp"
#include "Core/Logging.hpp"
#include "Core/PermissionManager.hpp"
#include "Discord/CdnUrls.hpp"
#include "Discord/Client.hpp"
#include "Discord/Enums.hpp"
#include "Proto/FrecencySettings.hpp"
#include "Proto/ProtoReader.hpp"
#include "Proto/UserSettings.hpp"

namespace Styx {
namespace Core {

namespace {

constexpr int PERSISTED_ENTRY_LIMIT = 100;
constexpr int FIRST_SAVE_MS = 10 * 1000;
constexpr int FIRST_SAVE_JITTER_MS = 10 * 1000;
constexpr int PERIODIC_SAVE_MS = 2 * 60 * 60 * 1000;
constexpr int PERIODIC_SAVE_JITTER_MS = 10 * 60 * 1000;
constexpr int RATE_LIMIT_BACKOFF_MS = 30 * 1000;
constexpr int FETCH_RETRY_MS = 60 * 1000;
constexpr int MAX_PENDING_USAGES = 1000;

// straight from discord
const char *const DEFAULT_CHAT_EMOJIS[] = { "thumbsup", "eyes", "laughing", "watermelon",
                                            "fork_and_knife", "yum", "weary", "tired_face",
                                            "poop", "100" };
const char *const DEFAULT_REACTION_EMOJIS[] = { "100", "100", "thumbsup", "thumbsup",
                                                "thumbsdown", "thumbsdown", "heart", "point_up",
                                                "eyes", "weary", "laughing", "white_check_mark",
                                                "x" };
const char *const QUICK_REACTION_FALLBACKS[] = { "100", "laughing", "sparkling_heart" };

std::string normalize(const std::string &text)
{
    std::string result = Platform::unicodeLower(text);
    std::erase_if(result, [](char c) { return c == ' ' || c == '_'; });
    return result;
}

std::string regexEscape(const std::string &text)
{
    std::string escaped;
    for (char c : text) {
        if (std::string_view("\\.^$|()[]{}*+?").find(c) != std::string_view::npos)
            escaped += '\\';
        escaped += c;
    }
    return escaped;
}

int jitter(int bound)
{
    static thread_local std::mt19937 generator(std::random_device{}());
    return std::uniform_int_distribution<int>(0, bound - 1)(generator);
}

struct QueryMatcher
{
    std::string normalized;
    std::string lowered;
    std::regex prefix;
    std::regex wordBoundary;

    explicit QueryMatcher(const std::string &query)
        : normalized(normalize(query)), lowered(Platform::unicodeLower(query)),
          prefix("^" + regexEscape(lowered), std::regex::icase),
          wordBoundary("(^|_|[A-Z])" + regexEscape(lowered) + "s?([A-Z]|_|$)") {}

    [[nodiscard]] bool matches(const std::string &name) const
    {
        return name.find(normalized) != std::string::npos;
    }

    [[nodiscard]] double baseWeight(const std::string &name) const
    {
        const std::string lowerName = Platform::unicodeLower(name);
        double weight = 1;
        if (lowerName == lowered) weight += 4;
        if (std::regex_search(lowerName, wordBoundary) ||
            (lowerName != name && std::regex_search(name, wordBoundary))) weight += 2;
        if (std::regex_search(name, prefix)) weight += 1;
        return weight;
    }
};

std::unordered_map<std::string, FrecencyTracker::Entry> toHistory(const Proto::EmojiFrecency &frecency)
{
    std::unordered_map<std::string, FrecencyTracker::Entry> history;
    for (const auto &[key, val] : frecency.emojis) {
        FrecencyTracker::Entry entry;
        entry.totalUses = val.totalUses;
        entry.recentUses = val.recentUses;
        entry.score = val.score;
        history.insert_or_assign(key, std::move(entry));
    }
    return history;
}

Proto::EmojiFrecency toFrecency(const std::unordered_map<std::string, FrecencyTracker::Entry> &history)
{
    Proto::EmojiFrecency frecency;
    for (const auto &[key, entry] : history) {
        Proto::FrecencyItem item;
        item.totalUses = entry.totalUses;
        item.recentUses = entry.recentUses;
        item.frecency = entry.frecency;
        item.score = static_cast<int32_t>(entry.score);
        frecency.emojis.insert_or_assign(key, std::move(item));
    }
    return frecency;
}

} // namespace

EmojiManager::EmojiManager(Discord::Client *client, ClientInstance *instance)
    : client(client),
      instance(instance),
      chatTracker(FrecencyTracker::chatEmojiOptions()),
      reactionTracker(FrecencyTracker::reactionEmojiOptions())
{

}

uint64_t EmojiManager::nowMs()
{
    return static_cast<uint64_t>(Platform::DateTime::now().unixMillis());
}

std::string EmojiManager::unicodeKey(const std::string &surrogates)
{
    const UnicodeEmoji *base = UnicodeEmojiIndex::instance().convertSurrogateToBase(surrogates);
    return base ? UnicodeEmojiIndex::primaryName(*base) : std::string();
}

void EmojiManager::setGuildEmojis(Snowflake guildId, const std::vector<Discord::Emoji> &emojis)
{
    if (!guildId.isValid())
        return;

    guildEmojis.insert_or_assign(guildId, emojis);
    customCandidatesDirty = true;
}

void EmojiManager::removeGuild(Snowflake guildId)
{
    if (guildEmojis.erase(guildId) > 0)
        customCandidatesDirty = true;
}

void EmojiManager::onGuildEmojisUpdated(const Discord::GuildEmojisUpdate &event)
{
    if (!event.guildId.hasValue())
        return;
    setGuildEmojis(event.guildId.get(), event.emojis.get());
}

const std::vector<EmojiManager::CustomEmoji> &EmojiManager::customEmojis()
{
    if (!customCandidatesDirty)
        return customCandidates;

    customCandidates.clear();
    customCandidateIndex.clear();
    customCandidatesDirty = false;

    // disambiguation follows guild order
    std::vector<Snowflake> guildIds;
    guildIds.reserve(guildEmojis.size());
    for (const auto &[id, emojis] : guildEmojis) guildIds.push_back(id);
    std::sort(guildIds.begin(), guildIds.end());

    const UnicodeEmojiIndex &index = UnicodeEmojiIndex::instance();
    std::unordered_map<std::string, int> nameCounts;

    for (Snowflake guildId : guildIds) {
        const auto it = guildEmojis.find(guildId);
        if (it == guildEmojis.end())
            continue;

        std::vector<Discord::Emoji> emojis = it->second;
        std::sort(emojis.begin(), emojis.end(),
                  [](const Discord::Emoji &a, const Discord::Emoji &b) {
                      return a.name.get() < b.name.get();
                  });

        for (const Discord::Emoji &emoji : emojis) {
            if (emoji.isUnicode() || !emoji.id.get().isValid() || !emoji.name.hasValue())
                continue;

            CustomEmoji custom;
            custom.emoji = emoji;
            custom.guildId = guildId;
            custom.originalName = emoji.name.get();

            auto countIt = nameCounts.find(custom.originalName);
            if (countIt == nameCounts.end()) {
                const UnicodeEmoji *shadowed = index.byName(custom.originalName);
                const bool takenByUnicode =
                        shadowed && UnicodeEmojiIndex::primaryName(*shadowed) == custom.originalName;
                countIt = nameCounts.emplace(custom.originalName, takenByUnicode ? 1 : 0).first;
            }

            const int taken = countIt->second++;
            custom.disambiguatedName = taken > 0
                                               ? custom.originalName + "~" + std::to_string(taken)
                                               : custom.originalName;
            custom.normalizedName = normalize(custom.disambiguatedName);

            customCandidateIndex.insert_or_assign(emoji.id.get(), static_cast<int>(customCandidates.size()));
            customCandidates.push_back(custom);
        }
    }

    return customCandidates;
}

const std::vector<EmojiManager::UnicodeCandidate> &EmojiManager::unicodeCandidates()
{
    if (!unicodeCandidateCache.empty())
        return unicodeCandidateCache;

    for (const UnicodeEmoji *emoji : UnicodeEmojiIndex::instance().topLevel()) {
        UnicodeCandidate candidate;
        candidate.emoji = emoji;
        candidate.normalizedNames.reserve(emoji->names.size() + emoji->keywords.size());
        for (const std::string &name : emoji->names)
            candidate.normalizedNames.push_back(normalize(name));
        for (const std::string &keyword : emoji->keywords)
            candidate.normalizedNames.push_back(normalize(keyword));
        unicodeCandidateCache.push_back(candidate);
    }
    return unicodeCandidateCache;
}

bool EmojiManager::isUsable(const CustomEmoji &custom, UsabilityContext &context) const
{
    const Discord::Emoji &emoji = custom.emoji;
    if (emoji.available.hasValue() && !emoji.available.get())
        return false;

    if (emoji.roles.hasValue() && !emoji.roles.get().empty()) {
        auto rolesIt = context.rolesByGuild.find(custom.guildId);
        if (rolesIt == context.rolesByGuild.end()) {
            std::unordered_set<Snowflake> ids;
            for (const Discord::Role &role :
                 instance->getMemberRolesSorted(custom.guildId, context.selfId))
                ids.insert(role.id.get());
            rolesIt = context.rolesByGuild.emplace(custom.guildId, std::move(ids)).first;
        }

        bool hasRole = false;
        for (Snowflake roleId : emoji.roles.get()) {
            if (rolesIt->second.contains(roleId)) {
                hasRole = true;
                break;
            }
        }
        if (!hasRole)
            return false;
    }

    const bool inGuild = context.channelGuildId.isValid();
    const bool external = !inGuild || context.channelGuildId != custom.guildId;
    if (external) {
        if (inGuild) {
            if (!context.externalEmojisAllowed)
                context.externalEmojisAllowed = instance->permissions()->hasChannelPermission(context.selfId, context.channelId, Discord::Permission::USE_EXTERNAL_EMOJIS);
            if (!*context.externalEmojisAllowed)
                return false;
        }

        const bool managed = emoji.managed.hasValue() && emoji.managed.get();
        if (!managed && !context.premium)
            return false;
    }

    const bool animated = emoji.animated.hasValue() && emoji.animated.get();
    return !animated || context.premium;
}

EmojiManager::UsabilityContext EmojiManager::usabilityIn(Snowflake channelId) const
{
    UsabilityContext usability;
    usability.channelId = channelId;
    usability.selfId = client->getMe().id.get();
    usability.premium = client->isPremium();
    usability.channelGuildId = client->getGuildIdForChannel(channelId);
    return usability;
}

std::vector<EmojiManager::RankedEmoji> EmojiManager::rankedMatches(const std::string &query, Snowflake channelId, FrecencyTracker &tracker)
{
    const QueryMatcher matcher(query);
    if (matcher.normalized.empty())
        return {};

    tracker.refresh(nowMs());
    UsabilityContext usability = usabilityIn(channelId);

    struct Scored
    {
        double weight = 0;
        std::string name;
        RankedEmoji emoji;
    };
    std::vector<Scored> scored;

    // scale by score
    auto weigh = [&tracker, &matcher](const std::string &name, const std::string &frecencyKey) {
        double weight = matcher.baseWeight(name);
        if (const std::optional<double> score = tracker.score(frecencyKey))
            weight *= *score / 100.0;
        return weight;
    };

    for (const UnicodeCandidate &candidate : unicodeCandidates()) {
        bool matched = false;
        for (const std::string &name : candidate.normalizedNames) {
            if (matcher.matches(name)) {
                matched = true;
                break;
            }
        }
        if (!matched)
            continue;

        const std::string primaryName = UnicodeEmojiIndex::primaryName(*candidate.emoji);
        scored.push_back({ weigh(primaryName, primaryName), primaryName, { candidate.emoji, nullptr } });
    }

    for (const CustomEmoji &custom : customEmojis()) {
        if (!matcher.matches(custom.normalizedName))
            continue;
        if (!isUsable(custom, usability))
            continue;

        scored.push_back({ weigh(custom.disambiguatedName, custom.emoji.id.get().toString()), custom.disambiguatedName, { nullptr, &custom } });
    }

    std::sort(scored.begin(), scored.end(), [](const Scored &a, const Scored &b) {
        if (a.weight != b.weight)
            return a.weight > b.weight;
        return a.name < b.name;
    });

    std::vector<RankedEmoji> ranked;
    ranked.reserve(scored.size());
    for (const Scored &entry : scored)
        ranked.push_back(entry.emoji);
    return ranked;
}

std::vector<EmojiMatch> EmojiManager::search(const std::string &query, Snowflake channelId, int maxResults)
{
    const std::vector<RankedEmoji> ranked = rankedMatches(query, channelId, chatTracker);

    std::vector<EmojiMatch> results;
    const int count = std::min<int>(ranked.size(), std::max(maxResults, 0));
    results.reserve(count);
    for (int i = 0; i < count; i++) {
        const RankedEmoji &emoji = ranked.at(i);
        EmojiMatch match;
        if (emoji.unicode) {
            match.insertText = toPickerEmoji(*emoji.unicode).messageText();
            match.displayLabel = match.insertText;
            match.surrogates = emoji.unicode->surrogates;
        } else {
            const CustomEmoji &custom = *emoji.custom;
            const Snowflake id = custom.emoji.id.get();
            match.insertText = toPickerEmoji(custom).messageText();
            match.displayLabel = ":" + custom.disambiguatedName + ":";
            match.customId = id;
            match.imageUrl = custom.emoji.getImageUrl();
        }
        results.push_back(match);
    }
    return results;
}

PickerEmoji EmojiManager::toPickerEmoji(const UnicodeEmoji &emoji)
{
    PickerEmoji picker;
    picker.name = UnicodeEmojiIndex::primaryName(emoji);
    picker.surrogates = emoji.surrogates;
    return picker;
}

PickerEmoji EmojiManager::toPickerEmoji(const CustomEmoji &custom)
{
    PickerEmoji picker;
    picker.name = custom.disambiguatedName;
    picker.customId = custom.emoji.id.get();
    picker.customName = custom.originalName;
    picker.guildId = custom.guildId;
    picker.animated = custom.emoji.isAnimated();
    return picker;
}

FrecencyTracker &EmojiManager::trackerFor(EmojiIntention intention)
{
    return intention == EmojiIntention::Reaction ? reactionTracker : chatTracker;
}

std::vector<PickerEmoji> EmojiManager::searchPicker(const std::string &query, Snowflake channelId, EmojiIntention intention)
{
    const std::vector<RankedEmoji> ranked = rankedMatches(query, channelId, trackerFor(intention));

    std::vector<PickerEmoji> results;
    results.reserve(ranked.size());
    for (const RankedEmoji &emoji : ranked)
        results.push_back(emoji.unicode ? toPickerEmoji(*emoji.unicode) : toPickerEmoji(*emoji.custom));
    return results;
}

std::vector<PickerEmoji> EmojiManager::frequentlyUsed(Snowflake channelId, EmojiIntention intention)
{
    const std::vector<CustomEmoji> &customs = customEmojis();
    const UnicodeEmojiIndex &index = UnicodeEmojiIndex::instance();
    UsabilityContext usability = usabilityIn(channelId);

    std::vector<PickerEmoji> frequent;
    for (const std::string &key : trackerFor(intention).frequently(nowMs())) {
        if (const UnicodeEmoji *emoji = index.byName(key)) {
            frequent.push_back(toPickerEmoji(*emoji));
            continue;
        }

        const auto it = customCandidateIndex.find(Snowflake([&key] { uint64_t value = 0; auto [end, ec] = std::from_chars(key.data(), key.data() + key.size(), value); return ec == std::errc{} && end == key.data() + key.size() ? value : uint64_t{0}; }()));
        if (it != customCandidateIndex.end() && isUsable(customs.at(it->second), usability))
            frequent.push_back(toPickerEmoji(customs.at(it->second)));
    }
    return frequent;
}

std::vector<PickerEmoji> EmojiManager::quickReactions(Snowflake channelId, int count)
{
    std::vector<PickerEmoji> quick = frequentlyUsed(channelId, EmojiIntention::Reaction);

    const UnicodeEmojiIndex &index = UnicodeEmojiIndex::instance();
    for (const char *name : QUICK_REACTION_FALLBACKS) {
        if (quick.size() >= count)
            break;
        const UnicodeEmoji *emoji = index.byName(name);
        if (!emoji)
            continue;

        const PickerEmoji fallback = toPickerEmoji(*emoji);
        const bool alreadyListed = std::any_of(quick.cbegin(), quick.cend(), [&fallback](const PickerEmoji &listed) { return listed.sameEmoji(fallback); });
        if (!alreadyListed)
            quick.push_back(fallback);
    }

    if (count >= 0 && quick.size() > static_cast<size_t>(count))
        quick.resize(static_cast<size_t>(count));
    return quick;
}

std::vector<Snowflake> EmojiManager::guildsInSidebarOrder() const
{
    std::vector<Snowflake> placedBySettings;
    const std::optional<Proto::GuildFolders> &folders = client->getSettings().guildFolders;
    if (folders && !folders->folders.empty()) {
        for (const Proto::GuildFolder &folder : folders->folders) {
            for (Core::Snowflake gid : folder.guildIds)
                placedBySettings.push_back(gid);
        }
    } else if (folders) {
        for (Core::Snowflake gid : folders->guildPositions)
            placedBySettings.push_back(gid);
    }

    const std::unordered_set<Snowflake> placed(placedBySettings.cbegin(), placedBySettings.cend());
    std::vector<Snowflake> unplacedNewestFirst;
    for (const auto &[id, emojis] : guildEmojis) {
        if (!placed.contains(id))
            unplacedNewestFirst.push_back(id);
    }
    std::sort(unplacedNewestFirst.begin(), unplacedNewestFirst.end(), [](Snowflake a, Snowflake b) { return b < a; });

    unplacedNewestFirst.insert(unplacedNewestFirst.end(), placedBySettings.begin(), placedBySettings.end());
    return unplacedNewestFirst;
}

std::vector<PickerSection> EmojiManager::pickerSections(Snowflake channelId, EmojiIntention intention)
{
    std::vector<PickerSection> sections;

    std::vector<PickerEmoji> frequent = frequentlyUsed(channelId, intention);
    if (!frequent.empty())
        sections.push_back({ PickerSection::RecentId, "Frequently Used", Snowflake(), std::string(), std::move(frequent) });

    UsabilityContext usability = usabilityIn(channelId);
    std::unordered_map<Snowflake, std::vector<PickerEmoji>> usableByGuild;
    for (const CustomEmoji &custom : customEmojis()) {
        if (isUsable(custom, usability))
            usableByGuild[custom.guildId].push_back(toPickerEmoji(custom));
    }

    std::vector<Snowflake> guildOrder = guildsInSidebarOrder();
    if (usability.channelGuildId.isValid())
        guildOrder.insert(guildOrder.begin(), usability.channelGuildId);

    for (Snowflake guildId : guildOrder) {
        const auto it = usableByGuild.find(guildId);
        if (it == usableByGuild.end())
            continue;

        PickerSection section;
        section.id = guildId.toString();
        section.guildId = guildId;
        if (const std::optional<Discord::Guild> guild = instance->getGuild(guildId)) {
            section.title = guild->name.valueOr(std::string());
            if (guild->icon.hasValue() && !guild->icon.get().empty())
                section.guildIconUrl = Discord::Cdn::guildIcon(guildId, guild->icon.get(), 64);
        }
        section.emojis = std::move(it->second);
        sections.push_back(section);
        usableByGuild.erase(it);
    }

    for (const UnicodeEmojiCategory &category : UnicodeEmojiIndex::instance().categories()) {
        PickerSection section;
        section.id = category.id;
        section.title = unicodeCategoryTitle(category.id);
        section.emojis.reserve(category.emojis.size());
        for (const UnicodeEmoji *emoji : category.emojis)
            section.emojis.push_back(toPickerEmoji(*emoji));
        sections.push_back(section);
    }

    return sections;
}

std::string EmojiManager::unicodeCategoryTitle(const std::string &categoryId)
{
    if (categoryId == std::string_view("people"))
        return "People";
    if (categoryId == std::string_view("nature"))
        return "Nature";
    if (categoryId == std::string_view("food"))
        return "Food";
    if (categoryId == std::string_view("activity"))
        return "Activities";
    if (categoryId == std::string_view("travel"))
        return "Travel";
    if (categoryId == std::string_view("objects"))
        return "Objects";
    if (categoryId == std::string_view("symbols"))
        return "Symbols";
    if (categoryId == std::string_view("flags"))
        return "Flags";
    return categoryId;
}

void EmojiManager::trackUsage(const std::string &key, bool reaction)
{
    if (key.empty())
        return;

    const uint64_t timestamp = nowMs();
    // a reaction counts towards both
    if (reaction)
        reactionTracker.track(key, timestamp);
    chatTracker.track(key, timestamp);

    if (pendingUsages.size() >= MAX_PENDING_USAGES)
        pendingUsages.erase(pendingUsages.begin());
    pendingUsages.push_back({ key, timestamp, reaction });
}

void EmojiManager::trackMessageEmojis(const std::string &content)
{
    if (content.empty())
        return;

    for (std::sregex_iterator it(content.begin(), content.end(), customEmojiTagRe()), end; it != end; ++it)
        trackUsage((*it)[2].str(), false);

    trackSurrogates(content);
}

void EmojiManager::trackSurrogates(const std::string &content)
{
    for (const std::string &sequence : extractEmojiSequences(content))
        trackUsage(unicodeKey(sequence), false);
}

// `reactionEmoji` is in REST form
void EmojiManager::trackReaction(const std::string &reactionEmoji)
{
    const auto idSeparator = reactionEmoji.rfind(':');
    if (idSeparator == std::string::npos)
        trackUsage(unicodeKey(reactionEmoji), true);
    else
        trackUsage(reactionEmoji.substr(idSeparator + 1), true);
}

void EmojiManager::onReady()
{
    fetchSettings();
}

void EmojiManager::fetchSettings()
{
    Platform::Guard<EmojiManager> self(this);
    client->fetchFrecencySettings([this, self](const Result<Proto::FrecencyUserSettings> &result) {
        if (!self)
            return;

        if (!result.success()) {
            Core::log(Core::LogLevel::Warning, LogCore) << "EmojiManager: frecency settings fetch failed:" << result.error;
            scheduleSync(FETCH_RETRY_MS);
            return;
        }

        adoptSettings(*result.value, false);

        replayPendingUsages();
        seedDefaultsIfEmpty();

        loaded = true;
        scheduleSync(FIRST_SAVE_MS + jitter(FIRST_SAVE_JITTER_MS));

        Core::log(Core::LogLevel::Info, LogCore) << "EmojiManager: loaded frecency for" << instance->accountId() << "-"
                        << chatTracker.usageHistory().size() << "chat,"
                        << reactionTracker.usageHistory().size() << "reaction entries";
    });
}

void EmojiManager::adoptSettings(const Proto::FrecencyUserSettings &settings, bool partial)
{
    if (settings.dataVersion)
        dataVersion = settings.dataVersion;

    auto apply = [partial](FrecencyTracker &tracker,
                           const std::optional<Proto::EmojiFrecency> &frecency) {
        if (!frecency) {
            if (!partial)
                tracker.reset();
            return;
        }

        std::unordered_map<std::string, FrecencyTracker::Entry> history = toHistory(*frecency);
        if (partial) {
            std::unordered_map<std::string, FrecencyTracker::Entry> merged = tracker.usageHistory();
            for (const auto &[key, entry] : history)
                merged.insert_or_assign(key, entry);
            history = merged;
        }
        tracker.overwriteHistory(history);
    };

    apply(chatTracker, settings.emojiFrecency);
    apply(reactionTracker, settings.emojiReactionFrecency);
}

void EmojiManager::replayPendingUsages()
{
    for (const PendingUsage &usage : pendingUsages) {
        if (usage.reaction)
            reactionTracker.track(usage.key, usage.timestamp);
        chatTracker.track(usage.key, usage.timestamp);
    }
}

void EmojiManager::seedDefaultsIfEmpty()
{
    if (chatTracker.usageHistory().empty()) {
        for (const char *name : DEFAULT_CHAT_EMOJIS)
            chatTracker.track(name);
    }

    if (reactionTracker.usageHistory().empty()) {
        for (const char *name : DEFAULT_REACTION_EMOJIS)
            reactionTracker.track(name);
    }
}

void EmojiManager::onUserSettingsProtoUpdated(const Discord::UserSettingsProtoUpdate &event)
{
    if (!event.type.hasValue() || event.type.get() != Discord::UserSettingsProtoType::FRECENCY || !event.proto.hasValue())
        return;

    const auto decoded = Platform::fromBase64(event.proto.get());
    if (!decoded || decoded->empty())
        return;

    Proto::ProtoReader reader(std::span<const uint8_t>(*decoded));
    const Proto::FrecencyUserSettings settings = Proto::FrecencyUserSettings::fromProto(reader);

    const bool partial = event.partial.hasValue() && event.partial.get();
    adoptSettings(settings, partial);

    if (!partial)
        replayPendingUsages();

    Core::log(Core::LogLevel::Info, LogCore) << "EmojiManager: frecency settings update, partial" << partial << "dataVersion"
                    << (dataVersion ? int(*dataVersion) : -1);
}

void EmojiManager::onFrecencySettingsReceived(const Proto::FrecencyUserSettings &settings)
{
    if (settings.dataVersion)
        dataVersion = settings.dataVersion;
}

void EmojiManager::scheduleSync(int milliseconds)
{
    syncTimer.start(std::chrono::milliseconds(milliseconds), false, [this] { onSyncTimer(); });
}

void EmojiManager::onSyncTimer()
{
    if (!loaded) {
        fetchSettings();
        return;
    }

    savePendingUsage();
    scheduleSync(PERIODIC_SAVE_MS + jitter(PERIODIC_SAVE_JITTER_MS));
}

void EmojiManager::savePendingUsage()
{
    if (!loaded || pendingUsages.empty() || saveInFlight)
        return;

    const uint64_t now = nowMs();
    chatTracker.compute(now);
    reactionTracker.compute(now);

    Proto::FrecencyUserSettings partial;
    partial.emojiFrecency = toFrecency(FrecencyTracker::capForPersistence(chatTracker.usageHistory(), PERSISTED_ENTRY_LIMIT));
    partial.emojiReactionFrecency = toFrecency(FrecencyTracker::capForPersistence(reactionTracker.usageHistory(), PERSISTED_ENTRY_LIMIT));

    std::vector<PendingUsage> inFlight;
    inFlight.swap(pendingUsages);
    saveInFlight = true;

    Platform::Guard<EmojiManager> self(this);
    const auto partialBytes = partial.toProtoPartial();
    client->patchFrecencySettings(
            std::string(reinterpret_cast<const char *>(partialBytes.data()), partialBytes.size()), dataVersion,
            [this, self, inFlight](const Discord::Client::FrecencyPatchResult &result) {
                if (!self)
                    return;

                saveInFlight = false;

                if (!result.success || result.outOfDate) {
                    pendingUsages.insert(pendingUsages.begin(), inFlight.begin(), inFlight.end());

                    if (result.outOfDate) {
                        Core::log(Core::LogLevel::Info, LogCore) << "EmojiManager: frecency save out of date, refetching";
                        fetchSettings();
                    } else if (result.rateLimited) {
                        Core::log(Core::LogLevel::Warning, LogCore) << "EmojiManager: frecency save rate limited, retrying";
                        scheduleSync(RATE_LIMIT_BACKOFF_MS);
                    } else {
                        Core::log(Core::LogLevel::Warning, LogCore) << "EmojiManager: frecency save failed:" << result.error;
                    }
                    return;
                }

                if (result.settings) {
                    adoptSettings(*result.settings, false);
                    replayPendingUsages();
                }

                Core::log(Core::LogLevel::Info, LogCore) << "EmojiManager: saved" << inFlight.size() << "emoji usages";
            });
}

} // namespace Core
} // namespace Styx
