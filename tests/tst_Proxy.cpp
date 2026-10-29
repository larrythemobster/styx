#include "Core/ImageManager.hpp"
#include "Core/ProxyConfig.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Voice/Socks5UdpAssociation.hpp"

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

using Styx::Core::ImageManager;
using Styx::Core::ProxyConfig;
using Styx::Core::Snowflake;
using Styx::Discord::Voice::Socks5UdpAssociation;

namespace {
ProxyConfig make(ProxyConfig::Type type, std::string host, std::uint16_t port,
                 std::string username = {}, std::string password = {})
{
    return {type, std::move(host), port, std::move(username), std::move(password)};
}

std::string datagram(unsigned char fragment, unsigned char type, const std::string &address,
                     std::uint16_t port, const std::string &payload)
{
    std::string result{char(0), char(0), char(fragment), char(type)};
    result += address;
    result += char(port >> 8);
    result += char(port & 0xff);
    return result + payload;
}
}

TEST_CASE("Proxy parsing accepts supported URLs", "[proxy]")
{
    using T = ProxyConfig::Type;
    const std::vector<std::pair<std::string, ProxyConfig>> cases = {
        {"http://proxy.test:8080", make(T::Http, "proxy.test", 8080)},
        {"https://proxy.test:8443", make(T::Https, "proxy.test", 8443)},
        {"socks5://proxy.test:1080", make(T::Socks5, "proxy.test", 1080)},
        {"socks5h://proxy.test:1080", make(T::Socks5, "proxy.test", 1080)},
        {"socks://proxy.test:1080", make(T::Socks5, "proxy.test", 1080)},
        {"SOCKS5://proxy.test:1080", make(T::Socks5, "proxy.test", 1080)},
        {"  socks5://proxy.test:1080  ", make(T::Socks5, "proxy.test", 1080)},
        {"socks5://127.0.0.1:1080", make(T::Socks5, "127.0.0.1", 1080)},
        {"socks5://[::1]:1080", make(T::Socks5, "::1", 1080)},
        {"socks5://proxy.test:1", make(T::Socks5, "proxy.test", 1)},
        {"socks5://proxy.test:65535", make(T::Socks5, "proxy.test", 65535)},
        {"socks5://proxy.test:1080/", make(T::Socks5, "proxy.test", 1080)},
        {"http://proxy.test:8080/proxy.pac", make(T::Http, "proxy.test", 8080)},
        {"socks5://PROXY.TEST:1080", make(T::Socks5, "proxy.test", 1080)},
        {"socks5://user:pass@proxy.test:1080", make(T::Socks5, "proxy.test", 1080, "user", "pass")},
        {"socks5://us%3Aer:p%40ss%2F1@proxy.test:1080", make(T::Socks5, "proxy.test", 1080, "us:er", "p@ss/1")},
        {"http://user@proxy.test:8080", make(T::Http, "proxy.test", 8080, "user")},
    };
    for (const auto &[input, expected] : cases) {
        INFO(input);
        const auto parsed = ProxyConfig::parse(input);
        REQUIRE(parsed.has_value());
        CHECK(*parsed == expected);
        CHECK(parsed->enabled());
    }
}

TEST_CASE("Proxy parsing fails closed and empty input means direct", "[proxy]")
{
    for (const std::string input : {"socks4://proxy.test:1080", "socks4a://proxy.test:1080", "ftp://proxy.test:21", "file:///etc/passwd", "javascript:alert(1)", "proxy.test:1080", "://proxy.test:1080", "socks5://proxy.test", "socks5://proxy.test:0", "socks5://proxy.test:65536", "socks5://proxy.test:-1", "socks5://proxy.test:http", "socks5://:1080", "socks5://", "my proxy is at home", "proxy.test"}) {
        INFO(input);
        CHECK_FALSE(ProxyConfig::parse(input).has_value());
    }
    for (const std::string input : {"", "   ", "\t\n"}) {
        const auto parsed = ProxyConfig::parse(input);
        REQUIRE(parsed.has_value());
        CHECK(parsed->type == ProxyConfig::Type::None);
        CHECK_FALSE(parsed->enabled());
        CHECK(parsed->toCurlUrl().empty());
    }
}

