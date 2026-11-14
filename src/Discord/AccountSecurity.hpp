#pragma once

#include "Core/Result.hpp"
#include "Core/Snowflake.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Styx::Discord {

struct HttpResponse;

// Discord's JSON error code when an action needs multi-factor verification first.
inline constexpr int MfaRequiredCode = 60003;
// Discord's answer to a password-only TOTP enable request when the password is right.
inline constexpr int TotpPasswordAcceptedCode = 60005;

// What is wrong with a password change before it is sent, if anything. New passwords
// are 8 to 72 characters and must be typed the same way twice.
[[nodiscard]] std::optional<std::string> passwordChangeProblem(std::string_view current, std::string_view next,
                                                               std::string_view confirm);

// PATCH /users/@me body for a password change. Holds the passwords; don't keep or log it.
[[nodiscard]] nlohmann::json passwordChangeBody(const std::string &current, const std::string &next);

// Changing the password revokes every token for the account. The response is the user
// with a replacement token for this session; reject one that belongs to another user.
[[nodiscard]] Core::Result<std::string> passwordChangeResult(const HttpResponse &response, Core::Snowflake userId);

// Authenticator app setup. The secret is 20 random bytes as 32 base32 characters.
[[nodiscard]] std::string totpSecretFromBytes(const std::vector<unsigned char> &bytes);
// The secret in groups of four, as authenticator apps show it when typed in.
[[nodiscard]] std::string groupedTotpSecret(std::string_view secret);
// otpauth:// URI an authenticator app can import, labelled with the account name.
[[nodiscard]] std::string totpUri(std::string_view secret, std::string_view accountName);
// A six-digit code as typed, with spaces removed; empty when it isn't six digits.
[[nodiscard]] std::string normalizedTotpCode(std::string_view typed);

// POST /users/@me/mfa/totp/enable. Without a secret it only checks the password.
[[nodiscard]] nlohmann::json totpEnableBody(const std::string &password, const std::string &secret = {},
                                            const std::string &code = {});
// The password check succeeds when Discord answers with TotpPasswordAcceptedCode.
[[nodiscard]] Core::Result<void> totpPasswordCheckResult(const HttpResponse &response);

// A backup code; consumed ones have already been used to sign in.
struct BackupCode
{
    std::string code;
    bool consumed = false;
};

struct TotpEnabled
{
    std::string token;
    // Shown once after setup; never stored or logged.
    std::vector<BackupCode> backupCodes;
};
// Enabling replaces the session token; reject one for another user.
[[nodiscard]] Core::Result<TotpEnabled> totpEnableResult(const HttpResponse &response, Core::Snowflake userId);

// Discord's request to confirm an action before it runs. Accounts without a second factor
// confirm with their password; others with an authenticator or backup code.
struct MfaRequirement
{
    std::string ticket;
    bool totp = false;
    bool backup = false;
    bool password = false;
};
[[nodiscard]] std::optional<MfaRequirement> mfaChallenge(const HttpResponse &response);
// What to send to POST /mfa/finish: the method name and its data.
struct MfaProof
{
    std::string method;
    std::string data;
};
// Six digits prove as "totp"; anything else as a backup code, lowercased without spaces or dashes.
[[nodiscard]] MfaProof mfaCodeProof(std::string_view code);
[[nodiscard]] MfaProof mfaPasswordProof(const std::string &password);
[[nodiscard]] nlohmann::json mfaFinishBody(const std::string &ticket, const MfaProof &proof);
// The short-lived verification to send as X-Discord-MFA-Authorization when retrying.
[[nodiscard]] Core::Result<std::string> mfaFinishResult(const HttpResponse &response);

// Removing the authenticator replaces the session token as well.
[[nodiscard]] Core::Result<std::string> totpDisableResult(const HttpResponse &response, Core::Snowflake userId);

// Viewing backup codes later: the password makes Discord email a verification key and return one
// single-use nonce for viewing and one for generating new codes. Each lasts 30 minutes.
struct BackupCodeNonces
{
    std::string view;
    std::string regenerate;
};
[[nodiscard]] Core::Result<BackupCodeNonces> backupCodesChallengeResult(const HttpResponse &response);
// POST /users/@me/mfa/codes-verification with the emailed key and the nonce for the action.
[[nodiscard]] nlohmann::json backupCodesBody(const std::string &key, const std::string &nonce, bool regenerate);
[[nodiscard]] Core::Result<std::vector<BackupCode>> backupCodesResult(const HttpResponse &response);

} // namespace Styx::Discord
