#include "Socks5UdpAssociation.hpp"
#include "Core/Logging.hpp"

#include "Platform/Dispatcher.hpp"
#include <functional>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using native_socket_t = SOCKET;
constexpr native_socket_t kInvalidNativeSocket = INVALID_SOCKET;
inline int closeNativeSocket(native_socket_t s) { return ::closesocket(s); }
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
using native_socket_t = int;
constexpr native_socket_t kInvalidNativeSocket = -1;
inline int closeNativeSocket(native_socket_t s) { return ::close(s); }
#endif

#include <algorithm>
#include <vector>

namespace Styx {
namespace Discord {
namespace Voice {

namespace {

constexpr char SOCKS_VERSION = 0x05;
constexpr char AUTH_VERSION = 0x01;
constexpr char METHOD_NONE = 0x00;
constexpr char METHOD_USERPASS = 0x02;
constexpr char METHOD_UNACCEPTABLE = char(0xFF);
constexpr char CMD_UDP_ASSOCIATE = 0x03;
constexpr std::uint8_t ATYP_IPV4 = 0x01;
constexpr std::uint8_t ATYP_DOMAIN = 0x03;
constexpr std::uint8_t ATYP_IPV6 = 0x04;
constexpr char REPLY_SUCCEEDED = 0x00;

void appendAddress(std::string &out, const std::string &destination, std::uint16_t port)
{
    const std::string &destStr = destination;
    in_addr addr4{};
    in6_addr addr6{};

    if (inet_pton(AF_INET, destStr.c_str(), &addr4) == 1) {
        out.push_back(char(ATYP_IPV4));
        out.append(reinterpret_cast<const char *>(&addr4), 4);
    } else if (inet_pton(AF_INET6, destStr.c_str(), &addr6) == 1) {
        out.push_back(char(ATYP_IPV6));
        out.append(reinterpret_cast<const char *>(&addr6), 16);
    } else {
        std::string domainUtf8 = destination;
        if (!domainUtf8.empty() && domainUtf8.size() <= 255) {
            out.push_back(char(ATYP_DOMAIN));
            out.push_back(static_cast<char>(domainUtf8.size()));
            out.append(domainUtf8);
        } else {
            return;
        }
    }

    std::uint16_t portBE = htons(port);
    out.append(reinterpret_cast<const char *>(&portBE), 2);
}

int addressFieldLength(const std::string &data, int offset)
{
    if (data.size() <= offset)
        return -1;

    switch (static_cast<std::uint8_t>(data.at(offset))) {
    case ATYP_IPV4:
        return 1 + 4 + 2;
    case ATYP_IPV6:
        return 1 + 16 + 2;
    case ATYP_DOMAIN:
        if (data.size() <= offset + 1)
            return -1;
        return 1 + 1 + static_cast<std::uint8_t>(data.at(offset + 1)) + 2;
    default:
        return -1;
    }
}

std::string replyErrorString(std::uint8_t code)
{
    switch (code) {
    case 0x01:
        return ("general SOCKS server failure");
    case 0x02:
        return ("connection not allowed by ruleset");
    case 0x03:
        return ("network unreachable");
    case 0x04:
        return ("host unreachable");
    case 0x05:
        return ("connection refused");
    case 0x06:
        return ("TTL expired");
    case 0x07:
        return ("command not supported");
    case 0x08:
        return ("address type not supported");
    default:
        return "unknown error " + std::to_string(code);
    }
}

int recvExact(native_socket_t sock, char *buf, int total)
{
    int received = 0;
    while (received < total) {
        int n = ::recv(sock, buf + received, total - received, 0);
        if (n <= 0)
            return -1;
        received += n;
    }
    return received;
}

} // namespace

Socks5UdpAssociation::~Socks5UdpAssociation()
{
    close();
}

void Socks5UdpAssociation::open(const Core::ProxyConfig &config)
{
    close();

    m_closing = false;
    m_established = false;
    m_workerThread = std::thread(&Socks5UdpAssociation::runHandshake, this, config);
}

void Socks5UdpAssociation::close()
{
    m_closing = true;
    ++m_generation;
    m_established = false;

    auto control = m_controlSocket.exchange(-1);
    if (control != -1) {
        native_socket_t s = static_cast<native_socket_t>(control);
#ifdef _WIN32
        ::shutdown(s, SD_BOTH);
#else
        ::shutdown(s, SHUT_RDWR);
#endif
        closeNativeSocket(s);
    }

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }

