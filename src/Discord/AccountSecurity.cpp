#include "AccountSecurity.hpp"
#include "ApiError.hpp"
#include "HttpClient.hpp"
#include "Core/TokenUtils.hpp"

#include <cctype>
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

namespace {
// A non-empty token for this account from a JSON object's "token" field.
Core::Result<std::string> sessionToken(const nlohmann::json &body, Core::Snowflake userId)
{
    const auto token = body.is_object() ? body.find("token") : body.end();
    if (!body.is_object() || token == body.end() || !token->is_string() || token->get_ref<const std::string &>().empty())
        return Core::Result<std::string>::makeError("Discord made the change but didn't return a new sign-in. Sign in again.");
    auto value = token->get<std::string>();
    if (Core::TokenUtils::getIdAndCheckToken(value) != userId)
        return Core::Result<std::string>::makeError("Discord returned a sign-in for a different account. Sign in again.");
    return Core::Result<std::string>::makeOk(std::move(value));
}

template <typename T>
Core::Result<T> apiFailure(const HttpResponse &response)
{
    auto error = ApiError::fromResponse(response);
    return Core::Result<T>::makeError(std::move(error.message), error.code);
}
}

std::string totpSecretFromBytes(const std::vector<unsigned char> &bytes)
{
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::string out;
    out.reserve((bytes.size() * 8 + 4) / 5);
    unsigned buffer = 0;
    int bits = 0;
    for (const unsigned char byte : bytes) {
        buffer = (buffer << 8) | byte;
        bits += 8;
        while (bits >= 5) {
            out += alphabet[(buffer >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits > 0) out += alphabet[(buffer << (5 - bits)) & 31];
    return out;
}

std::string groupedTotpSecret(std::string_view secret)
{
    std::string out;
    out.reserve(secret.size() + secret.size() / 4);
    for (std::size_t i = 0; i < secret.size(); ++i) {
        if (i && i % 4 == 0) out += ' ';
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(secret[i])));
    }
    return out;
}

std::string totpUri(std::string_view secret, std::string_view accountName)
{
    std::string label;
    for (const char c : accountName) {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte) || c == '-' || c == '.' || c == '_' || c == '~') {
            label += c;
        } else {
            constexpr char hex[] = "0123456789ABCDEF";
            label += '%';
            label += hex[byte >> 4];
            label += hex[byte & 15];
        }
    }
    return "otpauth://totp/Discord:" + label + "?secret=" + std::string(secret) + "&issuer=Discord";
}

std::string normalizedTotpCode(std::string_view typed)
{
    std::string code;
    for (const char c : typed) {
        if (c == ' ') continue;
        if (!std::isdigit(static_cast<unsigned char>(c))) return {};
        code += c;
    }
    return code.size() == 6 ? code : std::string();
}

nlohmann::json totpEnableBody(const std::string &password, const std::string &secret, const std::string &code)
{
    nlohmann::json body = {{"password", password}};
    if (!secret.empty()) body["secret"] = secret;
    if (!code.empty()) body["code"] = code;
    return body;
}

Core::Result<void> totpPasswordCheckResult(const HttpResponse &response)
{
    if (response.success)
        return Core::Result<void>::makeError("Discord didn't ask for an authenticator code. Try again.");
    auto error = ApiError::fromResponse(response);
    if (error.code == TotpPasswordAcceptedCode)
        return Core::Result<void>::makeOk();
    return Core::Result<void>::makeError(std::move(error.message), error.code);
}

Core::Result<TotpEnabled> totpEnableResult(const HttpResponse &response, Core::Snowflake userId)
{
    if (!response.success) {
        auto failure = apiFailure<TotpEnabled>(response);
        // The password confirmation lasts five minutes.
        if (failure.code == MfaRequiredCode) failure.error = "Your password confirmation expired. Cancel and start again.";
        return failure;
    }
    const auto body = nlohmann::json::parse(response.body, nullptr, false);
    auto token = sessionToken(body, userId);
    if (!token.success()) return Core::Result<TotpEnabled>::makeError(std::move(token.error));
    TotpEnabled enabled{std::move(*token.value), {}};
    if (const auto codes = body.find("backup_codes"); codes != body.end() && codes->is_array())
        for (const auto &entry : *codes)
            if (entry.is_object() && entry.contains("code") && entry["code"].is_string())
                enabled.backupCodes.push_back(entry["code"].get<std::string>());
    return Core::Result<TotpEnabled>::makeOk(std::move(enabled));
}

std::optional<MfaRequirement> mfaChallenge(const HttpResponse &response)
{
    if (response.success) return std::nullopt;
    const auto body = nlohmann::json::parse(response.body, nullptr, false);
    if (!body.is_object() || body.value("code", 0) != MfaRequiredCode) return std::nullopt;
    const auto mfa = body.find("mfa");
    if (mfa == body.end() || !mfa->is_object()) return std::nullopt;
    const auto ticket = mfa->find("ticket");
    if (ticket == mfa->end() || !ticket->is_string() || ticket->get_ref<const std::string &>().empty()) return std::nullopt;
    MfaRequirement challenge{ticket->get<std::string>()};
    if (const auto methods = mfa->find("methods"); methods != mfa->end() && methods->is_array())
        for (const auto &method : *methods) {
            if (!method.is_object() || !method.contains("type") || !method["type"].is_string()) continue;
            const auto type = method["type"].get<std::string>();
            if (type == "totp") {
                challenge.totp = true;
                challenge.backup = challenge.backup || method.value("backup_codes_allowed", false);
            } else if (type == "backup") {
                challenge.backup = true;
            } else if (type == "password") {
                challenge.password = true;
            }
        }
    return challenge;
}

MfaProof mfaCodeProof(std::string_view code)
{
    std::string data;
    for (const char c : code)
        if (c != ' ' && c != '-') data += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool digits = data.size() == 6 && normalizedTotpCode(data) == data;
    return {digits ? "totp" : "backup", std::move(data)};
}

MfaProof mfaPasswordProof(const std::string &password)
{
    return {"password", password};
}

nlohmann::json mfaFinishBody(const std::string &ticket, const MfaProof &proof)
{
    return {{"ticket", ticket}, {"mfa_type", proof.method}, {"data", proof.data}};
}

Core::Result<std::string> mfaFinishResult(const HttpResponse &response)
{
    if (!response.success) return apiFailure<std::string>(response);
    const auto body = nlohmann::json::parse(response.body, nullptr, false);
    const auto token = body.is_object() ? body.find("token") : body.end();
    if (!body.is_object() || token == body.end() || !token->is_string() || token->get_ref<const std::string &>().empty())
        return Core::Result<std::string>::makeError("Discord didn't confirm the code. Try again.");
    return Core::Result<std::string>::makeOk(token->get<std::string>());
}

Core::Result<std::string> totpDisableResult(const HttpResponse &response, Core::Snowflake userId)
{
    if (!response.success) return apiFailure<std::string>(response);
    return sessionToken(nlohmann::json::parse(response.body, nullptr, false), userId);
}

} // namespace Styx::Discord
