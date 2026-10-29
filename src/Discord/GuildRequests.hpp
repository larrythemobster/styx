#pragma once

#include "Platform/Native.hpp"
#include "Platform/Utf8.hpp"
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "Entities.hpp"

namespace Styx {
namespace Discord {

struct GuildEdit : Core::JsonUtils::JsonObject
{
    Field<std::string, true, true> splash;
    Field<std::string, true, true> banner;
    Field<Core::Snowflake, true, true> afkChannelId;
    Field<int, true> afkTimeout;
    Field<Core::Snowflake, true, true> systemChannelId;
    Field<MessageNotificationLevel, true> defaultMessageNotifications;
    Field<SystemChannelFlags, true> systemChannelFlags;
    Field<bool, true> premiumProgressBarEnabled;

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "splash", splash);
        insert(obj, "banner", banner);
        insert(obj, "afk_channel_id", afkChannelId);
        insert(obj, "afk_timeout", afkTimeout);
        insert(obj, "system_channel_id", systemChannelId);
        insert(obj, "default_message_notifications", defaultMessageNotifications);
        insert(obj, "system_channel_flags", systemChannelFlags);
        insert(obj, "premium_progress_bar_enabled", premiumProgressBarEnabled);
        return obj;
    }
};

struct GuildProfileEdit
{
    struct Trait
    {
        std::string label;
        nlohmann::json emojiId;
        nlohmann::json emojiName;
        nlohmann::json emojiAnimated;
    };

    static constexpr int TraitSlots = 5;

    std::string name;
    std::string description;
    nlohmann::json icon;
    nlohmann::json customBanner;
    nlohmann::json visibility;
    nlohmann::json brandColorPrimary;
    std::vector<Trait> traits;
    nlohmann::json gameApplicationIds;

    static GuildProfileEdit fromProfile(const nlohmann::json &profile)
    {
        GuildProfileEdit edit;
        if (const auto it = profile.find("name"); it != profile.end() && it->is_string())
            edit.name = it->get<std::string>();
        if (const auto it = profile.find("description"); it != profile.end() && it->is_string())
            edit.description = it->get<std::string>();
        edit.icon = profile.value("icon_hash", nlohmann::json());
        edit.customBanner = profile.value("custom_banner_hash", nlohmann::json());
        edit.visibility = profile.value("visibility", nlohmann::json());
        const nlohmann::json brandColor = profile.value("brand_color_primary", nlohmann::json());
        edit.brandColorPrimary = brandColor.is_string() && !brandColor.get_ref<const std::string &>().empty() ? brandColor : nlohmann::json(nullptr);

        edit.traits.assign(TraitSlots, Trait{});
        if (const auto traits = profile.find("traits"); traits != profile.end() && traits->is_array()) {
            for (const nlohmann::json &trait : *traits) {
                const int position = trait.value("position", -1);
                if (position < 0 || position >= TraitSlots)
                    continue;
                edit.traits[position] = { trait.value("label", std::string()),
                                          trait.value("emoji_id", nlohmann::json()),
                                          trait.value("emoji_name", nlohmann::json()),
                                          trait.value("emoji_animated", nlohmann::json()) };
            }
        }
        edit.gameApplicationIds = profile.value("game_application_ids", nlohmann::json());
        return edit;
    }

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        obj.insert("name", name);
        obj.insert("description", description);
        obj.insert("icon", icon);
        obj.insert("custom_banner", customBanner);
        if (!visibility.is_null())
            obj.insert("visibility", visibility);
        obj.insert("brand_color_primary", brandColorPrimary);

        Core::OrderedJson::Array traitArray;
        for (std::size_t i = 0; i < traits.size(); i++) {
            const Trait &trait = traits[i];
            if (trait.label.empty())
                continue;
            Core::OrderedJson entry;
            auto insertIfSet = [&entry](const std::string &key, const nlohmann::json &value) {
                if (!value.is_null())
                    entry.insert(key, value);
            };
            entry.insert("label", trait.label);
            entry.insert("position", i);
            insertIfSet("emoji_id", trait.emojiId);
            insertIfSet("emoji_name", trait.emojiName);
            insertIfSet("emoji_animated", trait.emojiAnimated);
            traitArray.append(entry);
        }
        obj.insert("traits", traitArray);
        obj.insert("game_application_ids", gameApplicationIds);
        return obj;
    }
};

