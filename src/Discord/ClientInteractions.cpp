#include "Client.hpp"
#include "ApiError.hpp"
#include "ComponentInteraction.hpp"

#include <string>
#include <utility>

namespace Styx::Discord {

void Client::createComponentInteraction(const ComponentInteractionRequest &request, ActionCallback callback)
{
    const auto sessionId = gateway->currentSessionId();
    auto bodyResult = componentInteractionBody(request, sessionId);
    if (!bodyResult.success()) {
        if (callback)
            callback(Core::Result<void>::makeError(std::move(bodyResult.error)));
        return;
    }

    httpClient->post("/interactions", *bodyResult.value,
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

} // namespace Styx::Discord