TEST_CASE("Proxy configuration invariants", "[proxy]")
{
    using T = ProxyConfig::Type;
    CHECK(make(T::Socks5, "h", 1080).enabled());
    CHECK_FALSE(ProxyConfig{}.enabled());
    CHECK_FALSE(make(T::None, "h", 1080).enabled());
    CHECK_FALSE(make(T::Socks5, "", 1080).enabled());
    CHECK_FALSE(make(T::Socks5, "h", 0).enabled());
    CHECK(ProxyConfig{}.canRelayUdp());
    CHECK(make(T::Socks5, "h", 1080).canRelayUdp());
    CHECK_FALSE(make(T::Http, "h", 8080).canRelayUdp());
    CHECK_FALSE(make(T::Https, "h", 8443).canRelayUdp());
    CHECK(make(T::Http, "h", 0).canRelayUdp());
    const auto base = make(T::Socks5, "h", 1080, "user", "pass");
    CHECK(base == make(T::Socks5, "h", 1080, "user", "pass"));
    CHECK_FALSE(base == make(T::Http, "h", 1080, "user", "pass"));
    CHECK_FALSE(base == make(T::Socks5, "other", 1080, "user", "pass"));
    CHECK_FALSE(base == make(T::Socks5, "h", 1081, "user", "pass"));
    CHECK_FALSE(base == make(T::Socks5, "h", 1080, "other", "pass"));
    CHECK_FALSE(base == make(T::Socks5, "h", 1080, "user", "other"));
}

TEST_CASE("Proxy formatting protects credentials and resolves DNS remotely", "[proxy]")
{
    using T = ProxyConfig::Type;
    for (const auto &[type, scheme, port] : std::vector<std::tuple<T, std::string, std::uint16_t>>{{T::Socks5, "socks5h", 1080}, {T::Http, "http", 8080}, {T::Https, "https", 8443}}) {
        const auto config = make(type, "proxy.test", port, "user", "secret");
        const auto expected = scheme + "://proxy.test:" + std::to_string(port);
        CHECK(config.toCurlUrl() == expected);
        CHECK(config.toCurlProxyString().find("user:secret@") != std::string::npos);
        CHECK(config.toDisplayString().find("secret") == std::string::npos);
        CHECK(config.toDisplayString().find('@') == std::string::npos);
        CHECK(config.toDisplayString() == (type == T::Socks5 ? "socks5" : scheme) + "://proxy.test:" + std::to_string(port));
        CHECK(make(type, "proxy.test", port).toCurlProxyString() == expected);
    }
    const auto ipv6 = make(T::Socks5, "::1", 1080, "user", "pass");
    CHECK(ipv6.toCurlUrl() == "socks5h://[::1]:1080");
    CHECK(ipv6.toDisplayString() == "socks5://[::1]:1080");
    CHECK(ipv6.toString() == "socks5://user:pass@[::1]:1080");
    const auto parsed = ProxyConfig::parse(ipv6.toString());
    REQUIRE(parsed.has_value());
    CHECK(*parsed == ipv6);
    const ProxyConfig direct;
    CHECK(direct.toCurlUrl().empty());
    CHECK(direct.toCurlProxyString().empty());
    CHECK(direct.toString().empty());
    CHECK(direct.toDisplayString().empty());
    CHECK(make(T::Socks5, "", 1080).toCurlUrl().empty());
    CHECK(make(T::Socks5, "h", 0).toCurlProxyString().empty());
}

TEST_CASE("Proxy storage round trips all host and credential forms", "[proxy]")
{
    using T = ProxyConfig::Type;
    for (const auto type : {T::Http, T::Https, T::Socks5})
        for (const std::string host : {"proxy.test", "a.b.c.example.com", "127.0.0.1", "::1", "2001:db8::dead:beef"})
            for (const auto &[user, password] : std::vector<std::pair<std::string, std::string>>{{"", ""}, {"user", "pass"}, {"user", ""}, {"us:er", "p@ss/w?rd#1"}, {"us er", "pa ss"}, {"ユーザー", "пароль"}, {"us%er", "p%40ss"}}) {
                const auto config = make(type, host, 1080, user, password);
                const auto stored = config.toString();
                INFO(stored);
                const auto reloaded = ProxyConfig::parse(stored);
                REQUIRE(reloaded.has_value());
                CHECK(*reloaded == config);
                CHECK(reloaded->enabled());
                CHECK(reloaded->toString() == stored);
            }
}

TEST_CASE("Image routes remain isolated per account", "[proxy]")
{
    using T = ProxyConfig::Type;
    ImageManager images;
    const Snowflake first(1), second(2);
    const auto config = make(T::Socks5, "first.test", 1080, "user", "pass");
    images.setAccountProxy(first, config);
    CHECK(images.hasRouteFor(first));
    CHECK_FALSE(images.hasRouteFor(Snowflake()));
    CHECK(images.accountProxy(first) == config);
    CHECK(images.curlProxyFor(first) == config.toCurlProxyString());
    images.setAccountProxy(second, make(T::Http, "second.test", 8080));
    CHECK(images.curlProxyFor(second) == "http://second.test:8080");
    CHECK_FALSE(images.hasRouteFor(Snowflake(3)));
    images.setAccountProxy(first, config);
    CHECK(images.curlProxyFor(first) == config.toCurlProxyString());
    images.setAccountProxy(first, make(T::Socks5, "new.test", 1080));
    CHECK(images.curlProxyFor(first) == "socks5h://new.test:1080");
    CHECK(images.curlProxyFor(second) == "http://second.test:8080");
    images.setAccountProxy(first, ProxyConfig{});
    CHECK(images.curlProxyFor(first).empty());
    CHECK_FALSE(images.accountProxy(first).enabled());
    for (const auto &incomplete : {make(T::Socks5, "proxy.test", 0), make(T::Socks5, "", 1080)}) {
        images.setAccountProxy(first, incomplete);
        CHECK(images.curlProxyFor(first).empty());
        CHECK_FALSE(images.accountProxy(first).enabled());
    }
}

