#include "AuditLogFormatter.hpp"

#include "Core/UserManager.hpp"
#include "Platform/Native.hpp"
#include <charconv>
#include <chrono>
#include <ctime>
#include <format>
#include <locale>
#include <unordered_set>

#include <algorithm>
#include <cmath>
#include <functional>

#include "Core/ClientInstance.hpp"

namespace Styx {
namespace UI {

using Action = Discord::AuditLogAction;

enum class AuditLogFormatter::Target {
    Unknown,
    Guild,
    Channel,
    ChannelOverwrite,
    User,
    Role,
    Invite,
    Webhook,
    Emoji,
    Integration,
    StageInstance,
    Sticker,
    ScheduledEvent,
    ScheduledEventException,
    Thread,
    ApplicationCommand,
    AutoModerationRule,
    Soundboard,
    OnboardingPrompt,
    Onboarding,
    GuildHome,
    HomeSettings,
    VoiceChannelStatus,
    MemberVerification,
    GuildProfile,
};

struct AuditLogFormatter::Change
{
    std::string key;
    nlohmann::json oldValue;
    nlohmann::json newValue;
    std::string subtarget;
    std::vector<std::string> items;
};

struct AuditLogFormatter::Entry
{
    Discord::AuditLogEntry raw;
    Action action;
    Target target = Target::Unknown;
    Category category = Category::Other;
    Core::Snowflake id;
    Core::Snowflake userId;
    Core::Snowflake targetId;
    Platform::DateTime time;
    std::vector<Change> changes;
};

namespace {

constexpr int64_t MergeWindowSecs = 30 * 60;
constexpr int MaxMerges = 50;

std::string tr(const char *message) { return message; }

const nlohmann::json &jsonMember(const nlohmann::json &object, std::string_view name)
{
    static const nlohmann::json missing;
    auto found = object.find(name);
    return found != object.end() ? *found : missing;
}

std::string join(const std::vector<std::string> &parts, std::string_view separator)
{
    size_t length = parts.empty() ? 0 : separator.size() * (parts.size() - 1);
    for (const auto &part : parts) length += part.size();
    std::string result;
    result.reserve(length);
    for (const auto &part : parts) {
        if (!result.empty()) result += separator;
        result += part;
    }
    return result;
}

std::string bold(const std::string &text) { return "<b>" + Platform::htmlEscape(text) + "</b>"; }
bool isNull(const nlohmann::json &value) { return value.is_null(); }
bool truthy(const nlohmann::json &value)
{
    if (value.is_boolean()) return value.get<bool>();
    if (value.is_number()) return value.get<double>() != 0;
    if (value.is_string()) return !value.get_ref<const std::string &>().empty();
    return value.is_array() || value.is_object();
}
std::string text(const nlohmann::json &value)
{
    if (value.is_string()) return value.get<std::string>();
    if (value.is_null() || value.is_discarded()) return {};
    if (value.is_array()) {
        std::vector<std::string> parts;
        parts.reserve(value.size());
        for (const auto &item : value) parts.push_back(text(item));
        return join(parts, ", ");
    }
    return value.dump();
}
int64_t number(const nlohmann::json &value)
{
    if (value.is_string()) {
        const auto &string = value.get_ref<const std::string &>();
        int64_t result = 0;
        std::from_chars(string.data(), string.data() + string.size(), result);
        return result;
    }
    return value.is_number() ? value.get<int64_t>() : 0;
}
uint64_t bitsOf(const nlohmann::json &value)
{
    if (value.is_string()) {
        const auto &string = value.get_ref<const std::string &>();
        uint64_t result = 0;
        std::from_chars(string.data(), string.data() + string.size(), result);
        return result;
    }
    return value.is_number_unsigned() ? value.get<uint64_t>() : 0;
}
Core::Snowflake snowflakeOf(const nlohmann::json &value) { return Core::Snowflake(bitsOf(value)); }
std::string countText(int64_t count, const char *one, const char *other)
{
    if (count == 1) return one;
    auto formatted = std::format(std::locale(""), "{:L}", count);
    return std::vformat(other, std::make_format_args(formatted));
}
std::string colorText(const nlohmann::json &value) { return std::format("#{:06X}", uint32_t(number(value)) & 0xffffff); }
std::string swatch(const std::string &color) { return "<span style=\"color:" + Platform::htmlEscape(color) + "\">&#9632;</span>"; }
std::chrono::sys_days localDate(const Platform::DateTime &time)
{
    auto seconds = static_cast<std::time_t>(time.unixMillis() / 1000);
    std::tm date{};
#ifdef _WIN32
    localtime_s(&date, &seconds);
#else
    localtime_r(&seconds, &date);
#endif
    return std::chrono::sys_days(std::chrono::year(date.tm_year + 1900) / (date.tm_mon + 1) / date.tm_mday);
}
std::string calendar(const Platform::DateTime &time)
{
    const auto days = (localDate(Platform::DateTime::now()) - localDate(time)).count();
    auto clock = time.format("%I:%M %p");
    auto weekday = time.format("%A");
    if (days == 0) return "Today at " + clock;
    if (days == 1) return "Yesterday at " + clock;
    if (days > 1 && days < 7) return "Last " + weekday + " at " + clock;
    if (days == -1) return "Tomorrow at " + clock;
    if (days < -1 && days > -7) return weekday + " at " + clock;
    return time.format("%m/%d/%Y");
}
std::string longDateTime(const nlohmann::json &value)
{
    auto time = Platform::DateTime::parse(text(value));
    return time.valid() ? time.format("%A, %B %d, %Y %I:%M %p") : text(value);
}

struct PermissionName
{
    int bit;
    const char *name;
};

constexpr PermissionName PermissionNames[] = {
    { 0, "Create Invite" },
    { 1, "Kick Members" },
    { 2, "Ban Members" },
    { 3, "Administrator" },
    { 4, "Manage Channels" },
    { 5, "Manage Server" },
    { 26, "Change Nickname" },
    { 27, "Manage Nicknames" },
    { 28, "Manage Roles" },
    { 29, "Manage Webhooks" },
    { 30, "Manage Expressions" },
    { 43, "Create Expressions" },
    { 7, "View Audit Log" },
    { 10, "View Channels" },
    { 19, "View Server Insights" },
    { 41, "View Server Subscription Insights" },
    { 40, "Timeout Members" },
    { 39, "Use Activities" },
    { 50, "Use External Apps" },
    { 11, "Send Messages" },
    { 12, "Send TTS Messages" },
    { 13, "Manage Messages" },
    { 14, "Embed Links" },
    { 15, "Attach Files" },
    { 16, "Read Message History" },
    { 17, "Mention @everyone, @here, and All Roles" },
    { 18, "Use External Emoji" },
    { 6, "Add Reactions" },
    { 31, "Use Application Commands" },
    { 34, "Manage Threads" },
    { 35, "Create Public Threads" },
    { 36, "Create Private Threads" },
    { 37, "Use External Stickers" },
    { 38, "Send Messages in Threads" },
    { 46, "Send Voice Messages" },
    { 49, "Create Polls" },
    { 51, "Pin Messages" },
    { 52, "Bypass Slowmode" },
    { 53, "Manage Official Messages" },
    { 20, "Connect" },
    { 21, "Speak" },
    { 22, "Mute Members" },
    { 23, "Deafen Members" },
    { 24, "Move Members" },
    { 25, "Use Voice Activity" },
    { 8, "Priority Speaker" },
    { 9, "Video" },
    { 42, "Use Soundboard" },
    { 45, "Use External Sounds" },
    { 48, "Set Voice Channel Status" },
    { 32, "Request to Speak" },
    { 33, "Manage Events" },
    { 44, "Create Events" },
};

std::vector<std::string> permissionNames(uint64_t bits, bool onChannel)
{
    std::vector<std::string> names;
    for (const PermissionName &permission : PermissionNames) {
        if (!(bits & (uint64_t(1) << permission.bit)))
            continue;
        if (onChannel && permission.bit == 4)
            names.push_back(std::string("Manage Channel"));
        else if (onChannel && permission.bit == 10)
            names.push_back(std::string("View Channel"));
        else
            names.push_back(tr(permission.name));
    }
    return names;
}

std::string channelTypeName(int64_t type)
{
    switch (type) {
    case 0:
        return std::string("Text Channel");
    case 1:
        return std::string("Direct Message");
    case 2:
        return std::string("Voice Channel");
    case 3:
        return std::string("Group DM");
    case 4:
        return std::string("Category");
    case 5:
        return std::string("Announcement Channel");
    case 6:
        return std::string("Store Channel");
    case 13:
        return std::string("Stage Channel");
    case 15:
        return std::string("Forum Channel");
    case 16:
        return std::string("Media Channel");
    }
    return std::to_string(type);
}

bool isTextLikeChannelType(int64_t type)
{
    switch (type) {
    case 0:
    case 1:
    case 3:
    case 5:
    case 6:
    case 10:
    case 11:
    case 12:
    case 14:
    case 15:
    case 16:
        return true;
    }
    return false;
}

std::string inviteMaxAgeText(int64_t seconds)
{
    switch (seconds) {
    case 0:
        return std::string("Never");
    case 1800:
        return std::string("30 minutes");
    case 3600:
        return std::string("1 hour");
    case 21600:
        return std::string("6 hours");
    case 43200:
        return std::string("12 hours");
    case 86400:
        return std::string("1 day");
    case 604800:
        return std::string("7 days");
    case 1209600:
        return std::string("14 days");
    case 2592000:
        return std::string("30 days");
    case 5184000:
        return std::string("60 days");
    }
    return std::to_string(seconds);
}

std::string localeName(const std::string &code)
{
    static const std::unordered_map<std::string, std::string> names = {
        { "en-US", "English, US" },
        { "en-GB", "English, UK" },
        { "zh-CN", "中文" },
        { "zh-TW", "繁體中文" },
        { "cs", "Čeština" },
        { "da", "Dansk" },
        { "nl", "Nederlands" },
        { "fr", "Français" },
        { "de", "Deutsch" },
        { "el", "Ελληνικά" },
        { "hu", "Magyar" },
        { "it", "Italiano" },
        { "ja", "日本語" },
        { "ko", "한국어" },
        { "pl", "Polski" },
        { "pt-BR", "Português do Brasil" },
        { "ru", "Русский" },
        { "es-419", "Español, LATAM" },
        { "es-ES", "Español" },
        { "sv-SE", "Svenska" },
        { "tr", "Türkçe" },
        { "bg", "български" },
        { "uk", "Українська" },
        { "fi", "Suomi" },
        { "no", "Norsk" },
        { "hr", "Hrvatski" },
        { "ro", "Română" },
        { "lt", "Lietuviškai" },
        { "th", "ไทย" },
        { "vi", "Tiếng Việt" },
        { "hi", "हिंदी" },
    };
    const auto found = names.find(code);
    return found != names.end() ? found->second : code;
}

std::string autoModTriggerName(int64_t type)
{
    switch (type) {
    case 1:
        return std::string("Block Custom Words");
    case 3:
        return std::string("Block Suspected Spam Content");
    case 4:
        return std::string("Block Commonly Flagged Words");
    case 5:
        return std::string("Block Mention Spam");
    case 6:
        return std::string("Block Words in Member Profile Names");
    }
    return std::string("Unknown");
}

std::string autoModEventName(int64_t type)
{
    switch (type) {
    case 1:
        return std::string("Message Send");
    case 2:
        return std::string("Member join or update");
    }
    return std::string("Unknown");
}

std::string autoModActionName(int64_t type)
{
    switch (type) {
    case 1:
        return std::string("Block message");
    case 2:
        return std::string("Send alert");
    case 3:
        return std::string("Timeout user");
    case 4:
        return std::string("Block member interactions");
    }
    return std::string("Unknown");
}

std::string quotedList(const nlohmann::json &value)
{
    std::vector<std::string> quoted;
    for (const nlohmann::json &item : value)
        quoted.push_back(std::format("'{0}'", text(item)));
    return join(quoted, std::string(", "));
}

std::string integrationPlatform(const std::string &type)
{
    if (type == std::string_view("twitch"))
        return std::string("Twitch");
    if (type == std::string_view("youtube"))
        return std::string("YouTube");
    if (type == std::string_view("discord"))
        return std::string("Discord");
    return std::string("Unknown Integration");
}

std::string tagText(const nlohmann::json &tag)
{
    const std::string emoji = text(jsonMember(tag, "emoji_name"));
    const std::string name = text(jsonMember(tag, "name"));
    return emoji.empty() ? name : emoji + char(' ') + name;
}

} // namespace

AuditLogFormatter::AuditLogFormatter(Core::ClientInstance *instance, Core::Snowflake guildId)
    : instance(instance), guildId(guildId)
{
}

AuditLogFormatter::Target AuditLogFormatter::targetOf(Action action)
{
    switch (action) {
    case Action::GUILD_UPDATE:
    case Action::CREATOR_MONETIZATION_REQUEST_CREATED:
    case Action::CREATOR_MONETIZATION_TERMS_ACCEPTED:
    case Action::HARMFUL_LINKS_BLOCKED_MESSAGE:
    case Action::GUILD_MIGRATE_PIN_PERMISSION:
    case Action::GUILD_MIGRATE_BYPASS_SLOWMODE_PERMISSION:
        return Target::Guild;
    case Action::CHANNEL_CREATE:
    case Action::CHANNEL_UPDATE:
    case Action::CHANNEL_DELETE:
    case Action::MESSAGE_BULK_DELETE:
        return Target::Channel;
    case Action::CHANNEL_OVERWRITE_CREATE:
    case Action::CHANNEL_OVERWRITE_UPDATE:
    case Action::CHANNEL_OVERWRITE_DELETE:
        return Target::ChannelOverwrite;
    case Action::MEMBER_KICK:
    case Action::MEMBER_PRUNE:
    case Action::MEMBER_BAN_ADD:
    case Action::MEMBER_BAN_REMOVE:
    case Action::MEMBER_UPDATE:
    case Action::MEMBER_ROLE_UPDATE:
    case Action::MEMBER_MOVE:
    case Action::MEMBER_DISCONNECT:
    case Action::BOT_ADD:
    case Action::MESSAGE_DELETE:
    case Action::MESSAGE_PIN:
    case Action::MESSAGE_UNPIN:
    case Action::AUTO_MODERATION_BLOCK_MESSAGE:
    case Action::AUTO_MODERATION_FLAG_TO_CHANNEL:
    case Action::AUTO_MODERATION_USER_COMMUNICATION_DISABLED:
    case Action::AUTO_MODERATION_QUARANTINE_USER:
        return Target::User;
    case Action::ROLE_CREATE:
    case Action::ROLE_UPDATE:
    case Action::ROLE_DELETE:
        return Target::Role;
    case Action::INVITE_CREATE:
    case Action::INVITE_UPDATE:
    case Action::INVITE_DELETE:
        return Target::Invite;
    case Action::WEBHOOK_CREATE:
    case Action::WEBHOOK_UPDATE:
    case Action::WEBHOOK_DELETE:
        return Target::Webhook;
    case Action::EMOJI_CREATE:
    case Action::EMOJI_UPDATE:
    case Action::EMOJI_DELETE:
        return Target::Emoji;
    case Action::INTEGRATION_CREATE:
    case Action::INTEGRATION_UPDATE:
    case Action::INTEGRATION_DELETE:
        return Target::Integration;
    case Action::STAGE_INSTANCE_CREATE:
    case Action::STAGE_INSTANCE_UPDATE:
    case Action::STAGE_INSTANCE_DELETE:
        return Target::StageInstance;
    case Action::STICKER_CREATE:
    case Action::STICKER_UPDATE:
    case Action::STICKER_DELETE:
        return Target::Sticker;
    case Action::GUILD_SCHEDULED_EVENT_CREATE:
    case Action::GUILD_SCHEDULED_EVENT_UPDATE:
    case Action::GUILD_SCHEDULED_EVENT_DELETE:
        return Target::ScheduledEvent;
    case Action::GUILD_SCHEDULED_EVENT_EXCEPTION_CREATE:
    case Action::GUILD_SCHEDULED_EVENT_EXCEPTION_UPDATE:
    case Action::GUILD_SCHEDULED_EVENT_EXCEPTION_DELETE:
        return Target::ScheduledEventException;
    case Action::THREAD_CREATE:
    case Action::THREAD_UPDATE:
    case Action::THREAD_DELETE:
        return Target::Thread;
    case Action::APPLICATION_COMMAND_PERMISSION_UPDATE:
        return Target::ApplicationCommand;
    case Action::SOUNDBOARD_SOUND_CREATE:
    case Action::SOUNDBOARD_SOUND_UPDATE:
    case Action::SOUNDBOARD_SOUND_DELETE:
        return Target::Soundboard;
    case Action::AUTO_MODERATION_RULE_CREATE:
    case Action::AUTO_MODERATION_RULE_UPDATE:
    case Action::AUTO_MODERATION_RULE_DELETE:
        return Target::AutoModerationRule;
    case Action::ONBOARDING_PROMPT_CREATE:
    case Action::ONBOARDING_PROMPT_UPDATE:
    case Action::ONBOARDING_PROMPT_DELETE:
        return Target::OnboardingPrompt;
    case Action::ONBOARDING_CREATE:
    case Action::ONBOARDING_UPDATE:
        return Target::Onboarding;
    case Action::GUILD_HOME_FEATURE_ITEM:
    case Action::GUILD_HOME_REMOVE_ITEM:
        return Target::GuildHome;
    case Action::HOME_SETTINGS_CREATE:
    case Action::HOME_SETTINGS_UPDATE:
        return Target::HomeSettings;
    case Action::VOICE_CHANNEL_STATUS_CREATE:
    case Action::VOICE_CHANNEL_STATUS_DELETE:
        return Target::VoiceChannelStatus;
    case Action::GUILD_MEMBER_VERIFICATION_UPDATE:
        return Target::MemberVerification;
    case Action::GUILD_PROFILE_UPDATE:
        return Target::GuildProfile;
    }
    return Target::Unknown;
}

AuditLogFormatter::Category AuditLogFormatter::categoryOf(Action action)
{
    switch (action) {
    case Action::CHANNEL_CREATE:
    case Action::CHANNEL_OVERWRITE_CREATE:
    case Action::MEMBER_BAN_REMOVE:
    case Action::BOT_ADD:
    case Action::ROLE_CREATE:
    case Action::INVITE_CREATE:
    case Action::WEBHOOK_CREATE:
    case Action::EMOJI_CREATE:
    case Action::MESSAGE_PIN:
    case Action::INTEGRATION_CREATE:
    case Action::STAGE_INSTANCE_CREATE:
    case Action::STICKER_CREATE:
    case Action::GUILD_SCHEDULED_EVENT_CREATE:
    case Action::THREAD_CREATE:
    case Action::SOUNDBOARD_SOUND_CREATE:
    case Action::AUTO_MODERATION_RULE_CREATE:
    case Action::CREATOR_MONETIZATION_REQUEST_CREATED:
    case Action::ONBOARDING_PROMPT_CREATE:
    case Action::ONBOARDING_CREATE:
    case Action::GUILD_HOME_FEATURE_ITEM:
    case Action::HOME_SETTINGS_CREATE:
    case Action::VOICE_CHANNEL_STATUS_CREATE:
        return Category::Create;
    case Action::CHANNEL_DELETE:
    case Action::CHANNEL_OVERWRITE_DELETE:
    case Action::MEMBER_KICK:
    case Action::MEMBER_PRUNE:
    case Action::MEMBER_BAN_ADD:
    case Action::MEMBER_DISCONNECT:
    case Action::ROLE_DELETE:
    case Action::INVITE_DELETE:
    case Action::WEBHOOK_DELETE:
    case Action::EMOJI_DELETE:
    case Action::MESSAGE_DELETE:
    case Action::MESSAGE_BULK_DELETE:
    case Action::MESSAGE_UNPIN:
    case Action::INTEGRATION_DELETE:
    case Action::STAGE_INSTANCE_DELETE:
    case Action::STICKER_DELETE:
    case Action::GUILD_SCHEDULED_EVENT_DELETE:
    case Action::THREAD_DELETE:
    case Action::SOUNDBOARD_SOUND_DELETE:
    case Action::AUTO_MODERATION_RULE_DELETE:
    case Action::AUTO_MODERATION_BLOCK_MESSAGE:
    case Action::ONBOARDING_PROMPT_DELETE:
    case Action::GUILD_HOME_REMOVE_ITEM:
    case Action::VOICE_CHANNEL_STATUS_DELETE:
    case Action::GUILD_SCHEDULED_EVENT_EXCEPTION_DELETE:
        return Category::Delete;
    case Action::HARMFUL_LINKS_BLOCKED_MESSAGE:
        return Category::Other;
    default:
        return Category::Update;
    }
}

bool AuditLogFormatter::neverMerged(Action action)
{
    switch (action) {
    case Action::MESSAGE_DELETE:
    case Action::MESSAGE_BULK_DELETE:
    case Action::MESSAGE_PIN:
    case Action::MESSAGE_UNPIN:
    case Action::MEMBER_MOVE:
    case Action::MEMBER_DISCONNECT:
    case Action::BOT_ADD:
    case Action::APPLICATION_COMMAND_PERMISSION_UPDATE:
    case Action::MEMBER_PRUNE:
        return true;
    default:
        return false;
    }
}

std::vector<AuditLogFormatter::Row> AuditLogFormatter::addPage(const Discord::AuditLog &page)
{
    for (const Discord::User &user : page.users)
        users.insert_or_assign(user.id.get(), user);
    auto index = [](std::unordered_map<Core::Snowflake, nlohmann::json> &into, const nlohmann::json &items) {
        for (const nlohmann::json &item : items)
            into.insert_or_assign(snowflakeOf(jsonMember(item, "id")), item);
    };
    index(integrations, page.integrations);
    index(webhooks, page.webhooks);
    index(scheduledEvents, page.guildScheduledEvents);
    index(autoModerationRules, page.autoModerationRules);
    index(threads, page.threads);
    index(applicationCommands, page.applicationCommands);

    std::vector<Entry> entries;
    for (auto it = page.entries.crbegin(); it != page.entries.crend(); ++it) {
        const Discord::AuditLogEntry &raw = *it;
        Entry entry;
        entry.raw = raw;
        entry.action = raw.actionType.get();
        entry.target = targetOf(entry.action);
        entry.category = categoryOf(entry.action);
        entry.id = raw.id.get();
        entry.userId = raw.userId.hasValue() ? raw.userId.get() : Core::Snowflake();
        entry.targetId = raw.targetId.hasValue() ? raw.targetId.get() : Core::Snowflake();
        entry.time = entry.id.toDateTime();

        if (raw.reason.hasValue() && !raw.reason->empty())
            entry.changes.push_back({ std::string("reason"), nlohmann::json(), raw.reason.get() });
        for (const Discord::AuditLogChange &change : raw.changes)
            entry.changes.push_back({ change.key, change.oldValue, change.newValue });
        if (entry.action == Action::MEMBER_PRUNE)
            entry.changes.push_back({ std::string("prune_delete_days"), nlohmann::json(),
                                   raw.options.contains("delete_member_days") ? jsonMember(raw.options, "delete_member_days")
                                                                              : nlohmann::json(1) });
        if (entry.action == Action::AUTO_MODERATION_BLOCK_MESSAGE && raw.options.contains("auto_moderation_rule_name"))
            entry.changes.push_back({ std::string("triggered_rule_name"), nlohmann::json(), jsonMember(raw.options, "auto_moderation_rule_name") });
        if (entry.action == Action::VOICE_CHANNEL_STATUS_CREATE && raw.options.contains("status"))
            entry.changes.push_back({ std::string("status"), nlohmann::json(), jsonMember(raw.options, "status") });
        entries.push_back(entry);
    }

    std::vector<std::vector<Entry>> groups;
    for (const Entry &entry : entries) {
        if (!groups.empty()) {
            const Entry &first = groups.back().front();
            const bool mergeable = first.action == entry.action &&
                                   first.targetId == entry.targetId &&
                                   first.userId == entry.userId &&
                                   first.raw.options == entry.raw.options &&
                                   first.time.secondsUntil(entry.time) < MergeWindowSecs &&
                                   groups.back().size() <= MaxMerges &&
                                   entry.target != Target::Invite &&
                                   !neverMerged(entry.action);
            if (mergeable) {
                groups.back().push_back(entry);
                continue;
            }
        }
        rememberDeletedName(entry);
        groups.push_back({ entry });
    }

    std::vector<Row> rows;
    for (auto it = groups.crbegin(); it != groups.crend(); ++it) {
        const Entry &first = it->front();
        const bool keptWithoutTarget = first.action == Action::MEMBER_PRUNE ||
                                       first.action == Action::MEMBER_DISCONNECT ||
                                       first.action == Action::MEMBER_MOVE ||
                                       first.action == Action::CREATOR_MONETIZATION_REQUEST_CREATED ||
                                       first.action == Action::CREATOR_MONETIZATION_TERMS_ACCEPTED;
        if (first.target == Target::Unknown)
            continue;
        if (targetName(first).empty() && !keptWithoutTarget)
            continue;
        rows.push_back(format(*it));
    }
    return rows;
}

void AuditLogFormatter::rememberDeletedName(const Entry &entry)
{
    if (entry.category != Category::Delete || !entry.targetId.isValid())
        return;

    std::string name;
    bool textLike = false;
    for (const Change &change : entry.changes) {
        if ((change.key == std::string_view("name") || change.key == std::string_view("title")) && name.empty())
            name = text(change.oldValue);
        if (change.key == std::string_view("type"))
            textLike = isTextLikeChannelType(number(change.oldValue));
    }
    if (name.empty())
        return;
    if ((entry.target == Target::Channel || entry.target == Target::ChannelOverwrite) && textLike)
        name.insert(name.begin(), '#');
    deletedNames.insert_or_assign(std::make_pair(int(entry.target), entry.targetId), name);
}

std::optional<Discord::User> AuditLogFormatter::user(Core::Snowflake userId) const
{
    const auto known = users.find(userId);
    if (known != users.end())
        return known->second;
    return instance ? instance->users()->getUser(userId) : std::nullopt;
}

std::string AuditLogFormatter::userTag(Core::Snowflake userId) const
{
    const auto found = user(userId);
    return found ? found->tag() : std::to_string(uint64_t(userId));
}

std::string AuditLogFormatter::channelName(Core::Snowflake channelId) const
{
    const auto channel = instance ? instance->getChannel(channelId) : std::nullopt;
    if (!channel || !channel->name.hasValue())
        return std::to_string(uint64_t(channelId));

    const std::string name = channel->name.get();
    switch (channel->type.valueOr(Discord::ChannelType::GUILD_TEXT)) {
    case Discord::ChannelType::GUILD_VOICE:
    case Discord::ChannelType::GUILD_STAGE_VOICE:
    case Discord::ChannelType::GUILD_CATEGORY:
        return name;
    case Discord::ChannelType::NEWS_THREAD:
    case Discord::ChannelType::PUBLIC_THREAD:
    case Discord::ChannelType::PRIVATE_THREAD:
        return std::format("\"{0}\"", name);
    default:
        return char('#') + name;
    }
}

std::string AuditLogFormatter::roleName(Core::Snowflake roleId) const
{
    if (instance)
        for (const Discord::Role &role : instance->getRolesForGuild(guildId))
            if (role.id.get() == roleId)
                return role.name.get();
    return {};
}

std::string AuditLogFormatter::authorName(const Entry &entry) const
{
    if (entry.userId.isValid()) {
        if (const auto found = user(entry.userId))
            return found->tag();
    }
    if (entry.raw.options.contains("integration_type"))
        return integrationPlatform(text(jsonMember(entry.raw.options, "integration_type")));
    return std::string("Unknown User");
}

std::string AuditLogFormatter::targetName(const Entry &entry) const
{
    const Core::Snowflake id = entry.targetId;
    auto changedValue = [&entry](const char *key) -> std::string {
        for (const Change &change : entry.changes)
            if (change.key == std::string_view(key))
                return text(truthy(change.newValue) ? change.newValue : change.oldValue);
        return {};
    };
    auto lookedUp = [&id](const std::unordered_map<Core::Snowflake, nlohmann::json> &lookup, const char *field) -> std::string {
        const auto found = lookup.find(id);
        return found != lookup.end() ? text(jsonMember(found->second, field)) : std::string();
    };
    auto fallback = [&](const std::string &found, const char *key) -> std::string {
        if (!found.empty())
            return found;
        const auto deleted = deletedNames.find(std::make_pair(int(entry.target), id));
        if (deleted != deletedNames.end())
            return deleted->second;
        const std::string changed = key ? changedValue(key) : std::string();
        if (!changed.empty())
            return changed;
        return id.isValid() ? std::to_string(uint64_t(id)) : std::string();
    };

    switch (entry.target) {
    case Target::Guild:
    case Target::GuildHome:
    case Target::GuildProfile:
    case Target::Onboarding:
    case Target::MemberVerification: {
        const auto guild = instance ? instance->getGuild(guildId) : std::nullopt;
        return guild ? guild->name.get() : std::string();
    }
    case Target::Channel:
    case Target::ChannelOverwrite:
    case Target::VoiceChannelStatus: {
        const auto channel = instance && id.isValid() ? instance->getChannel(id) : std::nullopt;
        return fallback(channel ? channelName(id) : std::string(), entry.target == Target::VoiceChannelStatus ? "status" : "name");
    }
    case Target::User: {
        const auto found = id.isValid() ? user(id) : std::nullopt;
        return fallback(found ? found->tag() : std::string(), "nick");
    }
    case Target::Role:
        return fallback(id.isValid() ? roleName(id) : std::string(), "name");
    case Target::OnboardingPrompt: {
        const std::string title = changedValue("title");
        return title.empty() ? std::string("[empty]") : title;
    }
    case Target::Invite:
        return fallback(std::string(), "code");
    case Target::Integration:
        return fallback(lookedUp(integrations, "name"), "type");
    case Target::Webhook:
        return fallback(lookedUp(webhooks, "name"), "name");
    case Target::Emoji:
    case Target::Sticker:
    case Target::Soundboard:
        return fallback(std::string(), "name");
    case Target::StageInstance:
        return fallback(std::string(), "topic");
    case Target::ScheduledEvent:
    case Target::ScheduledEventException:
        return fallback(lookedUp(scheduledEvents, "name"), "name");
    case Target::Thread:
        return fallback(lookedUp(threads, "name"), "name");
    case Target::AutoModerationRule:
        return fallback(lookedUp(autoModerationRules, "name"), "name");
    case Target::ApplicationCommand: {
        const Core::Snowflake applicationId = snowflakeOf(jsonMember(entry.raw.options, "application_id"));
        if (id == applicationId) {
            for (const auto &[integrationId, integration] : integrations)
                if (snowflakeOf(jsonMember(jsonMember(integration, "application"), "id")) == applicationId)
                    return text(jsonMember(integration, "name"));
            return std::to_string(uint64_t(id));
        }
        const auto command = applicationCommands.find(id);
        if (command == applicationCommands.end())
            return fallback(std::string(), "name");
        std::string name = text(jsonMember(command->second, "name_localized"));
        if (name.empty())
            name = text(jsonMember(command->second, "name"));
        return command->second.value("type", 1) == 1 ? std::string("/\u2060") + name : name;
    }
    case Target::HomeSettings:
        return std::string("Server Guide");
    case Target::Unknown:
        break;
    }
    return {};
}

std::string AuditLogFormatter::titleFor(const Entry &entry, const std::string &target, const std::string &count, const std::string &channel, const std::string &subtarget) const
{
    const std::string u = bold(authorName(entry));
    const std::string t = bold(target);
    const std::string c = bold(channel);
    const int64_t n = number(count);
    auto counted = [n](const char *one, const char *other) { return std::format("<b>{0}</b>", Platform::htmlEscape(countText(n, one, other))); };
    auto typeChange = [&entry]() -> std::optional<int64_t> {
        for (const Change &change : entry.changes)
            if (change.key == std::string_view("type") && !isNull(change.newValue))
                return number(change.newValue);
        return std::nullopt;
    };

    switch (entry.action) {
    case Action::GUILD_UPDATE:
    case Action::CHANNEL_UPDATE:
        return std::format("{0} made changes to {1}", u, t);
    case Action::CHANNEL_CREATE:
        switch (typeChange().value_or(0)) {
        case 13:
            return std::format("{0} created a stage channel {1}", u, t);
        case 2:
            return std::format("{0} created a voice channel {1}", u, t);
        case 4:
            return std::format("{0} created a category {1}", u, t);
        case 15:
            return std::format("{0} created a forum channel {1}", u, t);
        case 16:
            return std::format("{0} created a media channel {1}", u, t);
        case 5:
            return std::format("{0} created an announcement channel {1}", u, t);
        default:
            return std::format("{0} created a text channel {1}", u, t);
        }
    case Action::CHANNEL_DELETE:
        return std::format("{0} removed {1}", u, t);
    case Action::CHANNEL_OVERWRITE_CREATE:
        return std::format("{0} created channel overrides for {1}", u, t);
    case Action::CHANNEL_OVERWRITE_UPDATE:
        return std::format("{0} updated channel overrides for {1}", u, t);
    case Action::CHANNEL_OVERWRITE_DELETE:
        return std::format("{0} removed channel overrides for {1}", u, t);
    case Action::MEMBER_KICK:
        return std::format("{0} kicked {1}", u, t);
    case Action::MEMBER_PRUNE:
        return std::format("{0} pruned {1}", u, counted("a member", "{0} members"));
    case Action::MEMBER_BAN_ADD:
        return std::format("{0} banned {1}", u, t);
    case Action::MEMBER_BAN_REMOVE:
        return std::format("{0} removed the ban for {1}", u, t);
    case Action::MEMBER_UPDATE:
        return std::format("{0} updated {1}", u, t);
    case Action::MEMBER_ROLE_UPDATE:
        return std::format("{0} updated roles for {1}", u, t);
    case Action::MEMBER_MOVE:
        return std::format("{0} moved {1} to {2}", u, counted("a user", "{0} users"), c);
    case Action::MEMBER_DISCONNECT:
        return std::format("{0} disconnected {1} from voice", u, counted("a user", "{0} users"));
    case Action::BOT_ADD:
        return std::format("{0} added {1} to the server", u, t);
    case Action::ROLE_CREATE:
        return std::format("{0} created the role {1}", u, t);
    case Action::ROLE_UPDATE:
        return std::format("{0} updated the role {1}", u, t);
    case Action::ROLE_DELETE:
        return std::format("{0} deleted the role {1}", u, t);
    case Action::INVITE_CREATE:
        return std::format("{0} created an invite {1}", u, t);
    case Action::INVITE_UPDATE:
        return std::format("{0} updated an invite {1}", u, t);
    case Action::INVITE_DELETE:
        return std::format("{0} deleted an invite {1}", u, t);
    case Action::WEBHOOK_CREATE:
        return std::format("{0} created the webhook {1}", u, t);
    case Action::WEBHOOK_UPDATE:
        return std::format("{0} updated the webhook {1}", u, t);
    case Action::WEBHOOK_DELETE:
        return std::format("{0} deleted the webhook {1}", u, t);
    case Action::EMOJI_CREATE:
        return std::format("{0} created the emoji {1}", u, t);
    case Action::EMOJI_UPDATE:
        return std::format("{0} updated the emoji {1}", u, t);
    case Action::EMOJI_DELETE:
        return std::format("{0} deleted the emoji {1}", u, t);
    case Action::STICKER_CREATE:
        return std::format("{0} created the sticker {1}", u, t);
    case Action::STICKER_UPDATE:
        return std::format("{0} updated the sticker {1}", u, t);
    case Action::STICKER_DELETE:
        return std::format("{0} deleted the sticker {1}", u, t);
    case Action::MESSAGE_DELETE:
        return std::format("{0} deleted {1} by {2} in {3}", u, counted("a message", "{0} messages"), t, c);
    case Action::MESSAGE_BULK_DELETE:
        return std::format("{0} deleted {1} in {2}", u, counted("a message", "{0} messages"), t);
    case Action::MESSAGE_PIN:
        return std::format("{0} pinned a message by {1} in {2}", u, t, c);
    case Action::MESSAGE_UNPIN:
        return std::format("{0} unpinned a message by {1} in {2}", u, t, c);
    case Action::INTEGRATION_CREATE:
        return std::format("{0} added an integration for {1}", u, t);
    case Action::INTEGRATION_UPDATE:
        return std::format("{0} updated the integration for {1}", u, t);
    case Action::INTEGRATION_DELETE:
        return std::format("{0} deleted the integration for {1}", u, t);
    case Action::STAGE_INSTANCE_CREATE:
        return std::format("{0} started the stage for {1}", u, c);
    case Action::STAGE_INSTANCE_UPDATE:
        return std::format("{0} updated the stage for {1}", u, c);
    case Action::STAGE_INSTANCE_DELETE:
        if (!entry.userId.isValid())
            return std::format("Discord ended the stage for {0} due to inactivity.", c);
        return std::format("{0} ended the stage for {1}", u, c);
    case Action::GUILD_SCHEDULED_EVENT_CREATE:
        return std::format("{0} scheduled the event {1}", u, t);
    case Action::GUILD_SCHEDULED_EVENT_UPDATE:
        return std::format("{0} updated the scheduled event {1}", u, t);
    case Action::GUILD_SCHEDULED_EVENT_DELETE:
        return std::format("{0} canceled the scheduled event {1}", u, t);
    case Action::GUILD_SCHEDULED_EVENT_EXCEPTION_CREATE:
    case Action::GUILD_SCHEDULED_EVENT_EXCEPTION_UPDATE:
        return std::format("{0} updated an event recurrence for {1} at {2}", u, t, bold(subtarget));
    case Action::GUILD_SCHEDULED_EVENT_EXCEPTION_DELETE:
        return std::format("{0} reset an event recurrence for {1} at {2}", u, t, bold(subtarget));
    case Action::THREAD_CREATE:
        switch (typeChange().value_or(11)) {
        case 12:
            return std::format("{0} created a private thread {1}", u, t);
        case 10:
            return std::format("{0} created an announcement thread {1}", u, t);
        default:
            return std::format("{0} created a thread {1}", u, t);
        }
    case Action::THREAD_UPDATE:
        return std::format("{0} made changes to the thread {1}", u, t);
    case Action::THREAD_DELETE:
        return std::format("{0} deleted the thread {1}", u, t);
    case Action::APPLICATION_COMMAND_PERMISSION_UPDATE:
        return std::format("{0} updated permissions for {1}", u, t);
    case Action::AUTO_MODERATION_BLOCK_MESSAGE:
        return std::format("AutoMod blocked a message sent by {0} in {1}", t, c);
    case Action::AUTO_MODERATION_FLAG_TO_CHANNEL:
        if (text(jsonMember(entry.raw.options, "auto_moderation_rule_trigger_type")) == std::string_view("6"))
            return std::format("AutoMod flagged {0} for violating content in their user profile", u);
        return std::format("AutoMod flagged a message sent by {0} in {1}", t, c);
    case Action::AUTO_MODERATION_USER_COMMUNICATION_DISABLED:
        return std::format("AutoMod timed out {0} for a message posted in {1}", t, c);
    case Action::AUTO_MODERATION_QUARANTINE_USER:
        return std::format("AutoMod quarantined {0} for violating content in their user profile", u);
    case Action::CREATOR_MONETIZATION_REQUEST_CREATED:
        return std::format("{0} applied for monetization", u);
    case Action::CREATOR_MONETIZATION_TERMS_ACCEPTED:
        return std::format("{0} accepted terms for monetization", u);
    case Action::AUTO_MODERATION_RULE_CREATE:
        return std::format("{0} created AutoMod rule {1}", u, t);
    case Action::AUTO_MODERATION_RULE_UPDATE:
        return std::format("{0} updated AutoMod rule {1}", u, t);
    case Action::AUTO_MODERATION_RULE_DELETE:
        return std::format("{0} deleted AutoMod rule {1}", u, t);
    case Action::ONBOARDING_PROMPT_CREATE:
        return std::format("{0} created a new customization question", u);
    case Action::ONBOARDING_PROMPT_UPDATE:
        return std::format("{0} updated the customization question {1}", u, t);
    case Action::ONBOARDING_PROMPT_DELETE:
        return std::format("{0} deleted the customization question {1}", u, t);
    case Action::ONBOARDING_CREATE:
        return std::format("{0} started onboarding for this server", u);
    case Action::ONBOARDING_UPDATE:
        return std::format("{0} updated onboarding for this server", u);
    case Action::HOME_SETTINGS_CREATE:
        return std::format("{0} created the Server Guide for this server", u);
    case Action::HOME_SETTINGS_UPDATE:
        return std::format("{0} updated the Server Guide for this server", u);
    case Action::GUILD_HOME_FEATURE_ITEM: {
        for (const Change &change : entry.changes) {
            if (change.key != std::string_view("entity_type"))
                continue;
            if (text(change.newValue) == std::string_view("message"))
                return std::format("{0} featured a message on Home", u);
            if (text(change.newValue) == std::string_view("forum_post"))
                return std::format("{0} featured a forum post on Home", u);
        }
        return std::format("{0} featured something on Home", u);
    }
    case Action::GUILD_HOME_REMOVE_ITEM:
        return std::format("{0} removed an item from Home", u);
    case Action::SOUNDBOARD_SOUND_CREATE:
        return std::format("{0} uploaded a Soundboard sound", u);
    case Action::SOUNDBOARD_SOUND_UPDATE:
        return std::format("{0} updated a Soundboard sound", u);
    case Action::SOUNDBOARD_SOUND_DELETE:
        return std::format("{0} deleted a Soundboard sound", u);
    case Action::VOICE_CHANNEL_STATUS_CREATE:
        return std::format("{0} set a Voice Channel Status in {1}", u, c);
    case Action::VOICE_CHANNEL_STATUS_DELETE:
        return std::format("{0} removed a Voice Channel Status in {1}", u, c);
    case Action::GUILD_MEMBER_VERIFICATION_UPDATE:
        return std::format("{0} updated member application settings", u);
    case Action::GUILD_PROFILE_UPDATE:
        return std::format("{0} updated server profile settings", u);
    case Action::GUILD_MIGRATE_PIN_PERMISSION:
        return std::format("{0} updated Pin Messages permission", u);
    case Action::GUILD_MIGRATE_BYPASS_SLOWMODE_PERMISSION:
        return std::format("{0} updated Bypass Slowmode permission", u);
    case Action::HARMFUL_LINKS_BLOCKED_MESSAGE:
        break;
    }
    return {};
}

std::vector<AuditLogFormatter::Change> AuditLogFormatter::transform(const Entry &entry) const
{
    std::vector<Change> result;
    const bool onChannel = entry.target == Target::Channel || entry.target == Target::ChannelOverwrite;
    auto mapBoth = [](Change change, const std::function<nlohmann::json(const nlohmann::json &)> &map) {
        if (!isNull(change.oldValue))
            change.oldValue = map(change.oldValue);
        if (!isNull(change.newValue))
            change.newValue = map(change.newValue);
        return change;
    };
    auto channelValue = [this](const nlohmann::json &value) { return nlohmann::json(channelName(snowflakeOf(value))); };
    auto firstTagDifference = [](const Change &change) -> std::optional<Change> {
        std::unordered_map<std::string, nlohmann::json> before;
        for (const nlohmann::json &tag : change.oldValue)
            before.insert_or_assign(text(jsonMember(tag, "id")), tag);
        std::unordered_set<std::string> after;
        for (const nlohmann::json &tag : change.newValue) {
            const nlohmann::json added = tag;
            const std::string id = text(jsonMember(added, "id"));
            after.insert(id);
            const auto previous = before.find(id);
            if (previous == before.end())
                return Change{ std::string("available_tag_add"), nlohmann::json(), tagText(added) };
            if (previous->second != added)
                return Change{ std::string("available_tag_edit"), tagText(previous->second), tagText(added) };
        }
        for (const nlohmann::json &tag : change.oldValue)
            if (!after.contains(text(jsonMember(tag, "id"))))
                return Change{ std::string("available_tag_delete"), nlohmann::json(), tagText(tag) };
        return std::nullopt;
    };

    for (Change change : entry.changes) {
        const std::string &key = change.key;

        if (entry.action == Action::APPLICATION_COMMAND_PERMISSION_UPDATE && key != std::string_view("reason")) {
            const nlohmann::json permission = (isNull(change.oldValue) ? change.newValue : change.oldValue);
            const Core::Snowflake id = snowflakeOf(jsonMember(permission, "id"));
            switch (number(jsonMember(permission, "type"))) {
            case 1:
                change.subtarget = roleName(id);
                break;
            case 2:
                change.subtarget = userTag(id);
                break;
            case 3:
                change.subtarget = uint64_t(id) == uint64_t(guildId) - 1 ? std::string("all channels") : channelName(id);
                break;
            }
            if (change.subtarget.empty())
                change.subtarget = std::to_string(uint64_t(id));
            result.push_back(change);
            continue;
        }

        if (key == std::string_view("owner_id")) {
            result.push_back(mapBoth(change, [this](const nlohmann::json &value) {
                const auto owner = user(snowflakeOf(value));
                return nlohmann::json(owner ? owner->username.get() : std::string("???"));
            }));
        } else if (key == std::string_view("channel_id") ||
                   key == std::string_view("afk_channel_id") ||
                   key == std::string_view("system_channel_id") ||
                   key == std::string_view("rules_channel_id") ||
                   key == std::string_view("public_updates_channel_id") ||
                   key == std::string_view("widget_channel_id")) {
            result.push_back(mapBoth(change, channelValue));
        } else if (key == std::string_view("afk_timeout")) {
            result.push_back(mapBoth(change, [](const nlohmann::json &value) { return nlohmann::json(number(value) / 60); }));
        } else if (key == std::string_view("bitrate")) {
            result.push_back(mapBoth(change, [](const nlohmann::json &value) { return nlohmann::json(number(value) / 1000); }));
        } else if (key == std::string_view("color")) {
            result.push_back(mapBoth(change, [](const nlohmann::json &value) { return nlohmann::json(colorText(value)); }));
        } else if (key == std::string_view("max_age")) {
            result.push_back(mapBoth(change, [](const nlohmann::json &value) { return nlohmann::json(inviteMaxAgeText(number(value))); }));
        } else if (key == std::string_view("permissions") && entry.target == Target::Role) {
            const uint64_t before = bitsOf(change.oldValue);
            const uint64_t after = bitsOf(change.newValue);
            const std::vector<std::string> granted = permissionNames(after & ~before, false);
            const std::vector<std::string> denied = permissionNames(before & ~after, false);
            if (!granted.empty())
                result.push_back({ std::string("allow"), nlohmann::json(), nlohmann::json(), std::string(), granted });
            if (!denied.empty())
                result.push_back({ std::string("deny"), nlohmann::json(), nlohmann::json(), std::string(), denied });
        } else if ((key == std::string_view("allow") || key == std::string_view("deny")) && onChannel) {
            const uint64_t before = bitsOf(change.oldValue);
            const uint64_t after = bitsOf(change.newValue);
            const std::vector<std::string> added = permissionNames(after & ~before, true);
            const std::vector<std::string> reset = permissionNames(before & ~after, true);
            if (!added.empty())
                result.push_back({ key, nlohmann::json(), nlohmann::json(), std::string(), added });
            if (!reset.empty())
                result.push_back({ std::string("reset"), nlohmann::json(), nlohmann::json(), std::string(), reset });
        } else if (key == std::string_view("flags")) {
            const uint64_t before = uint64_t(number(change.oldValue));
            const uint64_t after = uint64_t(number(change.newValue));
            if (entry.target == Target::Invite) {
                if ((after & ~before) == 1)
                    result.push_back({ key, nlohmann::json(), nlohmann::json(1) });
                continue;
            }
            std::vector<std::string> items;
            const uint64_t added = after & ~before;
            const uint64_t removed = before & ~after;
            if (added & 1)
                items.push_back(std::string("Removed channel from Home"));
            if (added & 2)
                items.push_back(std::string("Pinned"));
            if (added & 4)
                items.push_back(std::string("Removed channel from Active Now"));
            if (removed & 1)
                items.push_back(std::string("Added channel back to Home"));
            if (removed & 2)
                items.push_back(std::string("Unpinned"));
            if (removed & 4)
                items.push_back(std::string("Added channel back to Active Now"));
            if (added || removed)
                result.push_back({ key, change.oldValue, change.newValue, std::string(), items });
        } else if (key == std::string_view("preferred_locale")) {
            result.push_back(mapBoth(change, [](const nlohmann::json &value) { return nlohmann::json(localeName(text(value))); }));
        } else if (key == std::string_view("video_quality_mode")) {
            result.push_back(mapBoth(change, [](const nlohmann::json &value) { return nlohmann::json(number(value) == 2 ? std::string("720p") : std::string("Auto")); }));
        } else if (key == std::string_view("system_channel_flags")) {
            const uint64_t before = uint64_t(number(change.oldValue));
            const uint64_t after = uint64_t(number(change.newValue));
            static const std::pair<uint64_t, const char *> flagKeys[] = {
                { 1, "join_notifications" },
                { 2, "premium_subscriptions" },
                { 4, "reminder_notifications" },
                { 8, "join_notification_replies" },
            };
            for (const auto &flagKey : flagKeys)
                if ((before & flagKey.first) != (after & flagKey.first))
                    result.push_back({ std::string(flagKey.second), nlohmann::json(!(before & flagKey.first)), nlohmann::json(!(after & flagKey.first)) });
        } else if (key == std::string_view("actions") && entry.target == Target::AutoModerationRule) {
            result.push_back(mapBoth(change, [](const nlohmann::json &value) {
                std::vector<std::string> names;
                for (const nlohmann::json &action : value)
                    names.push_back(autoModActionName(number(jsonMember(action, "type"))));
                return nlohmann::json(join(names, std::string(", ")));
            }));
        } else if (key == std::string_view("event_type") && entry.target == Target::AutoModerationRule) {
            result.push_back(mapBoth(change, [](const nlohmann::json &value) { return nlohmann::json(autoModEventName(number(value))); }));
        } else if (key == std::string_view("trigger_type") && entry.target == Target::AutoModerationRule) {
            result.push_back(mapBoth(change, [](const nlohmann::json &value) { return nlohmann::json(autoModTriggerName(number(value))); }));
        } else if (key == std::string_view("trigger_metadata")) {
            result.push_back(mapBoth(change, [](const nlohmann::json &value) {
                const nlohmann::json metadata = value;
                if (jsonMember(metadata, "keyword_filter").is_array())
                    return nlohmann::json(std::format("keywords to {0}", quotedList(jsonMember(metadata, "keyword_filter"))));
                return nlohmann::json(text(value));
            }));
        } else if (key.starts_with(std::string_view("$add_")) || key.starts_with(std::string_view("$remove_"))) {
            result.push_back(mapBoth(change, [](const nlohmann::json &value) { return nlohmann::json(quotedList(value)); }));
        } else if (key == std::string_view("exempt_channels") || key == std::string_view("exempt_roles")) {
            const bool channels = key == std::string_view("exempt_channels");
            result.push_back(mapBoth(change, [this, channels](const nlohmann::json &value) {
                std::vector<std::string> names;
                for (const nlohmann::json &id : value) {
                    const std::string name = channels ? (instance && instance->getChannel(snowflakeOf(id)) ? channelName(snowflakeOf(id)) : std::string())
                                                  : roleName(snowflakeOf(id));
                    if (!name.empty())
                        names.push_back(name);
                }
                return nlohmann::json(names.empty() ? std::string("None") : join(names, std::string(", ")));
            }));
        } else if ((key == std::string_view("$add") || key == std::string_view("$remove")) && entry.target == Target::User) {
            for (const nlohmann::json &role : change.newValue)
                change.items.push_back(text(jsonMember(role, "name")));
            result.push_back(change);
        } else if (key == std::string_view("role_ids") && entry.target == Target::Invite) {
            for (const nlohmann::json &roleId : change.newValue) {
                const std::string name = roleName(snowflakeOf(roleId));
                if (!name.empty())
                    change.items.push_back(name);
            }
            result.push_back(change);
        } else if (key == std::string_view("available_tags")) {
            if (const auto difference = firstTagDifference(change))
                result.push_back(*difference);
        } else if (key == std::string_view("applied_tags")) {
            std::unordered_map<Core::Snowflake, std::string> tagNames;
            const auto thread = threads.find(entry.targetId);
            const Core::Snowflake forumId = thread != threads.end() ? snowflakeOf(jsonMember(thread->second, "parent_id")) : Core::Snowflake();
            if (const auto forum = instance && forumId.isValid() ? instance->getChannel(forumId) : std::nullopt; forum && forum->availableTags.hasValue())
                for (const Discord::ForumTag &tag : forum->availableTags.get())
                    tagNames.insert_or_assign(tag.id.get(), tag.emojiName.hasValue() && !tag.emojiName->empty()
                                                          ? tag.emojiName.get() + char(' ') + tag.name.get()
                                                          : tag.name.get());
            auto nameOf = [&tagNames](const nlohmann::json &id) {
                const auto found = tagNames.find(snowflakeOf(id));
                return found != tagNames.end() ? found->second : text(id);
            };
            std::unordered_set<std::string> before;
            for (const nlohmann::json &id : change.oldValue)
                before.insert(text(id));
            std::unordered_set<std::string> after;
            for (const nlohmann::json &id : change.newValue)
                after.insert(text(id));
            for (const nlohmann::json &id : change.newValue)
                if (!before.contains(text(id)))
                    result.push_back({ std::string("available_tag_add"), nlohmann::json(), nameOf(id) });
            for (const nlohmann::json &id : change.oldValue)
                if (!after.contains(text(id)))
                    result.push_back({ std::string("available_tag_delete"), nlohmann::json(), nameOf(id) });
        } else if (key == std::string_view("scheduled_start_time") || key == std::string_view("scheduled_end_time")) {
            result.push_back(mapBoth(change, [](const nlohmann::json &value) { return nlohmann::json(longDateTime(value)); }));
        } else if (key == std::string_view("communication_disabled_until") && entry.action == Action::MEMBER_UPDATE) {
            result.push_back(mapBoth(change, [](const nlohmann::json &value) {
                const Platform::DateTime until = Platform::DateTime::parse(text(value));
                return until.valid() ? nlohmann::json(calendar(until)) : value;
            }));
        } else if (key == std::string_view("type") && onChannel) {
            result.push_back(mapBoth(change, [](const nlohmann::json &value) { return nlohmann::json(channelTypeName(number(value))); }));
        } else {
            result.push_back(change);
        }
    }
    return result;
}

std::optional<AuditLogFormatter::Line> AuditLogFormatter::describe(const Entry &entry, const Change &change) const
{
    const std::string &key = change.key;
    const nlohmann::json &oldValue = change.oldValue;
    const nlohmann::json &newValue = change.newValue;
    const bool hasOld = !isNull(oldValue);
    const bool hasNew = !isNull(newValue);
    const std::string oldText = bold(text(oldValue));
    const std::string newText = bold(text(newValue));
    const int64_t count = change.items.empty() && newValue.is_array() ? newValue.size() : change.items.size();

    auto line = [&change](const std::string &html) { return std::optional<Line>(Line{ html, change.items }); };
    auto createdOrChanged = [&](const std::string &created, const std::string &changed) { return line(hasOld ? changed : created); };
    auto clearedOrSet = [&](const std::string &cleared, const std::string &set) { return line(hasNew ? set : cleared); };
    auto changedSetCleared = [&](const std::string &changed, const std::string &set, const std::string &cleared) -> std::optional<Line> {
        if (hasOld && hasNew)
            return line(changed);
        if (hasNew)
            return line(set);
        if (hasOld)
            return line(cleared);
        return std::nullopt;
    };
    auto onOff = [&](const std::string &on, const std::string &off) { return line(truthy(newValue) ? on : off); };
    auto nameLines = [&](const char *created) {
        return createdOrChanged(std::vformat(created, std::make_format_args(newText)), std::format("Changed the name from {0} to {1}", oldText, newText));
    };
    auto slowmode = [&]() {
        const int64_t seconds = number(newValue);
        if (seconds == 0)
            return line(hasOld ? std::string("Disabled slowmode") : std::string("Set slowmode disabled"));
        return line(std::format("Set slowmode to {0}", std::format("<b>{0}</b>", countText(seconds, "1 second", "{0} seconds"))));
    };
    auto hideDuration = [&](const char *removed, const char *set, const char *changed) {
        const int64_t minutes = number(newValue);
        if (minutes == 0)
            return line(tr(removed));
        const std::string duration = std::format("<b>{0}</b>", countText(minutes, "1 minute", "{0} minutes"));
        return line(std::vformat(hasOld ? changed : set, std::make_format_args(duration)));
    };

    if (key == std::string_view("reason")) {
        if (entry.target == Target::GuildProfile)
            return std::nullopt;
        return line(std::format("With reason {0}", newText));
    }

    if (entry.category == Category::Delete &&
        entry.action != Action::MEMBER_KICK &&
        entry.action != Action::MEMBER_BAN_ADD &&
        entry.action != Action::MEMBER_PRUNE)
        return std::nullopt;

    switch (entry.target) {
    case Target::Guild:
        if (key == std::string_view("name"))
            return line(std::format("Set the server name to {0}", newText));
        if (key == std::string_view("description"))
            return clearedOrSet(std::string("<b>Cleared</b> the server description"), std::format("Set the server description to {0}", newText));
        if (key == std::string_view("icon_hash"))
            return line(std::string("Set the server icon"));
        if (key == std::string_view("splash_hash"))
            return line(std::string("Set the server invite background"));
        if (key == std::string_view("discovery_splash_hash"))
            return line(std::string("Set the Server Discovery background"));
        if (key == std::string_view("banner_hash"))
            return clearedOrSet(std::string("Removed the server banner"), std::string("Set the server banner"));
        if (key == std::string_view("owner_id"))
            return line(std::format("Passed ownership to {0}", newText));
        if (key == std::string_view("region"))
            return line(std::format("Set the voice region to {0}", newText));
        if (key == std::string_view("preferred_locale"))
            return line(std::format("Set the preferred locale to {0}", newText));
        if (key == std::string_view("afk_channel_id"))
            return clearedOrSet(std::string("<b>Cleared</b> the inactive channel"), std::format("Set the inactive channel to {0}", newText));
        if (key == std::string_view("afk_timeout"))
            return line(std::format("Set the inactive timeout to {0} minutes", newText));
        if (key == std::string_view("system_channel_id"))
            return clearedOrSet(std::string("<b>Disabled</b> the welcome notification messages"), std::format("Set the welcome notification channel to {0}", newText));
        if (key == std::string_view("rules_channel_id"))
            return clearedOrSet(std::string("<b>Cleared</b> the rules channel"), std::format("Set the rules channel to {0}", newText));
        if (key == std::string_view("public_updates_channel_id"))
            return clearedOrSet(std::string("<b>Cleared</b> the Community Server updates channel"), std::format("Set the Community Server updates channel to {0}", newText));
        if (key == std::string_view("mfa_level")) {
            if (!hasNew)
                return std::nullopt;
            return line(number(newValue) == 1 ? std::string("<b>Enabled</b> two-factor authentication requirement")
                                              : std::string("<b>Disabled</b> two-factor authentication requirement"));
        }
        if (key == std::string_view("widget_enabled"))
            return onOff(std::string("<b>Enabled</b> the widget"), std::string("<b>Disabled</b> the widget"));
        if (key == std::string_view("widget_channel_id"))
            return clearedOrSet(std::string("<b>Removed</b> the widget channel"), std::format("Set the widget channel to {0}", newText));
        if (key == std::string_view("verification_level")) {
            static const char *const levels[] = {
                "None",
                "Low",
                "Medium",
                "High",
                "Very High",
            };
            const int64_t level = number(newValue);
            if (!hasNew || level < 0 || level > 4)
                return std::nullopt;
            return line(std::format("Set the server verification level to {0}", bold(tr(levels[level]))));
        }
        if (key == std::string_view("default_message_notifications")) {
            if (!hasNew || number(newValue) > 1)
                return std::nullopt;
            return line(std::format("Set the default message notification setting to {0}", bold(number(newValue) == 0 ? std::string("All Messages") : std::string("Only Mentions"))));
        }
        if (key == std::string_view("vanity_url_code"))
            return clearedOrSet(std::string("<b>Removed</b> the Custom Invite Link"), std::format("Set the Custom Invite Link to {0}", newText));
        if (key == std::string_view("explicit_content_filter")) {
            switch (hasNew ? number(newValue) : -1) {
            case 0:
                return line(std::string("<b>Disabled</b> the explicit content filter"));
            case 1:
                return line(std::format("Set the explicit content filter to scan messages from {0}", bold(std::string("members without a role"))));
            case 2:
                return line(std::format("Set the explicit content filter to scan messages from {0}", bold(std::string("all members"))));
            }
            return std::nullopt;
        }
        if (key == std::string_view("premium_progress_bar_enabled"))
            return onOff(std::string("Turned <b>on</b> the Boost progress bar"), std::string("Turned <b>off</b> the Boost progress bar"));
        if (key == std::string_view("triggered_rule_name"))
            return line(std::format("Detected by rule {0}", newText));
        if (key == std::string_view("join_notifications"))
            return line(std::format("Set system channel welcome messages to {0}", newText));
        if (key == std::string_view("premium_subscriptions"))
            return line(std::format("Set system channel boost notifications to {0}", newText));
        if (key == std::string_view("reminder_notifications"))
            return line(std::format("Set system channel server setup tips to {0}", newText));
        if (key == std::string_view("join_notification_replies"))
            return line(std::format("Set system channel welcome stickers to {0}", newText));
        return std::nullopt;

    case Target::Channel:
    case Target::ChannelOverwrite:
        if (key == std::string_view("id") ||
            key == std::string_view("permission_overwrites") ||
            (entry.target == Target::ChannelOverwrite && key == std::string_view("type")))
            return std::nullopt;
        if (key == std::string_view("name"))
            return nameLines("Set the name to {0}");
        if (key == std::string_view("position"))
            return createdOrChanged(std::format("In position {0}", newText), std::format("Moved from {0} to {1}", oldText, newText));
        if (key == std::string_view("topic"))
            return changedSetCleared(std::format("Changed the topic to {0}", newText), std::format("Set the topic to {0}", newText), std::string("<b>Cleared</b> the topic"));
        if (key == std::string_view("bitrate"))
            return createdOrChanged(std::format("Set the bitrate to {0}", newText), std::format("Changed the bitrate to {0}", newText));
        if (key == std::string_view("rtc_region"))
            return changedSetCleared(std::format("Changed the region override from {0} to {1}", oldText, newText),
                                     std::format("Set the region override to {0}", newText), std::string("Removed the region override"));
        if (key == std::string_view("user_limit"))
            return createdOrChanged(std::format("Set the user limit to {0}", newText), std::format("Changed the user limit to {0}", newText));
        if (key == std::string_view("rate_limit_per_user"))
            return slowmode();
        if (key == std::string_view("reset") || key == std::string_view("allow") || key == std::string_view("deny")) {
            const std::string permissions = count == 1 ? std::string("a permission") : std::string("permissions");
            std::string subtarget = text(jsonMember(entry.raw.options, "type")) == std::string_view("1")
                                        ? userTag(snowflakeOf(jsonMember(entry.raw.options, "id")))
                                        : text(jsonMember(entry.raw.options, "role_name"));
            if (key == std::string_view("reset"))
                return line(std::format("<b>Reset</b> {0} for {1}", permissions, bold(subtarget)));
            if (key == std::string_view("allow"))
                return line(std::format("<b>Granted</b> {0} for {1}", permissions, bold(subtarget)));
            return line(std::format("<b>Denied</b> {0} for {1}", permissions, bold(subtarget)));
        }
        if (key == std::string_view("nsfw"))
            return onOff(std::string("Marked the channel as age-restricted"), std::string("Unmarked the channel as age-restricted"));
        if (key == std::string_view("type"))
            return createdOrChanged(std::format("Set the type to {0}", newText), std::format("Changed the type from {0} to {1}", oldText, newText));
        if (key == std::string_view("video_quality_mode"))
            return line(std::format("Set the video quality mode to {0}", newText));
        if (key == std::string_view("default_auto_archive_duration"))
            return hideDuration("Removed default hide duration",
                                "Set default hide duration to {0}",
                                "Changed default hide duration to {0}");
        if (key == std::string_view("default_thread_rate_limit_per_user")) {
            const std::string seconds = std::format("<b>{0}</b>", countText(number(newValue), "1 second", "{0} seconds"));
            return changedSetCleared(std::format("Changed default thread slowmode to {0}", seconds),
                                     std::format("Set default thread slowmode to {0}", seconds), std::string("Disabled default thread slowmode"));
        }
        if (key == std::string_view("flags"))
            return line(std::string("Changed channel settings"));
        if (key == std::string_view("available_tag_add"))
            return line(std::format("Added tag {0}", newText));
        if (key == std::string_view("available_tag_edit"))
            return line(std::format("Updated tag from {0} to {1}", oldText, newText));
        if (key == std::string_view("available_tag_delete"))
            return line(std::format("Deleted tag {0}", newText));
        return std::nullopt;

    case Target::User:
        if (key == std::string_view("nick"))
            return changedSetCleared(std::format("Changed their nickname from {0} to {1}", oldText, newText),
                                     std::format("Set their nickname to {0}", newText), std::format("<b>Removed</b> their nickname of {0}", oldText));
        if (key == std::string_view("deaf"))
            return onOff(std::string("<b>Deafened</b> them"), std::string("<b>Undeafened</b> them"));
        if (key == std::string_view("mute"))
            return onOff(std::string("<b>Muted</b> them"), std::string("<b>Unmuted</b> them"));
        if (key == std::string_view("$remove"))
            return line(std::format("<b>Removed</b> {0}", count == 1 ? std::string("a role") : std::string("some roles")));
        if (key == std::string_view("$add"))
            return line(std::format("<b>Added</b> {0}", count == 1 ? std::string("a role") : std::string("some roles")));
        if (key == std::string_view("prune_delete_days"))
            return line(std::format("For {0} of inactivity", std::format("<b>{0}</b>", countText(number(newValue), "a day", "{0} days"))));
        if (key == std::string_view("communication_disabled_until")) {
            if (hasNew)
                return line(std::format("Set timeout for user until {0}", newText));
            return hasOld ? line(std::string("Removed timeout")) : std::nullopt;
        }
        if (key == std::string_view("bypasses_verification"))
            return onOff(std::string("manually verified them"), std::string("removed their manual verification"));
        if (key == std::string_view("triggered_rule_name"))
            return line(std::format("Detected by rule {0}", newText));
        return std::nullopt;

    case Target::Role:
        if (key == std::string_view("name"))
            return nameLines("With the name {0}");
        if (key == std::string_view("description"))
            return createdOrChanged(std::format("With the description {0}", newText), std::format("Changed the description from {0} to {1}", oldText, newText));
        if (key == std::string_view("allow"))
            return line(std::format("<b>Granted</b> {0}", count == 1 ? std::string("permission") : std::string("permissions")));
        if (key == std::string_view("deny"))
            return line(std::format("<b>Denied</b> {0}", count == 1 ? std::string("permission") : std::string("permissions")));
        if (key == std::string_view("color")) {
            if (text(newValue) == std::string_view("#000000"))
                return line(std::string("With no color"));
            return line(std::format("Set the color to {0}", newText + char(' ') + swatch(text(newValue))));
        }
        if (key == std::string_view("colors")) {
            const nlohmann::json colors = newValue;
            if (isNull(jsonMember(colors, "secondary_color")))
                return line(std::string("With no gradient"));
            std::vector<std::string> parts;
            for (const char *field : { "primary_color", "secondary_color", "tertiary_color" })
                if (!isNull(jsonMember(colors, field)))
                    parts.push_back(colorText(jsonMember(colors, field)) + char(' ') + swatch(colorText(jsonMember(colors, field))));
            return line(std::format("Set the gradient to [{0}]", join(parts, std::string(", "))));
        }
        if (key == std::string_view("hoist"))
            return onOff(std::string("Hoisted"), std::string("Not hoisted"));
        if (key == std::string_view("mentionable"))
            return onOff(std::string("Mentionable"), std::string("Not mentionable"));
        if (key == std::string_view("icon_hash"))
            return line(std::string("Set the icon"));
        if (key == std::string_view("unicode_emoji"))
            return line(std::string("Set the unicode emoji"));
        return std::nullopt;

    case Target::Invite:
        if (key == std::string_view("code"))
            return line(std::format("With code {0}", newText));
        if (key == std::string_view("channel_id"))
            return line(std::format("For channel {0}", newText));
        if (key == std::string_view("max_uses"))
            return line(number(newValue) == 0 ? std::string("Which has <b>unlimited</b> uses") : std::format("Which expires after {0} uses", newText));
        if (key == std::string_view("max_age"))
            return line(text(newValue) == std::string("Never") ? std::string("Which <b>never</b> expires") : std::format("Which expires after {0}", newText));
        if (key == std::string_view("temporary"))
            return onOff(std::string("With temporary <b>on</b>"), std::string("With temporary <b>off</b>"));
        if (key == std::string_view("flags"))
            return line(std::string("Which is a <b>guest</b> invite"));
        if (key == std::string_view("role_ids")) {
            Line roles{ std::format("With {0}", count == 1 ? std::string("<b>a role</b>") : std::string("<b>some roles</b>")), {} };
            if (entry.action == Action::INVITE_CREATE)
                roles.items = change.items;
            return roles;
        }
        return std::nullopt;

    case Target::Webhook:
        if (key == std::string_view("channel_id"))
            return createdOrChanged(std::format("With channel {0}", newText), std::format("Changed the channel to {0}", newText));
        if (key == std::string_view("name"))
            return nameLines("With name {0}");
        if (key == std::string_view("avatar_hash"))
            return line(std::string("Changed the avatar"));
        return std::nullopt;

    case Target::Emoji:
        if (key == std::string_view("name"))
            return nameLines("With the name {0}");
        return std::nullopt;

    case Target::Sticker:
        if (key == std::string_view("name"))
            return nameLines("With the name {0}");
        if (key == std::string_view("tags"))
            return createdOrChanged(std::format("With the tags {0}", newText), std::format("Changed the tags from {0} to {1}", oldText, newText));
        if (key == std::string_view("description"))
            return createdOrChanged(std::format("With the description {0}", newText), std::format("Changed the description from {0} to {1}", oldText, newText));
        return std::nullopt;

    case Target::Integration:
        if (key == std::string_view("enable_emoticons"))
            return onOff(std::string("Enabled emoticons"), std::string("Disabled custom emoticons"));
        if (key == std::string_view("expire_behavior")) {
            if (!hasNew || number(newValue) > 1)
                return std::nullopt;
            return line(std::format("Set the expired sub behavior to {0}", bold(number(newValue) == 0 ? std::string("Remove Role") : std::string("Kick"))));
        }
        if (key == std::string_view("expire_grace_period"))
            return line(std::format("Set the expire grace period to {0}", std::format("<b>{0}</b>", countText(number(newValue), "a day", "{0} days"))));
        return std::nullopt;

    case Target::StageInstance:
    case Target::ScheduledEvent:
        if (key == std::string_view("privacy_level")) {
            switch (hasNew ? number(newValue) : 0) {
            case 1:
                return line(std::format("Set the privacy level to {0}", bold(std::string("Public"))));
            case 2:
                return line(std::format("Set the privacy level to {0}", bold(std::string("Closed"))));
            }
            return std::nullopt;
        }
        if (entry.target == Target::StageInstance) {
            if (key == std::string_view("topic"))
                return createdOrChanged(std::format("Set the topic to {0}", newText), std::format("Changed the topic to {0}", newText));
            return std::nullopt;
        }
        if (key == std::string_view("name"))
            return line(std::format("With the name {0}", newText));
        if (key == std::string_view("description"))
            return line(std::format("Set the description to {0}", newText));
        if (key == std::string_view("status")) {
            static const char *const statuses[] = {
                "Scheduled",
                "Active",
                "Completed",
                "Canceled",
            };
            const int64_t status = number(newValue);
            if (!hasNew || status < 1 || status > 4)
                return std::nullopt;
            return line(std::format("Set the status to {0}", bold(tr(statuses[status - 1]))));
        }
        if (key == std::string_view("entity_type")) {
            switch (hasNew ? number(newValue) : -1) {
            case 0:
                return line(std::string("Linked to <b>Nothing</b>"));
            case 1:
                return line(std::string("Linked with a <b>Stage</b>"));
            case 2:
                return line(std::string("Linked with a <b>Voice Channel</b>"));
            case 3:
                return line(std::string("Linked <b>externally</b>"));
            }
            return std::nullopt;
        }
        if (key == std::string_view("channel_id"))
            return clearedOrSet(std::string("Removed the channel"), std::format("Set the channel to {0}", newText));
        if (key == std::string_view("location"))
            return clearedOrSet(std::string("Removed the location"), std::format("Set the location to {0}", newText));
        if (key == std::string_view("image_hash"))
            return clearedOrSet(std::string("Removed the cover image"), std::string("Set the cover image"));
        return std::nullopt;

    case Target::ScheduledEventException:
        if (key == std::string_view("scheduled_start_time"))
            return clearedOrSet(std::string("Reset the start time"), std::format("Set the start time to {0}", newText));
        if (key == std::string_view("scheduled_end_time"))
            return clearedOrSet(std::string("Reset the end time"), std::format("Set the end time to {0}", newText));
        if (key == std::string_view("is_canceled")) {
            if (!truthy(oldValue) && truthy(newValue))
                return line(std::string("Canceled this event"));
            if (truthy(oldValue) && hasNew && !truthy(newValue))
                return line(std::string("Restored this event"));
        }
        return std::nullopt;

    case Target::Thread:
        if (key == std::string_view("id") || key == std::string_view("type"))
            return std::nullopt;
        if (key == std::string_view("name"))
            return nameLines("Set the name to {0}");
        if (key == std::string_view("archived"))
            return onOff(std::string("Closed the thread"), std::string("Reopened the thread"));
        if (key == std::string_view("locked"))
            return onOff(std::string("Locked the thread, restricting it to only be opened by moderators"),
                         std::string("Unlocked the thread, allowing it to be opened by non-moderators"));
        if (key == std::string_view("invitable"))
            return onOff(std::string("Allowed non-moderators to add members to the thread"), std::string("Disallowed non-moderators from adding members to the thread"));
        if (key == std::string_view("auto_archive_duration"))
            return hideDuration("Removed auto hide duration",
                                "Set auto hide duration to {0}",
                                "Changed auto hide duration to {0}");
        if (key == std::string_view("rate_limit_per_user"))
            return slowmode();
        if (key == std::string_view("flags")) {
            Line flags{ std::string("Updated thread properties"), {} };
            if (entry.action == Action::THREAD_UPDATE)
                flags.items = change.items;
            return flags;
        }
        if (key == std::string_view("available_tag_add"))
            return line(std::format("Added tag {0}", newText));
        if (key == std::string_view("available_tag_delete"))
            return line(std::format("Deleted tag {0}", newText));
        return std::nullopt;

    case Target::ApplicationCommand: {
        const std::string subtarget = bold(change.subtarget);
        if (!hasNew)
            return line(std::format("<b>Removed override</b> for {0}", subtarget));
        return line(truthy(jsonMember(newValue, "permission")) ? std::format("<b>Granted</b> permission for {0}", subtarget)
                                                                     : std::format("<b>Denied</b> permission for {0}", subtarget));
    }

    case Target::AutoModerationRule:
        if (key == std::string_view("name"))
            return line(std::format("Set the name to {0}", newText));
        if (key == std::string_view("trigger_type"))
            return line(std::format("Set the trigger type to {0}", newText));
        if (key == std::string_view("event_type"))
            return line(std::format("Set the event type to {0}", newText));
        if (key == std::string_view("actions"))
            return line(std::format("Set actions to {0}", newText));
        if (key == std::string_view("enabled"))
            return line(truthy((hasNew ? newValue : oldValue)) ? std::string("Enabled rule") : std::string("Disabled rule"));
        if (key == std::string_view("exempt_roles"))
            return line(std::format("Set exempt roles to {0}", newText));
        if (key == std::string_view("exempt_channels"))
            return line(std::format("Set exempt channels to {0}", newText));
        if (key == std::string_view("trigger_metadata"))
            return line(std::format("Set trigger metadata {0}", newText));
        if (key == std::string_view("$add_keyword_filter"))
            return line(std::format("added keywords {0}", newText));
        if (key == std::string_view("$remove_keyword_filter"))
            return line(std::format("removed keywords {0}", newText));
        if (key == std::string_view("$add_regex_patterns"))
            return line(std::format("added regex patterns {0}", newText));
        if (key == std::string_view("$remove_regex_patterns"))
            return line(std::format("removed regex patterns {0}", newText));
        if (key == std::string_view("$add_allow_list"))
            return line(std::format("added allow list keywords {0}", newText));
        if (key == std::string_view("$remove_allow_list"))
            return line(std::format("removed allow list keywords {0}", newText));
        return std::nullopt;

    case Target::Soundboard:
        if (key == std::string_view("name"))
            return nameLines("With the name {0}");
        if (key == std::string_view("volume"))
            return createdOrChanged(std::format("With the volume set to {0}", newText), std::format("Changed the volume from {0} to {1}", oldText, newText));
        if (key == std::string_view("emoji_name") || key == std::string_view("emoji_id"))
            return changedSetCleared(std::format("Changed the emoji from {0} to {1}", oldText, newText), std::format("With the emoji {0}", newText),
                                     std::format("Removed the emoji {0}", oldText));
        return std::nullopt;

    case Target::VoiceChannelStatus:
        if (key == std::string_view("status"))
            return line(std::format("Set to {0}", newText));
        return std::nullopt;

    case Target::MemberVerification:
        if (key == std::string_view("verification_enabled"))
            return line(truthy(newValue) ? std::string("Enabled join settings") : std::string("Disabled join settings"));
        if (key == std::string_view("manual_approval_enabled"))
            return line(truthy(newValue) ? std::string("Manual verification enabled") : std::string("Manual verification disabled"));
        return std::nullopt;

    case Target::GuildProfile:
        if (key == std::string_view("description"))
            return line(std::string("Updated server description"));
        if (key == std::string_view("brand_color_primary"))
            return line(std::string("Updated profile banner color"));
        if (key == std::string_view("custom_banner_hash"))
            return line(std::string("Updated profile banner"));
        if (key == std::string_view("traits"))
            return line(std::string("Updated profile traits"));
        if (key == std::string_view("game_application_ids"))
            return line(std::string("Updated profile games played"));
        if (key == std::string_view("visibility"))
            return line(std::string("Updated profile visibility"));
        if (key == std::string_view("server_tag"))
            return clearedOrSet(std::string("<b>Disabled</b> the server tag"), std::format("Updated server tag to {0}", newText));
        return std::nullopt;

    case Target::OnboardingPrompt:
        if (key == std::string_view("title"))
            return createdOrChanged(std::format("Set the title to {0}", newText), std::format("Changed the title from {0} to {1}", oldText, newText));
        if (key == std::string_view("description"))
            return createdOrChanged(std::format("Set the description to {0}", newText), std::format("Changed the description from {0} to {1}", oldText, newText));
        if (key == std::string_view("options"))
            return line(std::string("Changed answers for customization question"));
        if (key == std::string_view("single_select"))
            return onOff(std::string("Set the customization question to single select"), std::string("Set the customization question to multi select"));
        if (key == std::string_view("required"))
            return onOff(std::string("Set the customization question to required"), std::string("Set the customization question to optional"));
        return std::nullopt;

    case Target::Onboarding:
        if (key == std::string_view("default_channel_ids"))
            return line(countText(count, "Set 1 default channel",
                                  "Set {0} default channels"));
        if (key == std::string_view("enable_default_channels"))
            return onOff(std::string("Set default channels to enabled"), std::string("Set default channels to disabled"));
        if (key == std::string_view("enable_onboarding_prompts"))
            return onOff(std::string("Set customization questions to enabled"), std::string("Set customization questions to disabled"));
        if (key == std::string_view("enabled"))
            return onOff(std::string("Enabled Onboarding"), std::string("Disabled Onboarding"));
        return std::nullopt;

    case Target::HomeSettings:
        if (key == std::string_view("welcome_message"))
            return line(std::string("Changed the welcome message"));
        if (key == std::string_view("new_member_actions"))
            return line(std::string("Changed the new member To Dos"));
        if (key == std::string_view("resource_channels"))
            return line(std::string("Changed resource channels"));
        return std::nullopt;

    case Target::GuildHome:
    case Target::Unknown:
        break;
    }
    return std::nullopt;
}

AuditLogFormatter::Row AuditLogFormatter::format(const std::vector<Entry> &merged)
{
    const Entry &first = merged.front();
    const nlohmann::json &options = first.raw.options;

    std::string count = text(jsonMember(options, "count"));
    const std::string removed = text(jsonMember(options, "members_removed"));
    if (!removed.empty() && removed != std::string_view("0"))
        count = removed;

    const Core::Snowflake channelId = snowflakeOf(jsonMember(options, "channel_id"));
    const std::string channel = channelId.isValid() ? channelName(channelId) : std::string();

    std::string subtarget;
    const Core::Snowflake exceptionId = snowflakeOf(jsonMember(options, "event_exception_id"));
    if (first.target == Target::ScheduledEventException && exceptionId.isValid())
        subtarget = exceptionId.toDateTime().format("%B %d, %Y");

    Row row;
    row.action = first.action;
    row.userId = first.userId;
    row.category = first.category;
    row.start = first.time;
    row.end = merged.back().time;
    row.titleHtml = titleFor(first, targetName(first), count, channel, subtarget);
    for (const Entry &entry : merged)
        for (const Change &change : transform(entry))
            if (const auto line = describe(entry, change))
                row.lines.push_back(*line);
    return row;
}

std::string AuditLogFormatter::timeText(const Platform::DateTime &start, const Platform::DateTime &end)
{
    const std::string startText = calendar(start);
    const std::string endText = calendar(end);
    return startText == endText ? startText : startText + std::string("—") + endText;
}

std::vector<std::pair<Discord::AuditLogAction, std::string>> AuditLogFormatter::actionFilters()
{
    return {
        { Action::GUILD_UPDATE, std::string("Update Server") },
        { Action::CHANNEL_CREATE, std::string("Create Channel") },
        { Action::CHANNEL_UPDATE, std::string("Update Channel") },
        { Action::CHANNEL_DELETE, std::string("Delete Channel") },
        { Action::CHANNEL_OVERWRITE_CREATE, std::string("Create Channel Permissions") },
        { Action::CHANNEL_OVERWRITE_UPDATE, std::string("Update Channel Permissions") },
        { Action::CHANNEL_OVERWRITE_DELETE, std::string("Delete Channel Permissions") },
        { Action::MEMBER_KICK, std::string("Kick Member") },
        { Action::MEMBER_PRUNE, std::string("Prune Members") },
        { Action::MEMBER_BAN_ADD, std::string("Ban Member") },
        { Action::MEMBER_BAN_REMOVE, std::string("Unban Member") },
        { Action::MEMBER_UPDATE, std::string("Update Member") },
        { Action::MEMBER_ROLE_UPDATE, std::string("Update Member Roles") },
        { Action::MEMBER_MOVE, std::string("Move Member") },
        { Action::MEMBER_DISCONNECT, std::string("Disconnect Member") },
        { Action::BOT_ADD, std::string("Add Bot") },
        { Action::THREAD_CREATE, std::string("Create Thread") },
        { Action::THREAD_UPDATE, std::string("Update Thread") },
        { Action::THREAD_DELETE, std::string("Delete Thread") },
        { Action::ROLE_CREATE, std::string("Create Role") },
        { Action::ROLE_UPDATE, std::string("Update Role") },
        { Action::ROLE_DELETE, std::string("Delete Role") },
        { Action::ONBOARDING_PROMPT_CREATE, std::string("Create Customization Question") },
        { Action::ONBOARDING_PROMPT_UPDATE, std::string("Update Customization Question") },
        { Action::ONBOARDING_PROMPT_DELETE, std::string("Delete Customization Question") },
        { Action::ONBOARDING_CREATE, std::string("Create Onboarding") },
        { Action::ONBOARDING_UPDATE, std::string("Update Onboarding") },
        { Action::HOME_SETTINGS_CREATE, std::string("Create Server Guide") },
        { Action::HOME_SETTINGS_UPDATE, std::string("Update Server Guide") },
        { Action::INVITE_CREATE, std::string("Create Invite") },
        { Action::INVITE_UPDATE, std::string("Update Invite") },
        { Action::INVITE_DELETE, std::string("Delete Invite") },
        { Action::WEBHOOK_CREATE, std::string("Create Webhook") },
        { Action::WEBHOOK_UPDATE, std::string("Update Webhook") },
        { Action::WEBHOOK_DELETE, std::string("Delete Webhook") },
        { Action::EMOJI_CREATE, std::string("Create Emoji") },
        { Action::EMOJI_UPDATE, std::string("Update Emoji") },
        { Action::EMOJI_DELETE, std::string("Delete Emoji") },
        { Action::MESSAGE_DELETE, std::string("Delete Messages") },
        { Action::MESSAGE_BULK_DELETE, std::string("Bulk Delete Messages") },
        { Action::MESSAGE_PIN, std::string("Pin Message") },
        { Action::MESSAGE_UNPIN, std::string("Unpin Message") },
        { Action::INTEGRATION_CREATE, std::string("Create Integration") },
        { Action::INTEGRATION_UPDATE, std::string("Update Integration") },
        { Action::INTEGRATION_DELETE, std::string("Delete Integration") },
        { Action::STICKER_CREATE, std::string("Create Sticker") },
        { Action::STICKER_UPDATE, std::string("Update Sticker") },
        { Action::STICKER_DELETE, std::string("Delete Sticker") },
        { Action::STAGE_INSTANCE_CREATE, std::string("Start Stage") },
        { Action::STAGE_INSTANCE_UPDATE, std::string("Update Stage") },
        { Action::STAGE_INSTANCE_DELETE, std::string("End Stage") },
        { Action::GUILD_SCHEDULED_EVENT_CREATE, std::string("Create Event") },
        { Action::GUILD_SCHEDULED_EVENT_UPDATE, std::string("Update Event") },
        { Action::GUILD_SCHEDULED_EVENT_DELETE, std::string("Cancel Event") },
        { Action::APPLICATION_COMMAND_PERMISSION_UPDATE, std::string("Update Command Permissions") },
        { Action::AUTO_MODERATION_BLOCK_MESSAGE, std::string("AutoMod Block Message") },
        { Action::AUTO_MODERATION_RULE_CREATE, std::string("Create AutoMod Rule") },
        { Action::AUTO_MODERATION_RULE_UPDATE, std::string("Update AutoMod Rule") },
        { Action::AUTO_MODERATION_RULE_DELETE, std::string("Delete AutoMod Rule") },
        { Action::GUILD_HOME_FEATURE_ITEM, std::string("Feature Item on Home") },
        { Action::GUILD_HOME_REMOVE_ITEM, std::string("Remove Item from Home") },
        { Action::SOUNDBOARD_SOUND_CREATE, std::string("Create Soundboard Sound") },
        { Action::SOUNDBOARD_SOUND_UPDATE, std::string("Update Soundboard Sound") },
        { Action::SOUNDBOARD_SOUND_DELETE, std::string("Delete Soundboard Sound") },
        { Action::VOICE_CHANNEL_STATUS_CREATE, std::string("Create Voice Channel Status") },
        { Action::VOICE_CHANNEL_STATUS_DELETE, std::string("Delete Voice Channel Status") },
    };
}

} // namespace UI
} // namespace Styx
