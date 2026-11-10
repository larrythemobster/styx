#include "GroupDm.hpp"
#include "ApiError.hpp"
#include "HttpClient.hpp"

#include <initializer_list>
#include <unordered_set>
#include <utility>

namespace Styx::Discord {
namespace {

bool validId(Core::Snowflake id)
{
    return id.isValid() && id != Core::Snowflake(0);
}

} // namespace

Core::OrderedJson GroupDmEdit::toJson() const
{
    Core::OrderedJson obj;
    insert(obj, "name", name);
    insert(obj, "icon", icon);
    return obj;
}

Core::Result<nlohmann::json> groupDmCreateBody(const std::vector<Core::Snowflake> &recipients)
{
    std::unordered_set<Core::Snowflake> seen;
    if (recipients.size() > 1)
        seen.reserve(recipients.size());
    for (const auto &id : recipients) {
        if (!validId(id))
            return Core::Result<nlohmann::json>::makeError("Invalid recipient ID");
        if (recipients.size() > 1 && !seen.insert(id).second)
            return Core::Result<nlohmann::json>::makeError("Duplicate recipient ID");
    }

    nlohmann::json body = nlohmann::json::object();
    if (recipients.size() == 1) {
        body["recipient_id"] = recipients[0].toString();
    } else {
        nlohmann::json arr = nlohmann::json::array();
        arr.get_ref<nlohmann::json::array_t &>().reserve(recipients.size());
        for (const auto &id : recipients)
            arr.push_back(id.toString());
        body["recipients"] = std::move(arr);
    }
    return Core::Result<nlohmann::json>::makeOk(std::move(body));
}

Core::Result<Channel> groupDmChannelResult(const HttpResponse &response,
                                          std::optional<Core::Snowflake> expectedChannelId)
{
    if (!response.success) {
        auto error = ApiError::fromResponse(response);
        return Core::Result<Channel>::makeError(std::move(error.message), error.code);
    }
    if (response.statusCode != 200)
        return Core::Result<Channel>::makeError("Unexpected status code " + std::to_string(response.statusCode));

    auto obj = nlohmann::json::parse(response.body, nullptr, false);
    if (!obj.is_object())
        return Core::Result<Channel>::makeError("Invalid group DM response");

    const auto idIt = obj.find("id");
    if (idIt == obj.end() || !idIt->is_string())
        return Core::Result<Channel>::makeError("Invalid group DM response");

    const auto typeIt = obj.find("type");
    if (typeIt == obj.end() || !typeIt->is_number_integer() || *typeIt != 3)
        return Core::Result<Channel>::makeError("Invalid group DM response");

    const auto recipientsIt = obj.find("recipients");
    if (recipientsIt != obj.end()) {
        if (!recipientsIt->is_array())
            return Core::Result<Channel>::makeError("Invalid group DM response");
        for (const auto &userObj : *recipientsIt) {
            if (!userObj.is_object())
                return Core::Result<Channel>::makeError("Invalid group DM response");
            const auto userIdIt = userObj.find("id");
            if (userIdIt == userObj.end() || !userIdIt->is_string())
                return Core::Result<Channel>::makeError("Invalid group DM response");
        }
    }

    const auto ownerIdIt = obj.find("owner_id");
    if (ownerIdIt != obj.end() && !ownerIdIt->is_string())
        return Core::Result<Channel>::makeError("Invalid group DM response");

    for (const auto *key : {"name", "icon"}) {
        const auto field = obj.find(key);
        if (field != obj.end() && !field->is_string() && !field->is_null())
            return Core::Result<Channel>::makeError("Invalid group DM response");
    }

    try {
        auto channel = Channel::fromJson(obj);
        if (!validId(channel.id.get()) || (channel.ownerId.hasValue() && !validId(channel.ownerId.get())))
            return Core::Result<Channel>::makeError("Invalid group DM response");
        if (channel.recipients.hasValue()) {
            std::unordered_set<Core::Snowflake> seenUserIds;
            if (channel.recipients->size() > 1)
                seenUserIds.reserve(channel.recipients->size());
            for (const auto &user : channel.recipients.get()) {
                if (!validId(user.id.get()) || (channel.recipients->size() > 1 && !seenUserIds.insert(user.id.get()).second))
                    return Core::Result<Channel>::makeError("Invalid group DM response");
            }
        }
        if (expectedChannelId.has_value() && channel.id.get() != *expectedChannelId)
            return Core::Result<Channel>::makeError("Invalid group DM response");
        return Core::Result<Channel>::makeOk(std::move(channel));
    } catch (const nlohmann::json::exception &) {
        return Core::Result<Channel>::makeError("Invalid group DM response");
    }
}

Core::Result<std::optional<Channel>> addDmRecipientResult(const HttpResponse &response)
{
    if (response.success && response.statusCode == 204)
        return Core::Result<std::optional<Channel>>::makeOk(std::nullopt);

    auto channelResult = groupDmChannelResult(response);
    if (!channelResult.success())
        return Core::Result<std::optional<Channel>>::makeError(std::move(channelResult.error), channelResult.code);
    return Core::Result<std::optional<Channel>>::makeOk(std::make_optional(std::move(*channelResult.value)));
}

Core::Result<void> groupDmActionResult(const HttpResponse &response)
{
    if (!response.success) {
        auto error = ApiError::fromResponse(response);
        return Core::Result<void>::makeError(std::move(error.message), error.code);
    }
    if (response.statusCode != 204)
        return Core::Result<void>::makeError("Unexpected status code " + std::to_string(response.statusCode));
    return Core::Result<void>::makeOk();
}

} // namespace Styx::Discord
