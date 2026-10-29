#include "Client.hpp"
#include "ApiError.hpp"
#include "Core/Logging.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace Styx::Discord {
namespace {
using json = nlohmann::json;

std::string reactionLocationName(Client::ReactionLocation location)
{
    switch (location) {
    case Client::ReactionLocation::HoverBar: return "Message Hover Bar";
    case Client::ReactionLocation::InlineButton: return "Message Inline Button";
    case Client::ReactionLocation::ContextMenu: return "Message Context Menu";
    case Client::ReactionLocation::ReactionPicker: return "Message Reaction Picker";
    case Client::ReactionLocation::Message: return "Message";
    }
    return {};
}

std::string reactionsEndpoint(Snowflake channelId, Snowflake messageId, const std::string &key)
{
    std::string encoded;
    for (size_t start = 0, end;; start = end + 1) {
        end = key.find(':', start);
        encoded += Platform::urlEncode(key.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) break;
        encoded += ':';
    }
    return "/channels/" + channelId.toString() + "/messages/" + messageId.toString() + "/reactions/" + encoded;
}

Client::ReactionResult reactionResult(const HttpResponse &response)
{
    Client::ReactionResult result;
    result.success = response.success;
    result.statusCode = response.statusCode;
    if (response.success) return result;
    auto body = json::parse(response.body, nullptr, false);
    if (body.is_object()) {
        auto code = body.find("code");
        if (code != body.end() && code->is_number_integer()) result.errorCode = code->get<int>();
        auto retry = body.find("retry_after");
        if (response.rateLimited())
            result.retryAfterSeconds = std::max(1, static_cast<int>(std::ceil(retry != body.end() && retry->is_number() ? retry->get<double>() : 1.0)));
    } else if (response.rateLimited()) {
        result.retryAfterSeconds = 1;
    }
    return result;
}
}

void Client::sendMessage(Snowflake channelId, const std::string &content, const std::string &nonce,
                         Snowflake replyToMessageId, const std::vector<Core::PendingAttachment> &attachments)
{
    json payload = {{"content", content}, {"flags", 0}, {"mobile_network_type", "unknown"}, {"nonce", nonce}, {"tts", false}};
    if (replyToMessageId.isValid())
        payload["message_reference"] = {{"message_id", replyToMessageId.toString()}, {"channel_id", channelId.toString()}};
    const auto endpoint = "/channels/" + channelId.toString() + "/messages";
    if (!attachments.empty()) {
        auto state = std::make_shared<UploadState>();
        state->channelId = channelId;
        state->nonce = nonce;
        state->attachments = attachments;
        state->onUploaded = [this, endpoint, nonce, payload](const json &files) mutable {
            payload["attachments"] = files;
            httpClient->post(endpoint, payload, ContextProperties::location("chat_input"), [this, nonce](const HttpResponse &response) {
                if (!response.success) messageSendFailed.publish(nonce, response.error);
            });
        };
        state->onFailed = [this, nonce](const std::string &error) { messageSendFailed.publish(nonce, error); };
        state->uploadFilenames.resize(attachments.size());
        state->uploaded.resize(attachments.size(), false);
        state->cancelFlag = std::make_shared<std::atomic<bool>>(false);
        activeUploads[nonce] = state;
        uploadAttachmentsAndSend(state);
        return;
    }
    httpClient->post(endpoint, payload, ContextProperties::location("chat_input"), [this, nonce](const HttpResponse &response) {
        if (!response.success) messageSendFailed.publish(nonce, response.error);
    });
}