    m_relayAddress.clear();
    m_relayPort = 0;
}

void Socks5UdpAssociation::runHandshake(Core::ProxyConfig config)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;

    std::string portStr = std::to_string(config.port);
    std::string hostStr = config.host;

    int rc = getaddrinfo(hostStr.c_str(), portStr.c_str(), &hints, &res);
    if (rc != 0 || !res) {
        fail("Failed to resolve SOCKS5 proxy host: " + config.host);
        return;
    }

    native_socket_t sock = kInvalidNativeSocket;
    for (addrinfo *ai = res; ai != nullptr; ai = ai->ai_next) {
        sock = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock == kInvalidNativeSocket)
            continue;

#ifdef _WIN32
        DWORD timeoutMs = 10000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeoutMs), sizeof(timeoutMs));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeoutMs), sizeof(timeoutMs));
#else
        timeval tv{10, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

        if (::connect(sock, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
            break;
        }

        closeNativeSocket(sock);
        sock = kInvalidNativeSocket;
    }
    freeaddrinfo(res);

    if (sock == kInvalidNativeSocket) {
        fail("Failed to connect to SOCKS5 proxy at " + config.host + ":" + std::to_string(config.port));
        return;
    }

    m_controlSocket = static_cast<intptr_t>(sock);
    if (m_closing) {
        auto control = m_controlSocket.exchange(-1);
        if (control != -1) closeNativeSocket(static_cast<native_socket_t>(control));
        return;
    }

    // 1. Send greeting
    std::vector<char> greeting = {SOCKS_VERSION, 1, METHOD_NONE};
    if (!config.username.empty()) {
        greeting[1] = 2;
        greeting.push_back(METHOD_USERPASS);
    }

    if (::send(sock, greeting.data(), static_cast<int>(greeting.size()), 0) <= 0) {
        fail(("Failed to send greeting to SOCKS5 proxy"));
        return;
    }

    // 2. Receive method selection
    char methodReply[2]{};
    if (recvExact(sock, methodReply, 2) != 2) {
        fail(("Failed to receive method selection from SOCKS5 proxy"));
        return;
    }

    if (methodReply[0] != SOCKS_VERSION) {
        fail(("Invalid SOCKS version received from proxy"));
        return;
    }

    if (methodReply[1] == METHOD_UNACCEPTABLE) {
        fail(("Proxy rejected all offered authentication methods"));
        return;
    }

    if (methodReply[1] == METHOD_USERPASS) {
        std::string user = config.username;
        std::string pass = config.password;
        if (user.size() > 255 || pass.size() > 255) {
            fail(("SOCKS5 credentials exceed 255 bytes"));
            return;
        }

        std::vector<char> authReq;
        authReq.push_back(AUTH_VERSION);
        authReq.push_back(static_cast<char>(user.size()));
        authReq.insert(authReq.end(), user.begin(), user.end());
        authReq.push_back(static_cast<char>(pass.size()));
        authReq.insert(authReq.end(), pass.begin(), pass.end());

        if (::send(sock, authReq.data(), static_cast<int>(authReq.size()), 0) <= 0) {
            fail(("Failed to send credentials to SOCKS5 proxy"));
            return;
        }

        char authReply[2]{};
        if (recvExact(sock, authReply, 2) != 2 || authReply[1] != 0) {
            fail(("Proxy rejected the supplied credentials"));
            return;
        }
    } else if (methodReply[1] != METHOD_NONE) {
        fail("Proxy selected unsupported authentication method " + std::to_string(int(methodReply[1])));
        return;
    }

    // 3. Send UDP ASSOCIATE request (dst 0.0.0.0:0)
    char assocReq[10] = {SOCKS_VERSION, CMD_UDP_ASSOCIATE, 0x00, static_cast<char>(ATYP_IPV4), 0, 0, 0, 0, 0, 0};
    if (::send(sock, assocReq, 10, 0) <= 0) {
        fail(("Failed to send UDP ASSOCIATE request to SOCKS5 proxy"));
        return;
    }

    // 4. Receive associate reply
    char assocHdr[4]{};
    if (recvExact(sock, assocHdr, 4) != 4) {
        fail(("Failed to receive UDP ASSOCIATE reply"));
        return;
    }

    if (assocHdr[0] != SOCKS_VERSION || assocHdr[1] != REPLY_SUCCEEDED) {
        fail("UDP ASSOCIATE failed: " + replyErrorString(static_cast<std::uint8_t>(assocHdr[1])));
        return;
    }

    std::uint8_t atyp = static_cast<std::uint8_t>(assocHdr[3]);
    int addrLen = 0;
    if (atyp == ATYP_IPV4) {
        addrLen = 4;
    } else if (atyp == ATYP_IPV6) {
        addrLen = 16;
    } else {
        fail("Proxy returned unsupported relay address type " + std::to_string(int(atyp)));
        return;
    }

    std::vector<char> addrAndPort(addrLen + 2);
    if (recvExact(sock, addrAndPort.data(), addrLen + 2) != addrLen + 2) {
        fail(("Incomplete UDP ASSOCIATE relay address received"));
        return;
    }

    char ipBuf[INET6_ADDRSTRLEN]{};
    if (atyp == ATYP_IPV4) {
        inet_ntop(AF_INET, addrAndPort.data(), ipBuf, sizeof(ipBuf));
    } else {
        inet_ntop(AF_INET6, addrAndPort.data(), ipBuf, sizeof(ipBuf));
    }
    std::uint16_t port = (static_cast<std::uint8_t>(addrAndPort[addrLen]) << 8) | static_cast<std::uint8_t>(addrAndPort[addrLen + 1]);

    m_relayAddress = std::string(ipBuf);
    m_relayPort = port;

    if (m_relayAddress.empty() || m_relayAddress == ("0.0.0.0") || m_relayAddress == ("::")) {
        sockaddr_storage peerStorage{};
        socklen_t peerLen = sizeof(peerStorage);
        if (::getpeername(sock, reinterpret_cast<sockaddr *>(&peerStorage), &peerLen) == 0) {
            char peerBuf[INET6_ADDRSTRLEN]{};
            if (peerStorage.ss_family == AF_INET) {
                inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(&peerStorage)->sin_addr, peerBuf, sizeof(peerBuf));
            } else if (peerStorage.ss_family == AF_INET6) {
                inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6 *>(&peerStorage)->sin6_addr, peerBuf, sizeof(peerBuf));
            }
            m_relayAddress = std::string(peerBuf);
        }
    }

    if (m_relayPort == 0) {
        fail(("Proxy returned an invalid relay port"));
        return;
    }

    m_established = true;
    Core::log(Core::LogLevel::Info, LogVoice) << "SOCKS5 UDP relay established at" << m_relayAddress << ":" << m_relayPort;

    post([this] { established.publish(); });

    // Keep association alive until closed
