#include "MessageSearch.hpp"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace Styx::Discord {
namespace {

using Json = nlohmann::json;
using Parameters = std::vector<std::pair<std::string, std::string>>;

// Discord rejects offsets past this.
constexpr int MAX_OFFSET = 9975;

void addIds(Parameters &parameters, const char *key, const std::vector<Core::Snowflake> &ids)
{
    for (const auto id : ids)
        parameters.emplace_back(key, id.toString());
}

} // namespace

std::vector<std::pair<std::string, std::string>> searchParameters(const MessageSearchQuery &query, bool server)
{
    Parameters parameters;

    if (!query.content.empty())
        parameters.emplace_back("content", query.content);
    addIds(parameters, "author_id", query.authorIds);
    addIds(parameters, "mentions", query.mentions);
    if (server)
        addIds(parameters, "channel_id", query.channelIds);
    for (const auto &has : query.has)
        parameters.emplace_back("has", has);
    if (query.pinned.has_value())
        parameters.emplace_back("pinned", *query.pinned ? "true" : "false");
    if (query.before.isValid())
        parameters.emplace_back("max_id", query.before.toString());
    if (query.after.isValid())
        parameters.emplace_back("min_id", query.after.toString());
    if (query.offset > 0)
        parameters.emplace_back("offset", std::to_string(std::min(query.offset, MAX_OFFSET)));
    parameters.emplace_back("sort_by", "timestamp");
    parameters.emplace_back("sort_order", query.oldestFirst ? "asc" : "desc");

    return parameters;
}

MessageSearchResult parseSearchResponse(int statusCode, const std::string &body)
{
    MessageSearchResult result;
    const auto parsed = Json::parse(body, nullptr, false);
    const Json obj = parsed.is_object() ? parsed : Json::object();
    if (statusCode == 202) {
        result.indexNotReady = true;
        auto retry = obj.find("retry_after");
        const double seconds = retry != obj.end() && retry->is_number() ? retry->get<double>() : 1.0;
        // A retry_after of 0 still needs a short pause.
        result.retryAfterSeconds = std::max(1, static_cast<int>(std::round(seconds)));
        return result;
    }
    auto total = obj.find("total_results");
    if (total != obj.end() && total->is_number_integer())
        result.totalResults = total->get<int>();
    auto messages = obj.find("messages");
    if (messages != obj.end() && messages->is_array()) {
        for (const auto &group : *messages) {
            if (!group.is_array() || group.empty())
                continue;
            // Older responses surrounded each match with context and marked the match as the hit.
            auto hit = std::find_if(group.begin(), group.end(), [](const Json &message) {
                if (!message.is_object())
                    return false;
                const auto it = message.find("hit");
                return it != message.end() && it->is_boolean() && it->get<bool>();
            });
            const Json &match = hit != group.end() ? *hit : group.front();
            if (match.is_object())
                result.messages.push_back(Message::fromJson(match));
        }
    }
    auto threads = obj.find("threads");
    if (threads != obj.end() && threads->is_array())
        for (const auto &thread : *threads)
            if (thread.is_object())
                result.threads.push_back(Channel::fromJson(thread));
    return result;
}

} // namespace Styx::Discord