void Client::uploadAttachmentsAndSend(const std::shared_ptr<UploadState> &state)
{
    json files = json::array();
    for (size_t i = 0; i < state->attachments.size(); ++i)
        files.push_back({{"id", std::to_string(i)}, {"filename", state->attachments[i].filename}, {"file_size", state->attachments[i].size}});
    httpClient->post("/channels/" + state->channelId.toString() + "/attachments", json{{"files", files}}, [this, state](const HttpResponse &response) {
        if (state->cancelFlag->load()) { settleUpload(state); return; }
        if (!response.success) { settleUpload(state); failUpload(state, response.error); return; }
        auto body = json::parse(response.body, nullptr, false);
        auto slots = body.is_object() && body.contains("attachments") && body["attachments"].is_array() ? body["attachments"] : json::array();
        if (slots.size() != state->attachments.size()) { settleUpload(state); failUpload(state, "Unexpected upload slot response"); return; }
        std::vector<std::string> urls(state->attachments.size());
        for (const auto &slot : slots) {
            int index = 0;
            if (slot.is_object() && slot.contains("id")) {
                const auto &id = slot["id"];
                if (id.is_number_integer()) index = id.get<int>();
                else if (id.is_string()) {
                    const auto &text = id.get_ref<const std::string &>();
                    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), index);
                    if (ec != std::errc{} || end != text.data() + text.size()) index = 0;
                }
            }
            if (index < 0 || static_cast<size_t>(index) >= state->attachments.size()) { settleUpload(state); failUpload(state, "Unexpected upload slot response"); return; }
            if (slot.is_object()) {
                auto filename = slot.find("upload_filename");
                auto url = slot.find("upload_url");
                if (filename != slot.end() && filename->is_string()) state->uploadFilenames[index] = filename->get<std::string>();
                if (url != slot.end() && url->is_string()) urls[index] = url->get<std::string>();
            }
        }
        state->remaining = static_cast<int>(state->attachments.size());
        for (size_t index = 0; index < state->attachments.size(); ++index) {
            const auto &attachment = state->attachments[index];
            auto done = [this, state, index](const HttpResponse &putResponse) {
                --state->remaining;
                if (putResponse.success) state->uploaded[index] = true;
                else if (!state->failed && !state->cancelFlag->load()) {
                    state->failed = true;
                    failUpload(state, putResponse.error);
                    state->cancelFlag->store(true);
                }
                if (state->remaining > 0) return;
                if (state->failed || state->cancelFlag->load()) { cleanupUploadedSlots(state); settleUpload(state); return; }
                finishUpload(state);
            };
            auto progress = [this, state, index](int64_t sent, int64_t total) {
                attachmentUploadProgress.publish(state->nonce, static_cast<int>(index), sent, total);
            };
            if (!attachment.data.empty()) httpClient->putExternal(urls[index], attachment.data, attachment.mimeType, done, progress, state->cancelFlag);
            else httpClient->putExternalFile(urls[index], attachment.filePath, attachment.mimeType, done, progress, state->cancelFlag);
        }
    });
}

void Client::finishUpload(const std::shared_ptr<UploadState> &state)
{
    if (state->cancelFlag->load()) { cleanupUploadedSlots(state); settleUpload(state); return; }
    json files = json::array();
    for (size_t i = 0; i < state->attachments.size(); ++i) {
        const auto &attachment = state->attachments[i];
        json file = {{"id", std::to_string(i)}, {"filename", attachment.filename}, {"uploaded_filename", state->uploadFilenames[i]}};
        if (attachment.isSpoiler) file["is_spoiler"] = true;
        if (!attachment.description.empty()) file["description"] = attachment.description;
        files.push_back(std::move(file));
    }
    auto uploaded = state->onUploaded;
    settleUpload(state);
    uploaded(files);
}

void Client::failUpload(const std::shared_ptr<UploadState> &state, const std::string &error)
{
    if (state->onFailed) state->onFailed(error);
}

void Client::cleanupUploadedSlots(const std::shared_ptr<UploadState> &state)
{
    for (size_t i = 0; i < state->uploaded.size(); ++i)
        if (state->uploaded[i] && !state->uploadFilenames[i].empty())
            httpClient->delete_("/attachments/" + state->uploadFilenames[i], [](const auto &) {});
}

void Client::settleUpload(const std::shared_ptr<UploadState> &state) { activeUploads.erase(state->nonce); }

bool Client::cancelMessageSend(const std::string &nonce)
{
    auto it = activeUploads.find(nonce);
    if (it == activeUploads.end()) return false;
    it->second->cancelFlag->store(true);
    return true;
}

void Client::editMessage(Snowflake channelId, Snowflake messageId, const std::string &content)
{
    httpClient->patch("/channels/" + channelId.toString() + "/messages/" + messageId.toString(), json{{"content", content}}, [](const HttpResponse &response) {
        if (!response.success) Core::log(Core::LogLevel::Warning, LogDiscord) << "Failed to edit message:" << response.error;
    });
}

void Client::deleteMessage(Snowflake channelId, Snowflake messageId)
{
    httpClient->delete_("/channels/" + channelId.toString() + "/messages/" + messageId.toString(), [](const HttpResponse &response) {
        if (!response.success) Core::log(Core::LogLevel::Warning, LogDiscord) << "Failed to delete message:" << response.error;
    });
}