struct RoleEdit : Core::JsonUtils::JsonObject
{
    Field<std::string> name;
    Field<Permissions> permissions;
    Field<int> color;
    Field<RoleColors, true, true> colors;
    Field<bool> hoist;
    Field<bool> mentionable;
    Field<std::string, true, true> icon;
    Field<std::string, false, true> unicodeEmoji;

    static RoleEdit fromRole(const Role &role)
    {
        RoleEdit edit;
        edit.name = role.name.get();
        edit.permissions = role.permissions.get();
        edit.color = role.color.valueOr(0);
        if (role.colors.hasValue())
            edit.colors = role.colors.get();
        edit.hoist = role.hoist.valueOr(false);
        edit.mentionable = role.mentionable.valueOr(false);
        if (role.unicodeEmoji.hasValue())
            edit.unicodeEmoji = role.unicodeEmoji.get();
        else
            edit.unicodeEmoji = nullptr;
        return edit;
    }

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson obj;
        insert(obj, "name", name);
        obj.insert("permissions", std::to_string(static_cast<std::uint64_t>(permissions.get())));
        insert(obj, "color", color);
        if (colors.hasValue())
            insert(obj, "colors", colors);
        insert(obj, "hoist", hoist);
        insert(obj, "mentionable", mentionable);
        insert(obj, "icon", icon);
        insert(obj, "unicode_emoji", unicodeEmoji);
        return obj;
    }
};

struct MemberSearchQuery
{
    enum class Sort {
        JoinedNewest = 1,
        JoinedOldest = 2,
        UserIdDescending = 3,
        UserIdAscending = 4,
    };

    struct Cursor
    {
        std::int64_t guildJoinedAtMs = 0;
        Core::Snowflake userId;
    };

    static constexpr int MinimumTextLength = 2;

    std::string text;
    bool onlyTimedOut = false;
    std::optional<Sort> sort;
    int limit = 250;
    std::optional<Cursor> after;

    [[nodiscard]] bool searchesText() const { return text.size() >= MinimumTextLength; }

    Core::OrderedJson toJson() const
    {
        Core::OrderedJson andQuery;
        if (searchesText()) {
            std::vector<std::string> usernames;
            std::vector<std::string> userIds;
            static const std::regex snowflakePattern("^[0-9]{17,19}$");
            for (std::string_view raw : Platform::split(text, ',')) {
                const std::string_view term = Platform::trim(raw);
                if (term.empty())
                    continue;
                (std::regex_match(term.begin(), term.end(), snowflakePattern) ? userIds : usernames).emplace_back(term);
            }
            if (!usernames.empty())
                andQuery.insert("usernames", Core::OrderedJson().insert("or_query", nlohmann::json(usernames)));
            if (!userIds.empty())
                andQuery.insert("user_id", Core::OrderedJson().insert("or_query", nlohmann::json(userIds)));
        }

        Core::OrderedJson orQuery;
        if (onlyTimedOut) {
            Core::OrderedJson range;
            range.insert("gte", Platform::DateTime::now().unixMillis());
            Core::OrderedJson safetySignals;
            safetySignals.insert("communication_disabled_until", Core::OrderedJson().insert("range", range));
            orQuery.insert("safety_signals", safetySignals);
        }

        Core::OrderedJson obj;
        obj.insert("or_query", orQuery);
        obj.insert("and_query", andQuery);
        if (sort)
            obj.insert("sort", static_cast<int>(*sort));
        obj.insert("limit", limit);
        if (after) {
            Core::OrderedJson cursor;
            cursor.insert("guild_joined_at", after->guildJoinedAtMs);
            cursor.insert("user_id", std::to_string(std::uint64_t(after->userId)));
            obj.insert("after", cursor);
        }
        return obj;
    }
};

} // namespace Discord
} // namespace Styx
