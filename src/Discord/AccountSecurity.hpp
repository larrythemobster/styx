#pragma once

#include "Core/Result.hpp"
#include "Core/Snowflake.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace Styx::Discord {

struct HttpResponse;

// Discord's JSON error code when an action needs multi-factor verification first.
inline constexpr int MfaRequiredCode = 60003;

// What is wrong with a password change before it is sent, if anything. New passwords
// are 8 to 72 characters and must be typed the same way twice.
[[nodiscard]] std::optional<std::string> passwordChangeProblem(std::string_view current, std::string_view next,
                                                               std::string_view confirm);

// PATCH /users/@me body for a password change. Holds the passwords; don't keep or log it.
[[nodiscard]] nlohmann::json passwordChangeBody(const std::string &current, const std::string &next);

// Changing the password revokes every token for the account. The response is the user
// with a replacement token for this session; reject one that belongs to another user.
[[nodiscard]] Core::Result<std::string> passwordChangeResult(const HttpResponse &response, Core::Snowflake userId);

} // namespace Styx::Discord
