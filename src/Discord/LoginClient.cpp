#include "LoginClient.hpp"
#include "Core/Logging.hpp"

namespace Styx::Discord {
namespace {
using Json = nlohmann::json;
Json parseObject(const std::string &body) {
    auto value = Json::parse(body, nullptr, false);
    return value.is_object() ? value : Json::object();
}
std::string str(const Json &obj, const char *key) {
    auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string();
}
bool boolean(const Json &obj, const char *key) {
    auto it = obj.find(key);
    return it != obj.end() && it->is_boolean() && it->get<bool>();
}
int number(const Json &obj, const char *key) {
    auto it = obj.find(key);
    return it != obj.end() && it->is_number() ? it->get<int>() : 0;
}
std::string serverErrorMessage(const Json &root) {
    auto errors = root.find("errors");
    if (errors != root.end() && errors->is_object()) {
        for (const char *field : {"login", "email", "password", "code"}) {
            auto f = errors->find(field);
            if (f == errors->end() || !f->is_object()) continue;
            auto e = f->find("_errors");
            if (e != f->end() && e->is_array() && !e->empty() && (*e)[0].is_object())
                return str((*e)[0], "message");
        }
    }
    return str(root, "message");
}
const char *mfaEndpoint(MfaMethod method) {
    switch (method) {
    case MfaMethod::Totp: return "/auth/mfa/totp";
    case MfaMethod::Sms: return "/auth/mfa/sms";
    case MfaMethod::Backup: return "/auth/mfa/backup";
    }
    return "";
}
}

LoginClient::LoginClient(const Core::ProxyConfig &proxy, CaptchaResolver *resolver)
    : http("https://discord.com/api/v9", "", identity, proxy, resolver) {
    http.setReferer("https://discord.com/login");
}
void LoginClient::login(const std::string &login, const std::string &password) {
    mfaTicket.clear(); mfaLoginInstanceId.clear();
    if (haveFingerprint) postLogin(login, password);
    else fetchFingerprintThen([this, login, password] { postLogin(login, password); });
}
void LoginClient::fetchFingerprintThen(std::function<void()> next) {
    Platform::Guard<LoginClient> self(this);
    http.get("/experiments", {{"with_guild_experiments", "true"}}, [self, next = std::move(next)](const HttpResponse &response) {
        if (!self) return;
        const auto fingerprint = str(parseObject(response.body), "fingerprint");
        if (response.success && !fingerprint.empty()) {
            self->http.setFingerprint(fingerprint); self->haveFingerprint = true;
        } else Core::log(Core::LogLevel::Warning, LogDiscord) << "Login: fingerprint fetch failed, HTTP " << response.statusCode << " " << response.error;
        next();
    });
}
void LoginClient::postLogin(const std::string &login, const std::string &password) {
    Json body = {{"login", login}, {"password", password}, {"undelete", false}, {"login_source", nullptr}, {"gift_code_sku_id", nullptr}};
    Platform::Guard<LoginClient> self(this);
    http.post("/auth/login", body, [self](const HttpResponse &r) { if (self) self->handleLoginResponse(r); });
}
void LoginClient::handleLoginResponse(const HttpResponse &response) {
    const auto root = parseObject(response.body);
    if (response.success) {
        const auto token = str(root, "token");
        if (!token.empty()) { authenticated.publish(token); return; }
        if (!boolean(root, "mfa")) {
            Core::log(Core::LogLevel::Warning, LogDiscord) << "Login: success response without token";
            failed.publish(LoginError::Unknown); return;
        }
        MfaChallenge challenge;
        challenge.totp = boolean(root, "totp"); challenge.sms = boolean(root, "sms"); challenge.backup = boolean(root, "backup");
        auto webauthn = root.find("webauthn"); challenge.webauthn = webauthn != root.end() && webauthn->is_string();
        if (!challenge.totp && !challenge.sms && !challenge.backup) {
            Core::log(Core::LogLevel::Warning, LogDiscord) << "Login: no supported MFA method offered";
            failed.publish(challenge.webauthn ? LoginError::PasskeyOnly : LoginError::Unknown); return;
        }
        mfaTicket = str(root, "ticket"); mfaLoginInstanceId = str(root, "login_instance_id");
        mfaRequired.publish(challenge); return;
    }
    if (auto error = commonFailure(response, root)) { failed.publish(*error); return; }
    Core::log(Core::LogLevel::Warning, LogDiscord) << "Login failed, HTTP " << response.statusCode << " " << serverErrorMessage(root);
    switch (number(root, "code")) {
    case 20011: case 20013: failed.publish(LoginError::AccountLocked); return;
    case 70007: failed.publish(LoginError::RequiresVerification); return;
    default: break;
    }
    failed.publish(response.statusCode == 400 ? LoginError::InvalidCredentials : LoginError::Unknown);
}
void LoginClient::submitMfa(MfaMethod method, const std::string &code) {
    if (mfaTicket.empty() || code.empty()) return;
    Json body = {{"code", code}, {"ticket", mfaTicket}, {"login_source", nullptr}, {"gift_code_sku_id", nullptr}, {"login_instance_id", mfaLoginInstanceId.empty() ? Json(nullptr) : Json(mfaLoginInstanceId)}};
    Platform::Guard<LoginClient> self(this);
    http.post(mfaEndpoint(method), body, [self](const HttpResponse &r) { if (self) self->handleMfaResponse(r); });
}
void LoginClient::handleMfaResponse(const HttpResponse &response) {
    const auto root = parseObject(response.body);
    if (response.success) {
        const auto token = str(root, "token");
        if (token.empty()) { Core::log(Core::LogLevel::Warning, LogDiscord) << "Login: MFA response without token"; failed.publish(LoginError::Unknown); return; }
        authenticated.publish(token); return;
    }
    auto error = commonFailure(response, root);
    if (!error) {
        Core::log(Core::LogLevel::Warning, LogDiscord) << "MFA failed, HTTP " << response.statusCode << " " << serverErrorMessage(root);
        error = number(root, "code") == 60008 ? LoginError::InvalidMfaCode : LoginError::Unknown;
    }
    if (*error != LoginError::InvalidMfaCode && *error != LoginError::Network) mfaTicket.clear();
    failed.publish(*error);
}
void LoginClient::requestSmsCode() {
    if (mfaTicket.empty()) return;
    Platform::Guard<LoginClient> self(this);
    http.post("/auth/mfa/sms/send", Json{{"ticket", mfaTicket}}, [self](const HttpResponse &response) {
        if (!self) return;
        if (!response.success) Core::log(Core::LogLevel::Warning, LogDiscord) << "Login: SMS send failed, HTTP " << response.statusCode << " " << response.error;
        self->smsSent.publish(response.success);
    });
}
std::optional<LoginError> LoginClient::commonFailure(const HttpResponse &response, const Json &root) const {
    if (response.statusCode == 0) { Core::log(Core::LogLevel::Warning, LogNetwork) << "Login request failed: " << response.error; return LoginError::Network; }
    if (CaptchaChallenge::fromResponseBody(response.body)) return LoginError::CaptchaRequired;
    if (response.rateLimited()) { Core::log(Core::LogLevel::Warning, LogNetwork) << "Login rate limited, retry after " << number(root, "retry_after"); return LoginError::TooManyRequests; }
    if (root.contains("suspended_user_token")) return LoginError::Suspended;
    return std::nullopt;
}
} // namespace Styx::Discord