TEST_CASE("SOCKS5 UDP framing addresses exact destinations", "[proxy][udp]")
{
    const auto ipv4 = Socks5UdpAssociation::encapsulate("audio", "1.2.3.4", 0x1234);
    CHECK(ipv4 == std::string("\x00\x00\x00\x01\x01\x02\x03\x04\x12\x34", 10) + "audio");
    const auto ipv6 = Socks5UdpAssociation::encapsulate("x", "2001:db8::1", 443);
    CHECK(ipv6 == std::string("\x00\x00\x00\x04\x20\x01\x0d\xb8\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x01\xbb", 22) + "x");
    for (const auto &[address, port] : std::vector<std::pair<std::string, std::uint16_t>>{{"104.29.156.117", 1}, {"104.29.156.117", 19334}, {"255.255.255.255", 65535}, {"0.0.0.0", 443}, {"::1", 19334}, {"2001:db8:85a3::8a2e:370:7334", 50000}}) {
        INFO(address);
        const auto packet = Socks5UdpAssociation::encapsulate("payload", address, port);
        const bool isV6 = address.find(':') != std::string::npos;
        const std::size_t addressSize = isV6 ? 16 : 4;
        REQUIRE(packet.size() == addressSize + 6 + 7);
        CHECK(packet.substr(0, 3) == std::string(3, '\0'));
        CHECK(static_cast<unsigned char>(packet[3]) == (isV6 ? 4 : 1));
        char decoded[INET6_ADDRSTRLEN]{};
        REQUIRE(inet_ntop(isV6 ? AF_INET6 : AF_INET, packet.data() + 4, decoded, sizeof(decoded)) != nullptr);
        CHECK(std::string(decoded) == address);
        CHECK(static_cast<unsigned char>(packet[4 + addressSize]) == (port >> 8));
        CHECK(static_cast<unsigned char>(packet[5 + addressSize]) == (port & 0xff));
        CHECK(packet.substr(6 + addressSize) == "payload");
    }
    CHECK(Socks5UdpAssociation::encapsulate("x", "", 19334).empty());
}

TEST_CASE("SOCKS5 UDP preserves payloads and rejects malformed packets", "[proxy][udp]")
{
    const std::vector<std::string> payloads = {"", std::string("\x80\x78\x00\x01", 4), std::string(64, '\0'), std::string(64, char(0xff)), std::string("\x00\x00\x00\x01\x7f\x00\x00\x01\x04\x38", 10), std::string(1400, 'a')};
    for (const auto &payload : payloads)
        for (const std::string address : {"8.8.8.8", "2001:db8::1"}) {
            const auto packet = Socks5UdpAssociation::encapsulate(payload, address, 19334);
            const std::size_t header = address.find(':') == std::string::npos ? 10 : 22;
            REQUIRE(packet.size() == header + payload.size());
            CHECK(packet.substr(header) == payload);
            CHECK(Socks5UdpAssociation::decapsulate(packet) == payload);
            for (std::size_t length = 0; length < packet.size(); ++length) {
                INFO(address << " prefix " << length);
                const auto result = Socks5UdpAssociation::decapsulate(packet.substr(0, length));
                CHECK(result.size() <= (length > header ? length - header : 0));
            }
        }
    const std::string domain = std::string(1, char(11)) + "example.com";
    const std::string payload("\x80\x78opus", 6);
    CHECK(Socks5UdpAssociation::decapsulate(datagram(0, 3, domain, 19334, payload)) == payload);
    const std::vector<std::string> malformed = {"", std::string(1, '\0'), std::string(9, '\0'), datagram(0, 9, std::string(4, '\0'), 1, "x"), datagram(0, 0, std::string(4, '\0'), 1, "x"), std::string("\x00\x00\x00\x04", 4) + std::string(8, '\0'), std::string("\x00\x00\x00\x03", 4) + char(200) + "example.com", std::string("\x00\x00\x00\x03", 4) + char(8) + "hostname"};
    for (const auto &packet : malformed)
        CHECK(Socks5UdpAssociation::decapsulate(packet).empty());
    for (unsigned char fragment : {1, 3, 0x80})
        CHECK(Socks5UdpAssociation::decapsulate(datagram(fragment, 1, std::string("\x01\x02\x03\x04", 4), 19334, "payload")).empty());
}
