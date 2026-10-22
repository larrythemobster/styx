#include "GuildRepository.hpp"

#include "DatabaseManager.hpp"
#include "Core/Logging.hpp"

namespace Styx {
namespace Storage {

namespace {

template <typename F>
void bindSnowflake(Platform::Statement &q, std::string_view param, const F &field)
{
    if (field.hasValue())
        q.bind(param, static_cast<int64_t>(field.get()));
    else
        q.bindNull(param);
}

template <typename F>
void bindEnum(Platform::Statement &q, std::string_view param, const F &field)
{
    if (field.hasValue())
        q.bind(param, static_cast<int>(field.get()));
    else
        q.bindNull(param);
}

} // namespace

GuildRepository::GuildRepository(Core::Snowflake accountId)
    : BaseRepository(DatabaseManager::getCacheConnectionName(accountId))
{
}

void GuildRepository::saveGuild(const Discord::Guild &guild)
{
    auto &db = getDb();
    saveGuild(guild, db);
}

void GuildRepository::saveGuild(const Discord::Guild &guild, Platform::Database &db)
{
    try {
        auto q = db.prepare(R"(
            INSERT OR REPLACE INTO guilds
            (id, name, icon, owner_id, features, premium_tier, premium_subscription_count,
             rules_channel_id, default_message_notifications, description, banner, splash,
             afk_channel_id, afk_timeout, system_channel_id, system_channel_flags,
             verification_level, explicit_content_filter, mfa_level, premium_progress_bar_enabled,
             additional_emoji_slots, additional_sticker_slots, invites_disabled_until, dms_disabled_until,
             lockdown_duration_hours)
            VALUES
            (:id, :name, :icon, :owner_id, :features, :premium_tier, :premium_subscription_count,
             :rules_channel_id, :default_message_notifications, :description, :banner, :splash,
             :afk_channel_id, :afk_timeout, :system_channel_id, :system_channel_flags,
             :verification_level, :explicit_content_filter, :mfa_level, :premium_progress_bar_enabled,
             :additional_emoji_slots, :additional_sticker_slots, :invites_disabled_until, :dms_disabled_until,
             :lockdown_duration_hours)
        )");

        q.bind(":id", static_cast<int64_t>(guild.id.get()));
        std::string name = guild.name.get().toStdString();
        q.bind(":name", std::string_view(name));
        bindOptional(q, ":icon", guild.icon);
        q.bind(":owner_id", static_cast<int64_t>(guild.ownerId.get()));

        if (guild.features.hasValue()) {
            std::string feat = QStringList(guild.features.get()).join(',').toStdString();
            q.bind(":features", std::string_view(feat));
        } else {
            q.bindNull(":features");
        }

        bindEnum(q, ":premium_tier", guild.premiumTier);
        bindOptional(q, ":premium_subscription_count", guild.premiumSubscriptionCount);
        bindSnowflake(q, ":rules_channel_id", guild.rulesChannelId);
        bindEnum(q, ":default_message_notifications", guild.defaultMessageNotifications);
        bindOptional(q, ":description", guild.description);
        bindOptional(q, ":banner", guild.banner);
        bindOptional(q, ":splash", guild.splash);

        const Discord::GuildIncidentsData incidents = guild.incidentsData.valueOr({});
        if (incidents.invitesDisabledUntil.hasValue()) {
            std::string inv = incidents.invitesDisabledUntil->toString(Qt::ISODateWithMs).toStdString();
            q.bind(":invites_disabled_until", std::string_view(inv));
        } else {
            q.bindNull(":invites_disabled_until");
        }

        if (incidents.dmsDisabledUntil.hasValue()) {
            std::string dms = incidents.dmsDisabledUntil->toString(Qt::ISODateWithMs).toStdString();
            q.bind(":dms_disabled_until", std::string_view(dms));
        } else {
            q.bindNull(":dms_disabled_until");
        }

        if (incidents.lockdownDurationHours.hasValue()) {
            q.bind(":lockdown_duration_hours", incidents.lockdownDurationHours.get());
        } else {
            q.bindNull(":lockdown_duration_hours");
        }

        bindSnowflake(q, ":afk_channel_id", guild.afkChannelId);
        bindOptional(q, ":afk_timeout", guild.afkTimeout);
        bindSnowflake(q, ":system_channel_id", guild.systemChannelId);

        if (guild.systemChannelFlags.hasValue())
            q.bind(":system_channel_flags", static_cast<int>(guild.systemChannelFlags->toInt()));
        else
            q.bindNull(":system_channel_flags");

        bindEnum(q, ":verification_level", guild.verificationLevel);
        bindEnum(q, ":explicit_content_filter", guild.explicitContentFilter);
        bindEnum(q, ":mfa_level", guild.mfaLevel);
        bindOptional(q, ":premium_progress_bar_enabled", guild.premiumProgressBarEnabled);

        const bool hasPremiumFeatures = guild.premiumFeatures.hasValue();
        if (hasPremiumFeatures && guild.premiumFeatures->additionalEmojiSlots.hasValue())
            q.bind(":additional_emoji_slots", guild.premiumFeatures->additionalEmojiSlots.get());
        else
            q.bindNull(":additional_emoji_slots");

        if (hasPremiumFeatures && guild.premiumFeatures->additionalStickerSlots.hasValue())
            q.bind(":additional_sticker_slots", guild.premiumFeatures->additionalStickerSlots.get());
        else
            q.bindNull(":additional_sticker_slots");

        execLogged(q, "GuildRepository: Save");
    } catch (const Platform::SqliteError &e) {
        qCWarning(LogDB) << "GuildRepository: Save failed:" << e.what();
    }
}

void GuildRepository::deleteGuild(Core::Snowflake guildId)
{
    auto &db = getDb();
    deleteGuild(guildId, db);
}

void GuildRepository::deleteGuild(Core::Snowflake guildId, Platform::Database &db)
{
    try {
        auto q = db.prepare("DELETE FROM guilds WHERE id = :id");
        q.bind(":id", static_cast<int64_t>(guildId));
        execLogged(q, "GuildRepository: Delete");
    } catch (const Platform::SqliteError &e) {
        qCWarning(LogDB) << "GuildRepository: Delete failed:" << e.what();
    }
}

std::optional<Discord::Guild> GuildRepository::getGuild(Core::Snowflake guildId)
{
    auto &db = getDb();
    try {
        auto q = db.prepare(R"(
            SELECT id, name, icon, owner_id, features, premium_tier, premium_subscription_count,
                   rules_channel_id, default_message_notifications, description, banner, afk_channel_id,
                   afk_timeout, system_channel_id, system_channel_flags, verification_level,
                   explicit_content_filter, mfa_level, premium_progress_bar_enabled, additional_emoji_slots,
                   additional_sticker_slots, splash, invites_disabled_until, dms_disabled_until,
                   lockdown_duration_hours
            FROM guilds WHERE id = :id
        )");
        q.bind(":id", static_cast<int64_t>(guildId));

        if (!q.step())
            return std::nullopt;

        Discord::Guild guild;
        guild.id = static_cast<Core::Snowflake>(q.getInt64(0));
        guild.name = QString::fromStdString(q.getText(1));
        if (!q.isNull(2))
            guild.icon = QString::fromStdString(q.getText(2));
        guild.ownerId = static_cast<Core::Snowflake>(q.getInt64(3));

        if (!q.isNull(4))
            guild.features = QString::fromStdString(q.getText(4)).split(',', Qt::SkipEmptyParts);
        if (!q.isNull(5))
            guild.premiumTier = static_cast<Discord::PremiumTier>(q.getInt(5));
        if (!q.isNull(6))
            guild.premiumSubscriptionCount = q.getInt(6);
        if (q.isNull(7))
            guild.rulesChannelId = nullptr;
        else
            guild.rulesChannelId = static_cast<Core::Snowflake>(q.getInt64(7));

        if (!q.isNull(8))
            guild.defaultMessageNotifications = static_cast<Discord::MessageNotificationLevel>(q.getInt(8));
        if (!q.isNull(9))
            guild.description = QString::fromStdString(q.getText(9));
        if (!q.isNull(10))
            guild.banner = QString::fromStdString(q.getText(10));
        if (!q.isNull(11))
            guild.afkChannelId = static_cast<Core::Snowflake>(q.getInt64(11));
        if (!q.isNull(12))
            guild.afkTimeout = q.getInt(12);
        if (!q.isNull(13))
            guild.systemChannelId = static_cast<Core::Snowflake>(q.getInt64(13));
        if (!q.isNull(14))
            guild.systemChannelFlags = Discord::SystemChannelFlags::fromInt(q.getInt(14));
        if (!q.isNull(15))
            guild.verificationLevel = static_cast<Discord::VerificationLevel>(q.getInt(15));
        if (!q.isNull(16))
            guild.explicitContentFilter = static_cast<Discord::ExplicitContentFilter>(q.getInt(16));
        if (!q.isNull(17))
            guild.mfaLevel = static_cast<Discord::MfaLevel>(q.getInt(17));
        if (!q.isNull(18))
            guild.premiumProgressBarEnabled = q.getInt(18) != 0;

        if (!q.isNull(19) || !q.isNull(20)) {
            Discord::GuildPremiumFeatures premiumFeatures;
            if (!q.isNull(19))
                premiumFeatures.additionalEmojiSlots = q.getInt(19);
            if (!q.isNull(20))
                premiumFeatures.additionalStickerSlots = q.getInt(20);
            guild.premiumFeatures = premiumFeatures;
        }

        if (!q.isNull(21))
            guild.splash = QString::fromStdString(q.getText(21));

        if (!q.isNull(22) || !q.isNull(23) || !q.isNull(24)) {
            Discord::GuildIncidentsData incidents;
            if (!q.isNull(22))
                incidents.invitesDisabledUntil = QDateTime::fromString(QString::fromStdString(q.getText(22)), Qt::ISODateWithMs);
            if (!q.isNull(23))
                incidents.dmsDisabledUntil = QDateTime::fromString(QString::fromStdString(q.getText(23)), Qt::ISODateWithMs);
            if (!q.isNull(24))
                incidents.lockdownDurationHours = q.getInt(24);
            guild.incidentsData = incidents;
        }

        return guild;
    } catch (const Platform::SqliteError &e) {
        qCWarning(LogDB) << "GuildRepository: getGuild failed:" << e.what();
        return std::nullopt;
    }
}

} // namespace Storage
} // namespace Styx
