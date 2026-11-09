#include "Client.hpp"
#include "ApiError.hpp"
#include "CommandInteraction.hpp"
#include "ComponentInteraction.hpp"

#include <string>
#include <utility>

namespace Styx::Discord {
namespace {

void postInteraction(HttpClient &http, nlohmann::json body, Client::ActionCallback callback)
{
    http.post("/interactions", std::move(body),
              [callback = std::move(callback)](const HttpResponse &response) {
        if (!callback)
            return;
        if (!response.success) {
            auto error = ApiError::fromResponse(response);
            callback(Core::Result<void>::makeError(std::move(error.message), error.code));
            return;
        }
        if (response.statusCode != 204) {
            callback(Core::Result<void>::makeError("Unexpected status code " + std::to_string(response.statusCode)));
            return;
        }
        callback(Core::Result<void>::makeOk());
    });
}

} // namespace

void Client::createComponentInteraction(const ComponentInteractionRequest &request, ActionCallback callback)
{
    const auto sessionId = gateway->currentSessionId();
    auto bodyResult = componentInteractionBody(request, sessionId);
    if (!bodyResult.success()) {
        if (callback)
            callback(Core::Result<void>::makeError(std::move(bodyResult.error)));
        return;
    }

    postInteraction(*httpClient, std::move(*bodyResult.value), std::move(callback));
}

void Client::createApplicationCommandInteraction(const ApplicationCommandRequest &request, ActionCallback callback)
{
    const auto sessionId = gateway->currentSessionId();
    auto bodyResult = applicationCommandBody(request, sessionId);
    if (!bodyResult.success()) {
        if (callback)
            callback(Core::Result<void>::makeError(std::move(bodyResult.error)));
        return;
    }

    postInteraction(*httpClient, std::move(*bodyResult.value), std::move(callback));
}

} // namespace Styx::Discord
