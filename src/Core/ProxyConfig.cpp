#include "ProxyConfig.hpp"

#include "Platform/Native.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <memory>
#include <string_view>
#include <curl/curl.h>

namespace Styx::Core {
namespace {
const char *curlScheme(ProxyConfig::Type type)
{
    switch (type) {
    case ProxyConfig::Type::Http: return "http";
    case ProxyConfig::Type::Https: return "https";
    case ProxyConfig::Type::Socks5: return "socks5h";
    case ProxyConfig::Type::None: return "";
    }
    return "";
}

const char *displayScheme(ProxyConfig::Type type)
{
    return type == ProxyConfig::Type::Socks5 ? "socks5" : curlScheme(type);
}

std::string authority(const std::string &host, std::uint16_t port)
{
    return (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
}

std::string credentials(const ProxyConfig &config)
{
    if (config.username.empty()) return {};
    std::string result = Platform::urlEncode(config.username);
    if (!config.password.empty()) result += ":" + Platform::urlEncode(config.password);
    return result + "@";
}

std::string part(CURLU *url, CURLUPart name, unsigned flags = 0)
{
    char *value = nullptr;
    if (curl_url_get(url, name, &value, flags) != CURLUE_OK) return {};
    std::unique_ptr<char, decltype(&curl_free)> owned(value, curl_free);
    return value;
}
}

std::string ProxyConfig::toCurlUrl() const
{
    return enabled() ? std::string(curlScheme(type)) + "://" + authority(host, port) : std::string();
}

std::string ProxyConfig::toCurlProxyString() const
{
    return enabled() ? std::string(curlScheme(type)) + "://" + credentials(*this) + authority(host, port) : std::string();
}

std::string ProxyConfig::toString() const
{
    return enabled() ? std::string(displayScheme(type)) + "://" + credentials(*this) + authority(host, port) : std::string();
}

std::string ProxyConfig::toDisplayString() const
{
    return enabled() ? std::string(displayScheme(type)) + "://" + authority(host, port) : std::string();
}

std::optional<ProxyConfig> ProxyConfig::parse(const std::string &text)
{
    auto begin = text.find_first_not_of(" \t\r\n\f\v");
    if (begin == std::string::npos) return ProxyConfig{};
    auto end = text.find_last_not_of(" \t\r\n\f\v");
    auto trimmed = text.substr(begin, end - begin + 1);
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
    if (!url || curl_url_set(url.get(), CURLUPART_URL, trimmed.c_str(), CURLU_NON_SUPPORT_SCHEME) != CURLUE_OK) return std::nullopt;

    auto scheme = part(url.get(), CURLUPART_SCHEME);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    ProxyConfig config;
    if (scheme == "http") config.type = Type::Http;
    else if (scheme == "https") config.type = Type::Https;
    else if (scheme == "socks5" || scheme == "socks5h" || scheme == "socks") config.type = Type::Socks5;
    else return std::nullopt;

    config.host = part(url.get(), CURLUPART_HOST);
    if (config.host.size() >= 2 && config.host.front() == '[' && config.host.back() == ']')
        config.host = config.host.substr(1, config.host.size() - 2);
    if (config.host.empty()) return std::nullopt;
    const auto portText = part(url.get(), CURLUPART_PORT);
    unsigned port = 0;
    auto [ptr, ec] = std::from_chars(portText.data(), portText.data() + portText.size(), port);
    if (ec != std::errc{} || ptr != portText.data() + portText.size() || port == 0 || port > 65535) return std::nullopt;
    config.port = static_cast<std::uint16_t>(port);
    config.username = part(url.get(), CURLUPART_USER, CURLU_URLDECODE);
    config.password = part(url.get(), CURLUPART_PASSWORD, CURLU_URLDECODE);
    return config;
}
} // namespace Styx::Core
