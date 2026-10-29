#pragma once

#include "CaptchaResolver.hpp"
#include "ClientIdentity.hpp"
#include "HttpClient.hpp"
#include "Core/ProxyConfig.hpp"
#include "Platform/Trackable.hpp"
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace Styx::Discord {

enum class LoginError { Network, InvalidCredentials, CaptchaRequired, InvalidMfaCode, TooManyRequests, AccountLocked, Suspended, RequiresVerification, PasskeyOnly, Unknown };
enum class MfaMethod { Totp, Sms, Backup };
struct MfaChallenge { bool totp = false; bool sms = false; bool backup = false; bool webauthn = false; };

class LoginClient : public Platform::Trackable {
public:
    explicit LoginClient(const Core::ProxyConfig &proxy, CaptchaResolver *captchaResolver = nullptr);
    void login(const std::string &login, const std::string &password);
    void submitMfa(MfaMethod method, const std::string &code);
    void requestSmsCode();
    [[nodiscard]] bool canRetryMfa() const { return !mfaTicket.empty(); }
    Platform::Signal<const MfaChallenge &> mfaRequired;
    Platform::Signal<const std::string &> authenticated;
    Platform::Signal<LoginError> failed;
    Platform::Signal<bool> smsSent;
private:
    void fetchFingerprintThen(uint64_t generation, std::function<void()> next);
    void postLogin(uint64_t generation, const std::string &login, const std::string &password);
    void handleLoginResponse(const HttpResponse &response);
    void handleMfaResponse(const HttpResponse &response);
    [[nodiscard]] std::optional<LoginError> commonFailure(const HttpResponse &response, const nlohmann::json &root) const;
    ClientIdentity identity;
    HttpClient http;
    bool haveFingerprint = false;
    uint64_t generation = 0;
    std::string mfaTicket;
    std::string mfaLoginInstanceId;
};
} // namespace Styx::Discord
