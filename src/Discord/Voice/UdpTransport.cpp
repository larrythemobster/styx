#include "UdpTransport.hpp"
#include "Core/Logging.hpp"

#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using native_socket_t = SOCKET;
constexpr native_socket_t kInvalidNativeSocket = INVALID_SOCKET;
inline int closeNativeSocket(native_socket_t s) { return ::closesocket(s); }
inline bool setNonBlocking(native_socket_t s) {
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
}
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
using native_socket_t = int;
constexpr native_socket_t kInvalidNativeSocket = -1;
inline int closeNativeSocket(native_socket_t s) { return ::close(s); }
inline bool setNonBlocking(native_socket_t s) {
    int flags = fcntl(s, F_GETFL, 0);
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif

namespace Styx {
namespace Discord {
namespace Voice {

static constexpr int IP_DISCOVERY_PACKET_SIZE = 74;
static constexpr std::uint16_t IP_DISCOVERY_REQUEST_TYPE = 0x0001;
static constexpr std::uint16_t IP_DISCOVERY_RESPONSE_TYPE = 0x0002;
static constexpr std::uint16_t IP_DISCOVERY_LENGTH = 70;

namespace {

bool resolveSockAddr(const std::string &ip, std::uint16_t port, sockaddr_storage &storage, socklen_t &len)
{
    const std::string &ipStr = ip;
    sockaddr_in *sin = reinterpret_cast<sockaddr_in *>(&storage);
    if (inet_pton(AF_INET, ipStr.c_str(), &sin->sin_addr) == 1) {
        sin->sin_family = AF_INET;
        sin->sin_port = htons(port);
        len = sizeof(sockaddr_in);
        return true;
    }
    sockaddr_in6 *sin6 = reinterpret_cast<sockaddr_in6 *>(&storage);
    if (inet_pton(AF_INET6, ipStr.c_str(), &sin6->sin6_addr) == 1) {
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(port);
        len = sizeof(sockaddr_in6);
        return true;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *res = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(ipStr.c_str(), portStr.c_str(), &hints, &res) == 0 && res) {
        memcpy(&storage, res->ai_addr, res->ai_addrlen);
        len = static_cast<socklen_t>(res->ai_addrlen);
        freeaddrinfo(res);
        return true;
    }
    return false;
}

} // namespace

UdpTransport::UdpTransport(const Core::ProxyConfig &proxy, Platform::WorkerLoop *loop)
    : m_loop(loop), proxy(proxy)
{
}

UdpTransport::~UdpTransport()
{
    if (m_loop && m_discoveryTimerId) {
        m_loop->cancelTimer(m_discoveryTimerId);
        m_discoveryTimerId = 0;
    }
    if (m_socket != -1) {
        if (m_loop) {
            m_loop->unwatchSocket(static_cast<Platform::SocketHandle>(m_socket));
        }
        closeNativeSocket(static_cast<native_socket_t>(m_socket));
        m_socket = -1;
    }
}

void UdpTransport::startIpDiscovery(const std::string &ip, int port, std::uint32_t ssrc)
{
    serverAddress = ip;
    serverPort = static_cast<std::uint16_t>(port);
    discoveryPending = true;
    this->ssrc = ssrc;

    if (!openSocket())
        return;

    Core::log(Core::LogLevel::Info, LogVoice) << "UDP socket bound to port" << localPort()
                     << "- starting IP discovery to" << ip << ":" << serverPort;

    startDiscoveryTimeout();

    if (proxy.type != Core::ProxyConfig::Type::Socks5) {
        sendDiscoveryPacket();
        return;
    }

    association = std::make_unique<Socks5UdpAssociation>(m_loop);
    track(association->established.connect([this] { sendDiscoveryPacket(); }));
    track(association->failed.connect([this](const std::string &error) {
        if (!discoveryPending)
            return;
        discoveryPending = false;
        if (m_loop && m_discoveryTimerId) {
            m_loop->cancelTimer(m_discoveryTimerId);
            m_discoveryTimerId = 0;
        }
        ipDiscoveryFailed.publish("SOCKS5 proxy: " + error);
    }));
    association->open(proxy);
}

bool UdpTransport::openSocket()
{
    if (m_socket != -1) {
        if (m_loop) {
            m_loop->unwatchSocket(static_cast<Platform::SocketHandle>(m_socket));
        }
        closeNativeSocket(static_cast<native_socket_t>(m_socket));
        m_socket = -1;
    }

    native_socket_t sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kInvalidNativeSocket) {
        Core::log(Core::LogLevel::Warning, LogVoice) << "Failed to create UDP socket";
        ipDiscoveryFailed.publish(("Failed to create UDP socket"));
        return false;
    }

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    bindAddr.sin_port = 0;

    if (::bind(sock, reinterpret_cast<sockaddr *>(&bindAddr), sizeof(bindAddr)) != 0) {
        Core::log(Core::LogLevel::Warning, LogVoice) << "Failed to bind UDP socket";
        closeNativeSocket(sock);
        ipDiscoveryFailed.publish(("Failed to bind UDP socket"));
        return false;
    }

    setNonBlocking(sock);

    m_socket = static_cast<intptr_t>(sock);
    if (m_loop) {
        m_loop->watchSocket(static_cast<Platform::SocketHandle>(sock), [this] {
            onReadyRead();
        });
    }
    return true;
}

void UdpTransport::sendDiscoveryPacket()
{
    std::string packet(IP_DISCOVERY_PACKET_SIZE, '\0');
    std::uint16_t type = htons(IP_DISCOVERY_REQUEST_TYPE);
    std::uint16_t length = htons(IP_DISCOVERY_LENGTH);
    std::uint32_t ssrcBE = htonl(ssrc);

    memcpy(packet.data() + 0, &type, 2);
    memcpy(packet.data() + 2, &length, 2);
    memcpy(packet.data() + 4, &ssrcBE, 4);

    send(packet);
}

void UdpTransport::startDiscoveryTimeout()
{
    if (m_loop) {
        if (m_discoveryTimerId) {
            m_loop->cancelTimer(m_discoveryTimerId);
        }
        m_discoveryTimerId = m_loop->setTimer(std::chrono::milliseconds(DISCOVERY_TIMEOUT_MS), false, [this] {
            if (discoveryPending) {
                discoveryPending = false;
                ipDiscoveryFailed.publish("IP discovery timed out");
            }
        });
    }
}

void UdpTransport::send(const std::string &data)
{
    if (m_socket == -1)
        return;

    std::string targetIp = serverAddress;
    std::uint16_t targetPort = serverPort;

    std::string encapsulated;
    const std::string *packet = &data;
    if (association) {
        if (!association->isEstablished())
            return;
        encapsulated = Socks5UdpAssociation::encapsulate(data, serverAddress, serverPort);
        packet = &encapsulated;
        targetIp = association->relayAddress();
        targetPort = association->relayPort();
    }

    sockaddr_storage destAddr{};
    socklen_t destLen = 0;
    if (resolveSockAddr(targetIp, targetPort, destAddr, destLen)) {
        ::sendto(static_cast<native_socket_t>(m_socket), packet->data(), static_cast<int>(packet->size()), 0,
                 reinterpret_cast<const sockaddr *>(&destAddr), destLen);
    }
}

bool UdpTransport::isBound() const
{
    return m_socket != -1;
}

std::uint16_t UdpTransport::localPort() const
{
    if (m_socket == -1)
        return 0;

    sockaddr_storage localAddr{};
    socklen_t len = sizeof(localAddr);
    if (::getsockname(static_cast<native_socket_t>(m_socket), reinterpret_cast<sockaddr *>(&localAddr), &len) == 0) {
        if (localAddr.ss_family == AF_INET) {
            return ntohs(reinterpret_cast<sockaddr_in *>(&localAddr)->sin_port);
        } else if (localAddr.ss_family == AF_INET6) {
            return ntohs(reinterpret_cast<sockaddr_in6 *>(&localAddr)->sin6_port);
        }
    }
    return 0;
}

void UdpTransport::onReadyRead()
{
    if (m_socket == -1)
        return;

    const std::string expectedSender = association ? association->relayAddress() : serverAddress;
    const std::uint16_t expectedPort = association ? association->relayPort() : serverPort;

    native_socket_t sock = static_cast<native_socket_t>(m_socket);

    while (true) {
        char buffer[4096];
        sockaddr_storage senderAddr{};
        socklen_t senderLen = sizeof(senderAddr);

        int bytesRead = ::recvfrom(sock, buffer, sizeof(buffer), 0,
                                   reinterpret_cast<sockaddr *>(&senderAddr), &senderLen);
        if (bytesRead <= 0)
            break;

        char ipBuf[INET6_ADDRSTRLEN]{};
        std::uint16_t senderPort = 0;
        if (senderAddr.ss_family == AF_INET) {
            auto *sin = reinterpret_cast<sockaddr_in *>(&senderAddr);
            inet_ntop(AF_INET, &sin->sin_addr, ipBuf, sizeof(ipBuf));
            senderPort = ntohs(sin->sin_port);
        } else if (senderAddr.ss_family == AF_INET6) {
            auto *sin6 = reinterpret_cast<sockaddr_in6 *>(&senderAddr);
            inet_ntop(AF_INET6, &sin6->sin6_addr, ipBuf, sizeof(ipBuf));
            senderPort = ntohs(sin6->sin6_port);
        }

        std::string senderIp = std::string(ipBuf);
        if (senderPort != expectedPort || (!expectedSender.empty() && senderIp != expectedSender)) {
            Core::log(Core::LogLevel::Debug, LogVoice) << "Dropping UDP datagram from unexpected sender" << senderIp << senderPort;
            continue;
        }

        std::string datagram(buffer, bytesRead);
        if (association) {
            datagram = Socks5UdpAssociation::decapsulate(datagram);
            if (datagram.empty())
                continue;
        }

        if (discoveryPending && datagram.size() >= IP_DISCOVERY_PACKET_SIZE) {
            parseIpDiscoveryResponse(datagram);
        } else {
            datagramReceived.publish(datagram);
        }
    }
}

void UdpTransport::parseIpDiscoveryResponse(const std::string &data)
{
    std::uint16_t type = 0;
    memcpy(&type, data.data(), 2);
    type = ntohs(type);

    if (type != IP_DISCOVERY_RESPONSE_TYPE) {
        Core::log(Core::LogLevel::Debug, LogVoice) << "Dropping non-discovery packet while discovery pending";
        return;
    }

    discoveryPending = false;
    if (m_loop && m_discoveryTimerId) {
        m_loop->cancelTimer(m_discoveryTimerId);
        m_discoveryTimerId = 0;
    }
    const char *ipStart = data.data() + 8;
    int ipLen = 0;
    while (ipLen < 64 && ipStart[ipLen] != '\0') {
        ipLen++;
    }
    std::string externalIp = std::string(ipStart, ipLen);

    std::uint16_t externalPort = 0;
    memcpy(&externalPort, data.data() + data.size() - 2, 2);
    externalPort = ntohs(externalPort);

    Core::log(Core::LogLevel::Info, LogVoice) << "Discovered external IP:" << externalIp << ":" << externalPort;
    ipDiscovered.publish(externalIp, externalPort);
}

} // namespace Voice
} // namespace Discord
} // namespace Styx
