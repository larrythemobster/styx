#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace Styx::Core {
struct ProxyConfig
{
    enum class Type { None, Http, Https, Socks5 };
    Type type = Type::None;
    std::string host;
    std::uint16_t port = 0;
    std::string username;
    std::string password;

    [[nodiscard]] bool enabled() const noexcept { return type != Type::None && !host.empty() && port != 0; }
    [[nodiscard]] bool canRelayUdp() const noexcept { return !enabled() || type == Type::Socks5; }
    [[nodiscard]] std::string toCurlUrl() const;
    [[nodiscard]] std::string toCurlProxyString() const;
    [[nodiscard]] std::string toString() const;
    [[nodiscard]] std::string toDisplayString() const;
    static std::optional<ProxyConfig> parse(const std::string &text);
    bool operator==(const ProxyConfig &) const = default;
};
}
