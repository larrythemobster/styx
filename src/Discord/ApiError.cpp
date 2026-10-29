#include "ApiError.hpp"

#include <nlohmann/json.hpp>

#include "HttpClient.hpp"

namespace Styx {
namespace Discord {

namespace {

std::string firstNestedMessage(const nlohmann::json &errors)
{
    if (!errors.is_object())
        return {};
    const auto own = errors.find("_errors");
    if (own != errors.end() && own->is_array() && !own->empty()) {
        const auto &first = own->front();
        if (first.is_object()) {
            const auto message = first.find("message");
            if (message != first.end() && message->is_string())
                return message->get<std::string>();
        }
        return {};
    }
    for (auto it = errors.begin(); it != errors.end(); ++it)
        if (it->is_object())
            return firstNestedMessage(*it);
    return {};
}

} // namespace

ApiError ApiError::fromResponse(const HttpResponse &response)
{
    ApiError error;
    const auto body = nlohmann::json::parse(response.body, nullptr, false);
    if (body.is_object()) {
        const auto code = body.find("code");
        if (code != body.end() && code->is_number_integer())
            error.code = code->get<int>();
        if (error.code == InvalidFormBody) {
            const auto errors = body.find("errors");
            if (errors != body.end())
                error.message = firstNestedMessage(*errors);
        }
        if (error.message.empty()) {
            const auto message = body.find("message");
            if (message != body.end() && message->is_string())
                error.message = message->get<std::string>();
        }
    }
    if (error.message.empty())
        error.message = response.error;
    if (error.message.empty())
        error.message = "An unexpected error occurred.";
    return error;
}

} // namespace Discord
} // namespace Styx
