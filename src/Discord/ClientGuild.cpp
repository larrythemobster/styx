#include "Client.hpp"
#include "InviteLink.hpp"
#include "ApiError.hpp"
#include "Core/Logging.hpp"
#include "Platform/Utf8.hpp"
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace Styx::Discord {
namespace {
using Json = nlohmann::json;
std::string guildEndpoint(Snowflake id, const std::string &path = {}) { return "/guilds/" + id.toString() + path; }
std::string memberEndpoint(Snowflake guild, Snowflake user) { return guildEndpoint(guild, "/members/" + user.toString()); }
std::string roleEndpoint(Snowflake guild, Snowflake role, const std::string &path = {}) { return guildEndpoint(guild, "/roles/" + role.toString() + path); }
Json snowflakeArray(const std::vector<Snowflake> &ids) {
    Json result = Json::array();
    for (auto id : ids) result.push_back(id.toString());
    return result;
}
Json document(const HttpResponse &response) {
    auto value = Json::parse(response.body, nullptr, false);
    return value.is_discarded() ? Json() : value;
}
HttpCallback actionHandler(const char *what, Client::ActionCallback callback) {
    return [what, callback = std::move(callback)](const HttpResponse &response) {
        if (!response.success) {
            const auto error = ApiError::fromResponse(response);
            Core::log(Core::LogLevel::Warning, LogDiscord) << what << " failed with status " << response.statusCode << ": " << error.message;
            if (callback) callback(Core::Result<void>::makeError(error.message, error.code));
        } else if (callback) callback(Core::Result<void>::makeOk());
    };
}
template<class T, class Parse>
HttpCallback valueHandler(const char *what, Client::ResultCallback<T> callback, Parse parse) {
    return [what, callback = std::move(callback), parse = std::move(parse)](const HttpResponse &response) {
        if (!response.success) {
            const auto error = ApiError::fromResponse(response);
            Core::log(Core::LogLevel::Warning, LogDiscord) << what << " failed with status " << response.statusCode << ": " << error.message;
            callback(Core::Result<T>::makeError(error.message, error.code));
        } else callback(Core::Result<T>::makeOk(parse(document(response))));
    };
}
template<class T> std::vector<T> parseArray(const Json &doc) {
    std::vector<T> values;
    if (doc.is_array()) for (const auto &value : doc) values.push_back(T::fromJson(value));
    return values;
}
Snowflake snowflake(const std::string &text) {
    try { return Snowflake(std::stoull(text)); } catch (...) { return Snowflake::Invalid; }
}
Json timestamp(const Platform::DateTime &time) { return time.valid() ? Json(time.iso8601()) : Json(); }
}

void Client::deleteGuild(Snowflake guildId, ActionCallback callback) {
    httpClient->send(HttpClient::Method::POST, guildEndpoint(guildId, "/delete"), {}, {}, actionHandler("Deleting a guild", std::move(callback)));
}
void Client::createRole(Snowflake guildId, ResultCallback<Role> callback) {
    Core::OrderedJson colors; colors.insert("primary_color", 0).insert("secondary_color", nullptr).insert("tertiary_color", nullptr);
    Core::OrderedJson body; body.insert("name", "new role").insert("color", 0).insert("colors", colors).insert("permissions", "0");
    httpClient->post(guildEndpoint(guildId, "/roles"), body, applyThen([this, guildId](const Json &doc) {
        GuildRoleCreate event; event.guildId = guildId; event.role = Role::fromJson(doc); onGatewayGuildRoleCreate(event);
    }, valueHandler<Role>("Creating a role", std::move(callback), [](const Json &doc) { return Role::fromJson(doc); })));
}
void Client::modifyRole(Snowflake guildId, Snowflake roleId, const RoleEdit &edit, ActionCallback callback) {
    httpClient->patch(roleEndpoint(guildId, roleId), edit.toJson(), applyThen([this, guildId](const Json &doc) {
        GuildRoleUpdate event; event.guildId = guildId; event.role = Role::fromJson(doc); onGatewayGuildRoleUpdate(event);
    }, actionHandler("Saving a role", std::move(callback))));
}
void Client::modifyRolePositions(Snowflake guildId, const std::vector<std::pair<Snowflake, int>> &positions, ActionCallback callback) {
    Core::OrderedJson::Array body; std::unordered_set<Snowflake> moved;
    for (const auto &[id, position] : positions) { Core::OrderedJson entry; entry.insert("id", id.toString()).insert("position", position); body.append(entry); moved.insert(id); }
    httpClient->patch(guildEndpoint(guildId, "/roles"), body, applyThen([this, guildId, moved = std::move(moved)](const Json &doc) {
        for (const auto &role : parseArray<Role>(doc)) if (moved.contains(role.id.get())) { GuildRoleUpdate event; event.guildId = guildId; event.role = role; onGatewayGuildRoleUpdate(event); }
    }, actionHandler("Reordering roles", std::move(callback))));
}
void Client::deleteRole(Snowflake guildId, Snowflake roleId, ActionCallback callback) {
    httpClient->delete_(roleEndpoint(guildId, roleId), applyThen([this, guildId, roleId](const Json &) {
        GuildRoleDelete event; event.guildId = guildId; event.roleId = roleId; onGatewayGuildRoleDelete(event);
    }, actionHandler("Deleting a role", std::move(callback))));
}
void Client::fetchRoleMemberCounts(Snowflake guildId, ResultCallback<std::unordered_map<Snowflake, int>> callback) {
    httpClient->get(guildEndpoint(guildId, "/roles/member-counts"), {}, valueHandler<std::unordered_map<Snowflake, int>>("Fetching role member counts", std::move(callback), [](const Json &doc) {
        std::unordered_map<Snowflake, int> counts; if (doc.is_object()) for (auto it = doc.begin(); it != doc.end(); ++it) counts[snowflake(it.key())] = it.value().is_number_integer() ? it.value().get<int>() : 0; return counts;
    }));
}
void Client::fetchRoleMemberIds(Snowflake guildId, Snowflake roleId, ResultCallback<std::vector<Snowflake>> callback) {
    httpClient->get(roleEndpoint(guildId, roleId, "/member-ids"), {}, valueHandler<std::vector<Snowflake>>("Fetching role members", std::move(callback), [](const Json &doc) {
        std::vector<Snowflake> ids; if (doc.is_array()) for (const auto &v : doc) ids.push_back(snowflake(v.is_string() ? v.get<std::string>() : std::string())); return ids;
    }));
}
void Client::addRoleMembers(Snowflake guildId, Snowflake roleId, const std::vector<Snowflake> &userIds, ResultCallback<std::vector<Snowflake>> callback) {
    Core::OrderedJson body; body.insert("member_ids", snowflakeArray(userIds));
    Platform::Guard<Client> self(this);
    auto counted = [self, guildId, roleId, callback = std::move(callback)](const Core::Result<std::vector<Snowflake>> &result) {
        if (self && result.success()) self->roleMemberCountChanged.publish(guildId, roleId, static_cast<int>(result.value->size())); callback(result);
    };
    httpClient->patch(roleEndpoint(guildId, roleId, "/members"), body, applyThen([this, guildId](const Json &doc) {
        if (doc.is_object()) for (auto it = doc.begin(); it != doc.end(); ++it) { GuildMemberUpdate event; event.guildId = guildId; event.member = Member::fromJson(it.value()); guildMemberUpdated.publish(event); }
    }, valueHandler<std::vector<Snowflake>>("Adding role members", std::move(counted), [](const Json &doc) {
        std::vector<Snowflake> ids; if (doc.is_object()) for (auto it = doc.begin(); it != doc.end(); ++it) ids.push_back(snowflake(it.key())); return ids;
    })));
}
void Client::searchGuildMembers(Snowflake guildId, const MemberSearchQuery &query, ResultCallback<MemberSearchPage> callback) {
    httpClient->post(guildEndpoint(guildId, "/members-search"), query.toJson(), [callback = std::move(callback)](const HttpResponse &response) {
        if (!response.success) { const auto error = ApiError::fromResponse(response); Core::log(Core::LogLevel::Warning, LogDiscord) << "Member search failed with status " << response.statusCode << ": " << error.message; callback(Core::Result<MemberSearchPage>::makeError(error.message, error.code)); return; }
        const auto doc = document(response); MemberSearchPage page;
        if (response.statusCode == 202) { double retry = 1.0; if (doc.is_object() && doc.contains("retry_after") && doc["retry_after"].is_number()) retry = doc["retry_after"].get<double>(); page.indexingRetryAfterSeconds = std::max(1, static_cast<int>(std::round(retry))); }
        else if (doc.is_object()) { if (doc.contains("members") && doc["members"].is_array()) for (const auto &v : doc["members"]) page.members.push_back(MemberSearchResult::fromJson(v)); if (doc.contains("total_result_count") && doc["total_result_count"].is_number_integer()) page.totalResultCount = doc["total_result_count"].get<int>(); }
        callback(Core::Result<MemberSearchPage>::makeOk(page));
    });
}
HttpCallback Client::memberUpdateHandler(Snowflake guildId, const char *what, ActionCallback callback) {
    return applyThen([this, guildId](const Json &doc) { GuildMemberUpdate event; event.guildId = guildId; event.member = Member::fromJson(doc); guildMemberUpdated.publish(event); }, actionHandler(what, std::move(callback)));
}
HttpCallback Client::memberRemovalHandler(Snowflake guildId, Snowflake userId, const char *what, ActionCallback callback) {
    return applyThen([this, guildId, userId](const Json &) { User user; user.id = userId; GuildMemberRemove event; event.guildId = guildId; event.user = user; guildMemberRemoved.publish(event); }, actionHandler(what, std::move(callback)));
}
void Client::setMemberRoles(Snowflake guildId, Snowflake userId, const std::vector<Snowflake> &roleIds, const std::vector<Snowflake> &added, const std::vector<Snowflake> &removed, ActionCallback callback) {
    Core::OrderedJson body; body.insert("roles", snowflakeArray(roleIds)); Platform::Guard<Client> self(this);
    auto counted = [self, guildId, added, removed, callback = std::move(callback)](const Core::Result<void> &result) {
        if (self && result.success()) { for (auto id : added) self->roleMemberCountChanged.publish(guildId, id, 1); for (auto id : removed) self->roleMemberCountChanged.publish(guildId, id, -1); } if (callback) callback(result);
    };
    httpClient->patch(memberEndpoint(guildId, userId), body, memberUpdateHandler(guildId, "Updating member roles", std::move(counted)));
}
void Client::setMemberNickname(Snowflake guildId, Snowflake userId, const std::string &nick, ActionCallback callback) {
    Core::OrderedJson body; body.insert("nick", nick); httpClient->patch(memberEndpoint(guildId, userId), body, memberUpdateHandler(guildId, "Changing a nickname", std::move(callback)));
}
void Client::setMemberTimeout(Snowflake guildId, Snowflake userId, const Platform::DateTime &until, const std::optional<std::string> &reason, ActionCallback callback) {
    Core::OrderedJson body; body.insert("communication_disabled_until", timestamp(until)); RequestOptions options; options.auditLogReason = reason;
    httpClient->send(HttpClient::Method::PATCH, memberEndpoint(guildId, userId), body.toBytes(), options, memberUpdateHandler(guildId, "Timing out a member", std::move(callback)));
}
void Client::kickMember(Snowflake guildId, Snowflake userId, const std::string &reason, ActionCallback callback) {
    httpClient->delete_(memberEndpoint(guildId, userId) + "?reason=" + encodeUriComponent(reason), memberRemovalHandler(guildId, userId, "Kicking a member", std::move(callback)));
}
void Client::banMember(Snowflake guildId, Snowflake userId, int deleteMessageSeconds, const std::string &reason, ActionCallback callback) {
    Core::OrderedJson body; body.insert("delete_message_seconds", deleteMessageSeconds); RequestOptions options; options.auditLogReason = reason;
    httpClient->send(HttpClient::Method::PUT, guildEndpoint(guildId, "/bans/" + userId.toString()), body.toBytes(), options, memberRemovalHandler(guildId, userId, "Banning a member", std::move(callback)));
}
void Client::unbanMember(Snowflake guildId, Snowflake userId, ActionCallback callback) { httpClient->delete_(guildEndpoint(guildId, "/bans/" + userId.toString()), actionHandler("Revoking a ban", std::move(callback))); }
void Client::requestPruneCount(Snowflake guildId, int days, const std::vector<Snowflake> &includeRoles, ActionCallback callback) {
    std::vector<std::pair<std::string, std::string>> query{{"days", std::to_string(days)}}; for (auto id : includeRoles) query.emplace_back("include_roles", id.toString());
    httpClient->get(guildEndpoint(guildId, "/prune/v2"), query, actionHandler("Estimating a prune", std::move(callback)));
}
void Client::pruneMembers(Snowflake guildId, int days, const std::vector<Snowflake> &includeRoles, ActionCallback callback) {
    Core::OrderedJson body; body.insert("days", days).insert("compute_prune_count", false).insert("include_roles", snowflakeArray(includeRoles)); httpClient->post(guildEndpoint(guildId, "/prune"), body, actionHandler("Pruning members", std::move(callback)));
}
void Client::setMemberUpdatesSubscription(Snowflake guildId, bool subscribed) { gateway->setMemberUpdatesSubscription(guildId, subscribed); }
void Client::fetchBans(Snowflake guildId, Snowflake after, ResultCallback<std::vector<Ban>> callback) {
    std::vector<std::pair<std::string, std::string>> query{{"limit", "1000"}}; if (after.isValid()) query.emplace_back("after", after.toString()); httpClient->get(guildEndpoint(guildId, "/bans"), query, valueHandler<std::vector<Ban>>("Fetching bans", std::move(callback), parseArray<Ban>));
}
void Client::searchBans(Snowflake guildId, const std::string &text, const std::vector<Snowflake> &userIds, ResultCallback<std::vector<Ban>> callback) {
    std::vector<std::pair<std::string, std::string>> query{{"limit", "10"}}; for (auto id : userIds) query.emplace_back("user_ids", id.toString()); if (!Platform::trim(text).empty()) query.emplace_back("query", text); httpClient->get(guildEndpoint(guildId, "/bans/search"), query, valueHandler<std::vector<Ban>>("Searching bans", std::move(callback), parseArray<Ban>));
}
void Client::fetchGuildInvites(Snowflake guildId, ResultCallback<std::vector<Invite>> callback) { httpClient->get(guildEndpoint(guildId, "/invites"), {}, valueHandler<std::vector<Invite>>("Fetching invites", std::move(callback), parseArray<Invite>)); }
void Client::createInvite(Snowflake channelId, int maxAgeSeconds, int maxUses, bool temporary, ResultCallback<Invite> callback) {
    Core::OrderedJson body; body.insert("max_age", maxAgeSeconds).insert("max_uses", maxUses).insert("temporary", temporary).insert("flags", 0);
    RequestOptions options; options.context = ContextProperties::location("Settings Invite"); Platform::Guard<Client> self(this);
    auto announced = [self, callback = std::move(callback)](const Core::Result<Invite> &result) { if (self && result.success()) self->inviteCreated.publish(*result.value); if (callback) callback(result); };
    httpClient->send(HttpClient::Method::POST, "/channels/" + channelId.toString() + "/invites", body.toBytes(), options, valueHandler<Invite>("Creating an invite", std::move(announced), [](const Json &doc) { return Invite::fromJson(doc); }));
}
void Client::revokeInvite(const std::string &code, ActionCallback callback) {
    Platform::Guard<Client> self(this); auto announced = [self, code, callback = std::move(callback)](const Core::Result<void> &result) { if (self && (result.success() || result.code == ApiError::UnknownInvite)) self->inviteRevoked.publish(code); if (callback) callback(result); };
    httpClient->delete_("/invites/" + Platform::urlEncode(code), actionHandler("Revoking an invite", std::move(announced)));
}
void Client::resolveInvite(const std::string &code, ResultCallback<InvitePreview> callback) {
    httpClient->get("/invites/" + Platform::urlEncode(code), {{"with_counts", "true"}, {"with_expiration", "true"}}, valueHandler<InvitePreview>("Looking up an invite", std::move(callback), [](const Json &doc) { return parseInvitePreview(doc); }));
}
void Client::acceptInvite(const std::string &code, const std::string &location, ResultCallback<Snowflake> callback) {
    RequestOptions options; options.context = ContextProperties::location(location);
    httpClient->send(HttpClient::Method::POST, "/invites/" + Platform::urlEncode(code), Core::OrderedJson().toBytes(), options, valueHandler<Snowflake>("Joining a server", std::move(callback), [](const Json &doc) {
        if (doc.is_object()) {
            if (doc.contains("guild") && doc["guild"].is_object() && doc["guild"].contains("id") && doc["guild"]["id"].is_string()) return snowflake(doc["guild"]["id"].get<std::string>());
            if (doc.contains("guild_id") && doc["guild_id"].is_string()) return snowflake(doc["guild_id"].get<std::string>());
        }
        return Snowflake::Invalid;
    }));
}
void Client::setIncidentActions(Snowflake guildId, const Platform::DateTime &invitesDisabledUntil, const Platform::DateTime &dmsDisabledUntil, std::optional<int> lockdownHours, ActionCallback callback) {
    Core::OrderedJson body; body.insert("invites_disabled_until", timestamp(invitesDisabledUntil)).insert("dms_disabled_until", timestamp(dmsDisabledUntil)).insert("lockdown_duration_hours", lockdownHours ? Json(*lockdownHours) : Json()); httpClient->put(guildEndpoint(guildId, "/incident-actions"), body, actionHandler("Updating invite and DM pauses", std::move(callback)));
}
void Client::fetchGuildEmojis(Snowflake guildId, ResultCallback<std::vector<Emoji>> callback) { httpClient->get(guildEndpoint(guildId, "/emojis"), {}, valueHandler<std::vector<Emoji>>("Fetching emojis", std::move(callback), parseArray<Emoji>)); }
void Client::createGuildEmoji(Snowflake guildId, const std::string &name, const std::string &imageDataUri, const std::string &originalMd5, ResultCallback<Emoji> callback) {
    Core::OrderedJson body; body.insert("image", imageDataUri).insert("name", name); RequestOptions options; options.context = ContextProperties().add("client_event_source", "Guild Settings"); options.originalMd5 = originalMd5;
    httpClient->send(HttpClient::Method::POST, guildEndpoint(guildId, "/emojis"), body.toBytes(), options, valueHandler<Emoji>("Uploading an emoji", std::move(callback), [](const Json &doc) { return Emoji::fromJson(doc); }));
}
void Client::renameGuildEmoji(Snowflake guildId, Snowflake emojiId, const std::string &name, ActionCallback callback) { Core::OrderedJson body; body.insert("name", name); httpClient->patch(guildEndpoint(guildId, "/emojis/" + emojiId.toString()), body, actionHandler("Renaming an emoji", std::move(callback))); }
void Client::deleteGuildEmoji(Snowflake guildId, Snowflake emojiId, ActionCallback callback) { httpClient->delete_(guildEndpoint(guildId, "/emojis/" + emojiId.toString()), actionHandler("Deleting an emoji", std::move(callback))); }
void Client::fetchGuildStickers(Snowflake guildId, ResultCallback<std::vector<Sticker>> callback) { httpClient->get(guildEndpoint(guildId, "/stickers"), {}, valueHandler<std::vector<Sticker>>("Fetching stickers", std::move(callback), parseArray<Sticker>)); }
void Client::createGuildSticker(Snowflake guildId, const std::string &name, const std::string &tags, const std::string &description, const FileUpload &file, const std::string &originalMd5, ResultCallback<Sticker> callback) {
    FileUpload part = file; part.fieldName = "file"; RequestOptions options; options.originalMd5 = originalMd5;
    httpClient->postForm(guildEndpoint(guildId, "/stickers"), {{"name", name}, {"tags", tags}, {"description", description}}, {part}, options, valueHandler<Sticker>("Uploading a sticker", std::move(callback), [](const Json &doc) { return Sticker::fromJson(doc); }));
}
void Client::modifyGuildSticker(Snowflake guildId, Snowflake stickerId, const std::string &name, const std::string &tags, const std::string &description, ActionCallback callback) {
    Core::OrderedJson body; body.insert("name", name).insert("tags", tags).insert("description", description); httpClient->patch(guildEndpoint(guildId, "/stickers/" + stickerId.toString()), body, actionHandler("Editing a sticker", std::move(callback)));
}
void Client::deleteGuildSticker(Snowflake guildId, Snowflake stickerId, ActionCallback callback) { httpClient->delete_(guildEndpoint(guildId, "/stickers/" + stickerId.toString()), actionHandler("Deleting a sticker", std::move(callback))); }
void Client::fetchAuditLog(Snowflake guildId, const AuditLogQuery &auditQuery, ResultCallback<AuditLog> callback) {
    std::vector<std::pair<std::string, std::string>> query{{"limit", std::to_string(AuditLogQuery::PageSize)}}; if (auditQuery.before.isValid()) query.emplace_back("before", auditQuery.before.toString()); if (auditQuery.userId.isValid()) query.emplace_back("user_id", auditQuery.userId.toString()); if (auditQuery.actionType) query.emplace_back("action_type", std::to_string(static_cast<int>(*auditQuery.actionType)));
    httpClient->get(guildEndpoint(guildId, "/audit-logs"), query, valueHandler<AuditLog>("Fetching the audit log", std::move(callback), [](const Json &doc) { return AuditLog::fromJson(doc); }));
}
} // namespace Styx::Discord
