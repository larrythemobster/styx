#include "Client.hpp"

#include "Core/Logging.hpp"
#include "Platform/Dispatcher.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <utility>

namespace Styx::Discord {
namespace {
using Json = nlohmann::json;
using Query = std::vector<std::pair<std::string, std::string>>;
// How many times a search waits for Discord's index before giving up.
constexpr int SEARCH_ATTEMPTS = 5;

Json object(const std::string &body)
{
    auto value = Json::parse(body, nullptr, false);
    return value.is_object() ? std::move(value) : Json::object();
}

Json arrayField(const Json &obj, const char *key)
{
    auto it = obj.find(key);
    return it != obj.end() && it->is_array() ? *it : Json::array();
}

std::string stringField(const Json &obj, const char *key)
{
    auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

template <typename ResultT, typename ParseExtra>
void runThreadSearch(HttpClient *http, Snowflake channelId, const Query &query,
                     const std::string &errorPrefix,
                     std::function<void(const Core::Result<ResultT> &)> callback,
                     ParseExtra parseExtra)
{
    http->get("/channels/" + channelId.toString() + "/threads/search", query,
              [callback = std::move(callback), parseExtra, errorPrefix](const HttpResponse &response) {
        const auto obj = object(response.body);
        if (response.statusCode == 202) {
            ResultT result;
            result.indexNotReady = true;
            auto retry = obj.find("retry_after");
            const double seconds = retry != obj.end() && retry->is_number() ? retry->get<double>() : 1.0;
            result.retryAfterSeconds = std::max(1, static_cast<int>(std::round(seconds)));
            callback(Core::Result<ResultT>::makeOk(std::move(result)));
            return;
        }
        if (!response.success) {
            Core::log(Core::LogLevel::Warning, LogDiscord) << errorPrefix << " " << response.error;
            callback(Core::Result<ResultT>::makeError(errorPrefix + " " + response.error));
            return;
        }
        ResultT result;
        auto hasMore = obj.find("has_more");
        result.hasMore = hasMore != obj.end() && hasMore->is_boolean() && hasMore->get<bool>();
        for (const auto &value : arrayField(obj, "threads"))
            result.threads.push_back(Channel::fromJson(value.is_object() ? value : Json::object()));
        parseExtra(obj, result);
        callback(Core::Result<ResultT>::makeOk(std::move(result)));
    });
}
}

void Client::fetchMessages(Snowflake channelId, Query query, int limit, MessagesCallback callback)
{
    query.emplace_back("limit", std::to_string(limit));
    httpClient->get("/channels/" + channelId.toString() + "/messages", query,
                    [callback = std::move(callback)](const HttpResponse &response) {
        if (!response.success) {
            callback(Core::Result<std::vector<Message>>::makeError(response.error));
            return;
        }
        std::vector<Message> results;
        const auto arr = Json::parse(response.body, nullptr, false);
        if (arr.is_array()) {
            results.reserve(arr.size());
            for (const auto &value : arr)
                results.push_back(Message::fromJson(value.is_object() ? value : Json::object()));
        }
        callback(Core::Result<std::vector<Message>>::makeOk(std::move(results)));
    });
}

void Client::fetchLatestMessages(Snowflake channelId, int limit, MessagesCallback callback)
{
    fetchMessages(channelId, {}, limit, std::move(callback));
}

void Client::fetchHistory(Snowflake channelId, Snowflake beforeId, int limit, MessagesCallback callback)
{
    fetchMessages(channelId, {{"before", beforeId.toString()}}, limit, std::move(callback));
}

void Client::fetchMessagesAfter(Snowflake channelId, Snowflake afterId, int limit, MessagesCallback callback)
{
    fetchMessages(channelId, {{"after", afterId.toString()}}, limit, std::move(callback));
}

void Client::fetchMessagesAround(Snowflake channelId, Snowflake messageId, int limit, MessagesCallback callback)
{
    fetchMessages(channelId, {{"around", messageId.toString()}}, limit, std::move(callback));
}

void Client::fetchUserProfile(Snowflake userId, Snowflake guildId, ProfileCallback callback)
{
    Query query{{"type", "popout"}, {"with_mutual_guilds", "true"},
                {"with_mutual_friends", "true"}, {"with_mutual_friends_count", "false"}};
    if (guildId.isValid()) query.emplace_back("guild_id", guildId.toString());
    httpClient->get("/users/" + userId.toString() + "/profile", query,
                    [userId, callback = std::move(callback)](const HttpResponse &response) {
        if (!response.success) {
            Core::log(Core::LogLevel::Warning, LogDiscord) << "Failed to fetch user profile for " << userId.toString() << ": " << response.error;
            callback(Core::Result<UserProfile>::makeError("Failed to fetch user profile: " + response.error));
            return;
        }
        callback(Core::Result<UserProfile>::makeOk(UserProfile::fromJson(object(response.body))));
    });
}

std::string Client::applicationIconHash(Snowflake applicationId)
{
    if (!applicationId.isValid()) return {};
    auto cached = applicationIcons.find(applicationId);
    if (cached != applicationIcons.end()) return cached->second;
    if (applicationIconRequests.contains(applicationId)) return {};
    applicationIconRequests.insert(applicationId);
    Platform::Guard<Client> self(this);
    httpClient->get("/applications/" + applicationId.toString() + "/public", {},
                    [self, applicationId](const HttpResponse &response) {
        if (!self) return;
        self->applicationIconRequests.erase(applicationId);
        std::string icon;
        if (response.success) icon = stringField(object(response.body), "icon");
        else Core::log(Core::LogLevel::Warning, LogDiscord) << "Failed to fetch application " << applicationId.toString() << ": " << response.error;
        self->applicationIcons.insert_or_assign(applicationId, icon);
        if (!icon.empty()) self->applicationIconResolved.publish(applicationId);
    });
    return {};
}

void Client::refreshAttachmentUrls(const std::vector<std::string> &urls, RefreshedUrlsCallback callback)
{
    Json body = {{"attachment_urls", urls}};
    httpClient->post("/attachments/refresh-urls", body, [urls, callback = std::move(callback)](const HttpResponse &response) {
        if (!response.success) {
            Core::log(Core::LogLevel::Warning, LogDiscord) << "Failed to refresh attachment URLs: " << response.error;
            callback(Core::Result<std::unordered_map<std::string, std::string>>::makeError(response.error));
            return;
        }
        std::unordered_map<std::string, std::string> refreshed;
        const auto entries = arrayField(object(response.body), "refreshed_urls");
        for (std::size_t i = 0; i < entries.size() && i < urls.size(); ++i) {
            const auto fresh = entries[i].is_object() ? stringField(entries[i], "refreshed") : std::string{};
            if (!fresh.empty()) refreshed.insert_or_assign(urls[i], fresh);
        }
        callback(Core::Result<std::unordered_map<std::string, std::string>>::makeOk(std::move(refreshed)));
    });
}

void Client::setUserNote(Snowflake userId, const std::string &note)
{
    httpClient->put("/users/@me/notes/" + userId.toString(), Json{{"note", note}}, [userId](const HttpResponse &response) {
        if (!response.success) Core::log(Core::LogLevel::Warning, LogDiscord) << "Failed to set note for user " << userId.toString() << ": " << response.error;
    });
}

void Client::openDmChannel(Snowflake userId, DmChannelCallback callback)
{
    Json body = {{"recipients", Json::array({userId.toString()})}};
    httpClient->post("/users/@me/channels", body, ContextProperties::empty(), [userId, callback = std::move(callback)](const HttpResponse &response) {
        if (!response.success) {
            Core::log(Core::LogLevel::Warning, LogDiscord) << "Failed to open DM with user " << userId.toString() << ": " << response.error;
            callback(Core::Result<ChannelCreate>::makeError(response.error));
            return;
        }
        callback(Core::Result<ChannelCreate>::makeOk(ChannelCreate::fromJson(object(response.body))));
    });
}

void Client::searchForumThreads(Snowflake forumId, int offset, const std::string &sortBy, ForumThreadsCallback callback)
{
    Query query{{"sort_by", sortBy}, {"sort_order", "desc"}, {"limit", "25"}, {"tag_setting", "match_some"}};
    if (offset > 0) query.emplace_back("offset", std::to_string(offset));
    runThreadSearch<ForumThreadSearchResult>(httpClient, forumId, query, "Failed to search forum threads:", std::move(callback),
        [](const Json &obj, ForumThreadSearchResult &result) {
            for (const auto &value : arrayField(obj, "first_messages")) {
                auto msg = Message::fromJson(value.is_object() ? value : Json::object());
                if (msg.channelId.hasValue()) result.firstMessages.insert_or_assign(msg.channelId.get(), std::move(msg));
            }
        });
}

void Client::joinThread(Snowflake threadId)
{
    httpClient->put("/channels/" + threadId.toString() + "/thread-members/@me", Json::object(), [threadId](const HttpResponse &response) {
        if (!response.success) Core::log(Core::LogLevel::Warning, LogDiscord) << "Failed to join thread " << threadId.toString() << ": " << response.error;
    });
}

void Client::leaveThread(Snowflake threadId)
{
    httpClient->delete_("/channels/" + threadId.toString() + "/thread-members/@me", [threadId](const HttpResponse &response) {
        if (!response.success) Core::log(Core::LogLevel::Warning, LogDiscord) << "Failed to leave thread " << threadId.toString() << ": " << response.error;
    });
}

void Client::searchThreads(Snowflake channelId, bool archived, int offset, ThreadListCallback callback)
{
    Query query{{"archived", archived ? "true" : "false"}, {"sort_by", "last_message_time"}, {"sort_order", "desc"}, {"limit", "25"}};
    if (offset > 0) query.emplace_back("offset", std::to_string(offset));
    runThreadSearch<ThreadListResult>(httpClient, channelId, query, "Failed to search threads:", std::move(callback),
        [](const Json &obj, ThreadListResult &result) {
            for (const auto &value : arrayField(obj, "members"))
                result.members.push_back(ThreadMember::fromJson(value.is_object() ? value : Json::object()));
        });
}

void Client::searchMessages(Snowflake guildId, Snowflake channelId, const MessageSearchQuery &query, MessageSearchCallback callback)
{
    const bool server = guildId.isValid();
    std::string endpoint = server ? "/guilds/" + guildId.toString() + "/messages/search" : "/channels/" + channelId.toString() + "/messages/search";
    runMessageSearch(std::move(endpoint), searchParameters(query, server), SEARCH_ATTEMPTS, std::move(callback));
}

void Client::runMessageSearch(std::string endpoint, Query query, int attemptsLeft, MessageSearchCallback callback)
{
    Platform::Guard<Client> self(this);
    httpClient->get(endpoint, query, [self, endpoint, query, attemptsLeft, callback = std::move(callback)](const HttpResponse &response) {
        if (!self) return;
        if (response.statusCode != 202 && !response.success) {
            Core::log(Core::LogLevel::Warning, LogDiscord) << "Failed to search messages: " << response.error;
            callback(Core::Result<MessageSearchResult>::makeError("Failed to search messages: " + response.error));
            return;
        }
        auto result = parseSearchResponse(response.statusCode, response.body);
        if (!result.indexNotReady || attemptsLeft <= 1) {
            callback(Core::Result<MessageSearchResult>::makeOk(std::move(result)));
            return;
        }
        Platform::Dispatcher::main().setTimer(static_cast<uint32_t>(result.retryAfterSeconds * 1000), false, [self, endpoint, query, attemptsLeft, callback] {
            if (self) self->runMessageSearch(endpoint, query, attemptsLeft - 1, callback);
        });
    });
}

void Client::createForumThread(Snowflake forumId, const std::string &name, const std::vector<Snowflake> &appliedTags,
                               const std::string &content, const std::string &nonce,
                               const std::vector<Core::PendingAttachment> &attachments, ForumThreadCallback callback)
{
    Json message = {{"content", content}};
    if (attachments.empty()) {
        postForumThread(forumId, name, appliedTags, message, std::move(callback));
        return;
    }
    auto state = std::make_shared<UploadState>();
    state->channelId = forumId;
    state->nonce = nonce;
    state->attachments = attachments;
    state->onUploaded = [this, forumId, name, appliedTags, message, callback](const Json &attachmentsJson) {
        auto withFiles = message;
        withFiles["attachments"] = attachmentsJson;
        postForumThread(forumId, name, appliedTags, withFiles, callback);
    };
    state->onFailed = [callback](const std::string &error) {
        if (callback) callback(Core::Result<CreatedForumThread>::makeError("Failed to create forum post: " + error));
    };
    state->uploadFilenames.resize(attachments.size());
    state->uploaded.resize(attachments.size(), false);
    state->cancelFlag = std::make_shared<std::atomic<bool>>(false);
    activeUploads.insert_or_assign(nonce, state);
    uploadAttachmentsAndSend(state);
}

void Client::postForumThread(Snowflake forumId, const std::string &name, const std::vector<Snowflake> &appliedTags,
                             const Json &message, ForumThreadCallback callback)
{
    Json tags = Json::array();
    for (auto tag : appliedTags) tags.push_back(tag.toString());
    Json body = {{"name", name}, {"auto_archive_duration", 1440}, {"applied_tags", std::move(tags)}, {"message", message}};
    httpClient->post("/channels/" + forumId.toString() + "/threads?use_nested_fields=true", body,
                     [callback = std::move(callback)](const HttpResponse &response) {
        if (!response.success) {
            Core::log(Core::LogLevel::Warning, LogDiscord) << "Failed to create forum thread: " << response.error;
            if (callback) callback(Core::Result<CreatedForumThread>::makeError("Failed to create forum thread: " + response.error));
            return;
        }
        const auto obj = object(response.body);
        CreatedForumThread created;
        created.thread = Channel::fromJson(obj);
        auto message = obj.find("message");
        if (message != obj.end()) created.starterMessage = Message::fromJson(message->is_object() ? *message : Json::object());
        if (callback) callback(Core::Result<CreatedForumThread>::makeOk(std::move(created)));
    });
}

void Client::fetchForumPostData(Snowflake forumId, const std::vector<Snowflake> &threadIds, ForumPostDataCallback callback)
{
    Json ids = Json::array();
    for (auto id : threadIds) ids.push_back(id.toString());
    httpClient->post("/channels/" + forumId.toString() + "/post-data", Json{{"thread_ids", std::move(ids)}},
                     [callback = std::move(callback)](const HttpResponse &response) {
        if (!response.success) {
            Core::log(Core::LogLevel::Warning, LogDiscord) << "Failed to fetch forum post data: " << response.error;
            callback(Core::Result<std::unordered_map<Snowflake, Message>>::makeError("Failed to fetch forum post data: " + response.error));
            return;
        }
        std::unordered_map<Snowflake, Message> firstMessages;
        const auto obj = object(response.body);
        auto threads = obj.find("threads");
        if (threads != obj.end() && threads->is_object()) {
            for (auto it = threads->begin(); it != threads->end(); ++it) {
                if (!it.value().is_object()) continue;
                auto message = it.value().find("first_message");
                if (message == it.value().end() || !message->is_object()) continue;
                std::uint64_t id = 0;
                const auto &key = it.key();
                const auto parsed = std::from_chars(key.data(), key.data() + key.size(), id);
                if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size()) id = 0;
                firstMessages.insert_or_assign(Snowflake(id), Message::fromJson(*message));
            }
        }
        callback(Core::Result<std::unordered_map<Snowflake, Message>>::makeOk(std::move(firstMessages)));
    });
}

} // namespace Styx::Discord
