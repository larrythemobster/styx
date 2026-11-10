#include "Client.hpp"
#include "GroupDm.hpp"

#include <utility>

namespace Styx::Discord {
namespace {

bool validId(Snowflake id)
{
    return id.isValid() && id != Snowflake(0);
}

} // namespace

void Client::createGroupDm(const std::vector<Snowflake> &recipients, ResultCallback<Channel> callback)
{
    auto body = groupDmCreateBody(recipients);
    if (!body.success()) {
        if (callback)
            callback(Core::Result<Channel>::makeError(std::move(body.error), body.code));
        return;
    }

    const std::string endpoint = recipients.size() == 1
        ? "/users/@me/channels/group-dm-shell"
        : "/users/@me/channels";

    httpClient->post(endpoint, *body.value, ContextProperties::empty(),
                     [callback = std::move(callback)](const HttpResponse &response) {
        if (!callback)
            return;
        callback(groupDmChannelResult(response));
    });
}

void Client::addDmRecipient(Snowflake channelId, Snowflake userId, ResultCallback<std::optional<Channel>> callback)
{
    if (!validId(channelId)) {
        if (callback)
            callback(Core::Result<std::optional<Channel>>::makeError("Invalid channel ID"));
        return;
    }
    if (!validId(userId)) {
        if (callback)
            callback(Core::Result<std::optional<Channel>>::makeError("Invalid recipient ID"));
        return;
    }

    httpClient->put("/channels/" + channelId.toString() + "/recipients/" + userId.toString(),
                    nlohmann::json::object(),
                    [callback = std::move(callback)](const HttpResponse &response) {
        if (!callback)
            return;
        callback(addDmRecipientResult(response));
    });
}

void Client::removeGroupDmRecipient(Snowflake channelId, Snowflake userId, ActionCallback callback)
{
    if (!validId(channelId)) {
        if (callback)
            callback(Core::Result<void>::makeError("Invalid channel ID"));
        return;
    }
    if (!validId(userId)) {
        if (callback)
            callback(Core::Result<void>::makeError("Invalid recipient ID"));
        return;
    }

    httpClient->delete_("/channels/" + channelId.toString() + "/recipients/" + userId.toString(),
                         [callback = std::move(callback)](const HttpResponse &response) {
        if (!callback)
            return;
        callback(groupDmActionResult(response));
    });
}

void Client::modifyGroupDm(Snowflake channelId, const GroupDmEdit &edit, ResultCallback<Channel> callback)
{
    if (!validId(channelId)) {
        if (callback)
            callback(Core::Result<Channel>::makeError("Invalid channel ID"));
        return;
    }

    httpClient->patch("/channels/" + channelId.toString(), edit.toJson(),
                      [channelId, callback = std::move(callback)](const HttpResponse &response) {
        if (!callback)
            return;
        callback(groupDmChannelResult(response, channelId));
    });
}

void Client::leaveGroupDm(Snowflake channelId, bool silent, ResultCallback<Channel> callback)
{
    if (!validId(channelId)) {
        if (callback)
            callback(Core::Result<Channel>::makeError("Invalid channel ID"));
        return;
    }

    const std::string endpoint = "/channels/" + channelId.toString() + (silent ? "?silent=true" : "?silent=false");
    httpClient->delete_(endpoint, [channelId, callback = std::move(callback)](const HttpResponse &response) {
        if (!callback)
            return;
        callback(groupDmChannelResult(response, channelId));
    });
}

} // namespace Styx::Discord
