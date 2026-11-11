#include "Client.hpp"
#include "Core/JsonUtils.hpp"

namespace Styx::Discord {

void Client::fetchRecentMentions(Snowflake before, int limit, MessagesCallback callback)
{
    using Result = Core::Result<std::vector<Message>>;
    if (limit < 1 || limit > 100) {
        callback(Result::makeError("Mention page size must be between 1 and 100"));
        return;
    }
    std::vector<std::pair<std::string, std::string>> query{{"limit", std::to_string(limit)}};
    if (before.isValid())
        query.emplace_back("before", before.toString());
    httpClient->get("/users/@me/mentions", query, [callback = std::move(callback)](const HttpResponse &response) {
        if (!response.success) {
            callback(Result::makeError(response.error.empty() ? "Could not load mentions" : response.error, response.statusCode));
            return;
        }
        const auto json = nlohmann::json::parse(response.body, nullptr, false);
        if (!json.is_array()) {
            callback(Result::makeError("Invalid mentions response"));
            return;
        }
        std::vector<Message> messages;
        messages.reserve(json.size());
        for (const auto &item : json) {
            if (!item.is_object() || !item.contains("id") || !item["id"].is_string() ||
                !item.contains("channel_id") || !item["channel_id"].is_string() ||
                !Core::JsonUtils::detail::snowflake(item["id"].get<std::string>()).isValid() ||
                !Core::JsonUtils::detail::snowflake(item["channel_id"].get<std::string>()).isValid()) {
                callback(Result::makeError("Invalid message in mentions response"));
                return;
            }
            messages.push_back(Message::fromJson(item));
        }
        callback(Result::makeOk(std::move(messages)));
    });
}

} // namespace Styx::Discord
