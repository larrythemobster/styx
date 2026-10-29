#pragma once

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <functional>
#include <optional>
#include <string>
#include "Core/ProxyConfig.hpp"

namespace Styx::Discord {

inline constexpr int kMaxCaptchaAttempts = 3;

struct CaptchaChallenge {
    std::string service, sitekey, sessionId, rqdata, rqtoken;
    bool shouldServeInvisible = false;
    static std::optional<CaptchaChallenge> fromResponseBody(const std::string &body);
};

struct CaptchaSolution {
    std::string token, sessionId, rqtoken;
};

inline void appendCaptchaHeaders(curl_slist *&headers, const CaptchaSolution &solution)
{
    headers = curl_slist_append(headers, ("X-Captcha-Key: " + solution.token).c_str());
    if (!solution.sessionId.empty())
        headers = curl_slist_append(headers, ("X-Captcha-Session-Id: " + solution.sessionId).c_str());
    if (!solution.rqtoken.empty())
        headers = curl_slist_append(headers, ("X-Captcha-Rqtoken: " + solution.rqtoken).c_str());
}

class CaptchaResolver {
public:
    using Callback = std::function<void(std::optional<CaptchaSolution>)>;
    virtual ~CaptchaResolver() = default;
    virtual void resolve(const CaptchaChallenge &challenge, const Core::ProxyConfig &proxy, Callback cb) = 0;
    virtual void notifyConcluded() {}
};

inline std::optional<CaptchaChallenge> CaptchaChallenge::fromResponseBody(const std::string &body)
{
    const auto root = nlohmann::json::parse(body, nullptr, false);
    if (!root.is_object() || !root.contains("captcha_key") || !root["captcha_key"].is_array())
        return std::nullopt;
    auto str = [&root](const char *key) {
        auto it = root.find(key);
        return it != root.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    CaptchaChallenge c;
    c.service = str("captcha_service");
    c.sitekey = str("captcha_sitekey");
    c.sessionId = str("captcha_session_id");
    c.rqdata = str("captcha_rqdata");
    c.rqtoken = str("captcha_rqtoken");
    auto invisible = root.find("should_serve_invisible");
    c.shouldServeInvisible = invisible != root.end() && invisible->is_boolean() && invisible->get<bool>();
    return c;
}

} // namespace Styx::Discord