void Client::pinMessage(Snowflake channelId, Snowflake messageId)
{
    httpClient->put("/channels/" + channelId.toString() + "/pins/" + messageId.toString(), json::object(), [](const HttpResponse &response) {
        if (!response.success) Core::log(Core::LogLevel::Warning, LogDiscord) << "Failed to pin message:" << response.error;
    });
}

void Client::unpinMessage(Snowflake channelId, Snowflake messageId)
{
    httpClient->delete_("/channels/" + channelId.toString() + "/pins/" + messageId.toString(), [](const HttpResponse &response) {
        if (!response.success) Core::log(Core::LogLevel::Warning, LogDiscord) << "Failed to unpin message:" << response.error;
    });
}

void Client::addReaction(Snowflake channelId, Snowflake messageId, const std::string &key, bool isBurst, ReactionLocation location, ReactionCallback callback)
{
    auto endpoint = reactionsEndpoint(channelId, messageId, key) + "/%40me?location=" + Platform::urlEncode(reactionLocationName(location)) + "&type=" + (isBurst ? "1" : "0");
    httpClient->put(endpoint, json::object(), [callback = std::move(callback)](const HttpResponse &response) {
        if (callback) callback(reactionResult(response));
    });
}

void Client::removeReaction(Snowflake channelId, Snowflake messageId, const std::string &key, bool isBurst, ReactionLocation location, std::optional<Snowflake> reactorUnlessMe, ReactionCallback callback)
{
    auto endpoint = reactionsEndpoint(channelId, messageId, key) + "/" + (isBurst ? "1" : "0") + "/" + (reactorUnlessMe ? reactorUnlessMe->toString() : "%40me") + "?location=" + Platform::urlEncode(reactionLocationName(location)) + "&burst=" + (isBurst ? "true" : "false");
    httpClient->delete_(endpoint, [callback = std::move(callback)](const HttpResponse &response) {
        if (callback) callback(reactionResult(response));
    });
}

void Client::fetchReactors(Snowflake channelId, Snowflake messageId, const std::string &key, bool isBurst, int limit, std::optional<Snowflake> after, ResultCallback<std::vector<User>> callback)
{
    std::vector<std::pair<std::string, std::string>> query{{"limit", std::to_string(limit)}};
    if (after) query.emplace_back("after", after->toString());
    query.emplace_back("type", isBurst ? "1" : "0");
    httpClient->get(reactionsEndpoint(channelId, messageId, key), query, [callback = std::move(callback)](const HttpResponse &response) {
        if (!response.success) {
            auto error = ApiError::fromResponse(response);
            callback(Core::Result<std::vector<User>>::makeError(error.message, error.code));
            return;
        }
        auto body = json::parse(response.body, nullptr, false);
        std::vector<User> users;
        if (body.is_array()) for (const auto &value : body) users.push_back(User::fromJson(value.is_object() ? value : json::object()));
        callback(Core::Result<std::vector<User>>::makeOk(std::move(users)));
    });
}

void Client::leaveGuild(Snowflake guildId)
{
    httpClient->delete_("/users/@me/guilds/" + guildId.toString(), json::object(), [this, guildId](const HttpResponse &response) {
        if (!response.success) guildLeaveFailed.publish(guildId, "status=" + std::to_string(response.statusCode) + " error=" + response.error);
    });
}

void Client::ackMessage(Snowflake channelId, Snowflake messageId, int flags, int lastViewed)
{
    httpClient->post("/channels/" + channelId.toString() + "/messages/" + messageId.toString() + "/ack", json{{"flags", flags}, {"last_viewed", lastViewed}, {"token", nullptr}}, [](const HttpResponse &response) {
        if (!response.success) Core::log(Core::LogLevel::Warning, LogDiscord) << "Failed to ack message:" << response.error;
    });
}

void Client::ackBulk(const std::vector<AckEntry> &entries, std::function<void(bool)> onFinished)
{
    json states = json::array();
    for (const auto &entry : entries)
        states.push_back({{"channel_id", entry.channelId.toString()}, {"message_id", entry.messageId.toString()}, {"read_state_type", entry.readStateType}});
    httpClient->post("/read-states/ack-bulk", json{{"read_states", states}}, [onFinished = std::move(onFinished)](const HttpResponse &response) {
        if (!response.success) Core::log(Core::LogLevel::Warning, LogDiscord) << "Failed to bulk ack:" << response.error;
        onFinished(response.success);
    });
}

} // namespace Styx::Discord
