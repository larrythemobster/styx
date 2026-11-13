#include "AccountSecurity.hpp"
#include "ApiError.hpp"
#include "HttpClient.hpp"
#include "Core/TokenUtils.hpp"

#include <cstddef>
#include <utility>

namespace Styx::Discord {
namespace {
std::size_t characterCount(std::string_view text)
{
    std::size_t count = 0;
    for (const char c : text) count += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    return count;
}
}

std::optional<std::string> passwordChangeProblem(std::string_view current, std::string_view next, std::string_view confirm)
{
    if (current.empty()) return "Enter your current password.";
    const auto length = characterCount(next);
    if (length < 8) return "The new password must be at least 8 characters.";
    if (length > 72) return "The new password can't be longer than 72 characters.";
    if (next != confirm) return "The new passwords don't match.";
    if (next == current) return "The new password is the same as the current one.";
    return std::nullopt;
}

nlohmann::json passwordChangeBody(const std::string &current, const std::string &next)
{
    return {{"password", current}, {"new_password", next}};
}

Core::Result<std::string> passwordChangeResult(const HttpResponse &response, Core::Snowflake userId)
{
    if (!response.success) {
        auto error = ApiError::fromResponse(response);
        if (error.code == MfaRequiredCode)
            error.message = "This account uses multi-factor authentication, which Styx can't confirm yet. "
                            "Change the password in the official Discord app.";
        return Core::Result<std::string>::makeError(std::move(error.message), error.code);
    }
    const auto body = nlohmann::json::parse(response.body, nullptr, false);
    const auto token = body.is_object() ? body.find("token") : body.end();
    if (!body.is_object() || token == body.end() || !token->is_string() || token->get_ref<const std::string &>().empty())
        return Core::Result<std::string>::makeError("Discord changed the password but didn't return a new sign-in. Sign in again.");
    auto value = token->get<std::string>();
    if (Core::TokenUtils::getIdAndCheckToken(value) != userId)
        return Core::Result<std::string>::makeError("Discord returned a sign-in for a different account. Sign in again.");
    return Core::Result<std::string>::makeOk(std::move(value));
}

} // namespace Styx::Discord