#ifdef _WIN32
    DWORD zeroTimeout = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&zeroTimeout), sizeof(zeroTimeout));
#else
    timeval tvZero{0, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tvZero, sizeof(tvZero));
#endif

    char dummy[16];
    ::recv(sock, dummy, sizeof(dummy), 0);

    if (!m_closing) {
        fail(("Proxy closed the UDP association"));
    }
}

void Socks5UdpAssociation::post(std::function<void()> callback)
{
    auto guard = Platform::Guard<Socks5UdpAssociation>(this);
    auto generation = m_generation.load();
    auto task = [guard, generation, callback = std::move(callback)] {
        if (guard && guard->m_generation.load() == generation)
            callback();
    };
    if (m_loop)
        m_loop->post(std::move(task));
    else
        Platform::Dispatcher::main().post(std::move(task));
}

void Socks5UdpAssociation::fail(const std::string &error)
{
    m_established = false;
    if (m_closing)
        return;

    Core::log(Core::LogLevel::Warning, LogVoice) << "SOCKS5 UDP association failed:" << error;
    post([this, error] { failed.publish(error); });
}

std::string Socks5UdpAssociation::encapsulate(const std::string &payload, const std::string &destination,
                                             std::uint16_t destinationPort)
{
    if (destination.empty())
        return std::string();

    std::string datagram(3, '\0');
    int before = datagram.size();
    appendAddress(datagram, destination, destinationPort);
    if (datagram.size() == before) {
        return std::string();
    }
    datagram.append(payload);
    return datagram;
}

std::string Socks5UdpAssociation::decapsulate(const std::string &datagram)
{
    if (datagram.size() < 4 || datagram.at(2) != 0)
        return std::string();

    const int tail = addressFieldLength(datagram, 3);
    if (tail < 0 || datagram.size() < 3 + tail)
        return std::string();

    return datagram.substr(3 + tail);
}

} // namespace Voice
} // namespace Discord
} // namespace Styx
