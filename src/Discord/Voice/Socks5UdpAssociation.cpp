#include "Socks5UdpAssociation.hpp"
#include "Core/Logging.hpp"

#include <QtEndian>
#include <QMetaObject>

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
constexpr quint8 ATYP_IPV4 = 0x01;
constexpr quint8 ATYP_DOMAIN = 0x03;
constexpr quint8 ATYP_IPV6 = 0x04;
constexpr char REPLY_SUCCEEDED = 0x00;

void appendAddress(QByteArray &out, const QString &destination, quint16 port)
{
    std::string destStr = destination.toStdString();
    in_addr addr4{};
    in6_addr addr6{};

    if (inet_pton(AF_INET, destStr.c_str(), &addr4) == 1) {
        out.append(char(ATYP_IPV4));
        out.append(reinterpret_cast<const char *>(&addr4), 4);
    } else if (inet_pton(AF_INET6, destStr.c_str(), &addr6) == 1) {
        out.append(char(ATYP_IPV6));
        out.append(reinterpret_cast<const char *>(&addr6), 16);
    } else {
        QByteArray domainUtf8 = destination.toUtf8();
        if (!domainUtf8.isEmpty() && domainUtf8.size() <= 255) {
            out.append(char(ATYP_DOMAIN));
            out.append(static_cast<char>(domainUtf8.size()));
            out.append(domainUtf8);
        } else {
            return;
        }
    }

    quint16 portBE = qToBigEndian(port);
    out.append(reinterpret_cast<const char *>(&portBE), 2);
}

int addressFieldLength(const QByteArray &data, int offset)
{
    if (data.size() <= offset)
        return -1;

    switch (static_cast<quint8>(data.at(offset))) {
    case ATYP_IPV4:
        return 1 + 4 + 2;
    case ATYP_IPV6:
        return 1 + 16 + 2;
    case ATYP_DOMAIN:
        if (data.size() <= offset + 1)
            return -1;
        return 1 + 1 + static_cast<quint8>(data.at(offset + 1)) + 2;
    default:
        return -1;
    }
}

QString replyErrorString(quint8 code)
{
    switch (code) {
    case 0x01:
        return QStringLiteral("general SOCKS server failure");
    case 0x02:
        return QStringLiteral("connection not allowed by ruleset");
    case 0x03:
        return QStringLiteral("network unreachable");
    case 0x04:
        return QStringLiteral("host unreachable");
    case 0x05:
        return QStringLiteral("connection refused");
    case 0x06:
        return QStringLiteral("TTL expired");
    case 0x07:
        return QStringLiteral("command not supported");
    case 0x08:
        return QStringLiteral("address type not supported");
    default:
        return QStringLiteral("unknown error %1").arg(code);
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

Socks5UdpAssociation::Socks5UdpAssociation(QObject *parent)
    : QObject(parent)
{
}

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
    m_established = false;

    if (m_controlSocket != -1) {
        native_socket_t s = static_cast<native_socket_t>(m_controlSocket);
        m_controlSocket = -1;
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
    std::string hostStr = config.host.toStdString();

    int rc = getaddrinfo(hostStr.c_str(), portStr.c_str(), &hints, &res);
    if (rc != 0 || !res) {
        fail(QStringLiteral("Failed to resolve SOCKS5 proxy host: %1").arg(config.host));
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
        fail(QStringLiteral("Failed to connect to SOCKS5 proxy at %1:%2").arg(config.host).arg(config.port));
        return;
    }

    m_controlSocket = static_cast<intptr_t>(sock);

    // 1. Send greeting
    std::vector<char> greeting = {SOCKS_VERSION, 1, METHOD_NONE};
    if (!config.username.isEmpty()) {
        greeting[1] = 2;
        greeting.push_back(METHOD_USERPASS);
    }

    if (::send(sock, greeting.data(), static_cast<int>(greeting.size()), 0) <= 0) {
        fail(QStringLiteral("Failed to send greeting to SOCKS5 proxy"));
        return;
    }

    // 2. Receive method selection
    char methodReply[2]{};
    if (recvExact(sock, methodReply, 2) != 2) {
        fail(QStringLiteral("Failed to receive method selection from SOCKS5 proxy"));
        return;
    }

    if (methodReply[0] != SOCKS_VERSION) {
        fail(QStringLiteral("Invalid SOCKS version received from proxy"));
        return;
    }

    if (methodReply[1] == METHOD_UNACCEPTABLE) {
        fail(QStringLiteral("Proxy rejected all offered authentication methods"));
        return;
    }

    if (methodReply[1] == METHOD_USERPASS) {
        QByteArray user = config.username.toUtf8();
        QByteArray pass = config.password.toUtf8();
        if (user.size() > 255 || pass.size() > 255) {
            fail(QStringLiteral("SOCKS5 credentials exceed 255 bytes"));
            return;
        }

        std::vector<char> authReq;
        authReq.push_back(AUTH_VERSION);
        authReq.push_back(static_cast<char>(user.size()));
        authReq.insert(authReq.end(), user.begin(), user.end());
        authReq.push_back(static_cast<char>(pass.size()));
        authReq.insert(authReq.end(), pass.begin(), pass.end());

        if (::send(sock, authReq.data(), static_cast<int>(authReq.size()), 0) <= 0) {
            fail(QStringLiteral("Failed to send credentials to SOCKS5 proxy"));
            return;
        }

        char authReply[2]{};
        if (recvExact(sock, authReply, 2) != 2 || authReply[1] != 0) {
            fail(QStringLiteral("Proxy rejected the supplied credentials"));
            return;
        }
    } else if (methodReply[1] != METHOD_NONE) {
        fail(QStringLiteral("Proxy selected unsupported authentication method %1").arg(int(methodReply[1])));
        return;
    }

    // 3. Send UDP ASSOCIATE request (dst 0.0.0.0:0)
    char assocReq[10] = {SOCKS_VERSION, CMD_UDP_ASSOCIATE, 0x00, static_cast<char>(ATYP_IPV4), 0, 0, 0, 0, 0, 0};
    if (::send(sock, assocReq, 10, 0) <= 0) {
        fail(QStringLiteral("Failed to send UDP ASSOCIATE request to SOCKS5 proxy"));
        return;
    }

    // 4. Receive associate reply
    char assocHdr[4]{};
    if (recvExact(sock, assocHdr, 4) != 4) {
        fail(QStringLiteral("Failed to receive UDP ASSOCIATE reply"));
        return;
    }

    if (assocHdr[0] != SOCKS_VERSION || assocHdr[1] != REPLY_SUCCEEDED) {
        fail(QStringLiteral("UDP ASSOCIATE failed: %1").arg(replyErrorString(static_cast<quint8>(assocHdr[1]))));
        return;
    }

    quint8 atyp = static_cast<quint8>(assocHdr[3]);
    int addrLen = 0;
    if (atyp == ATYP_IPV4) {
        addrLen = 4;
    } else if (atyp == ATYP_IPV6) {
        addrLen = 16;
    } else {
        fail(QStringLiteral("Proxy returned unsupported relay address type %1").arg(int(atyp)));
        return;
    }

    std::vector<char> addrAndPort(addrLen + 2);
    if (recvExact(sock, addrAndPort.data(), addrLen + 2) != addrLen + 2) {
        fail(QStringLiteral("Incomplete UDP ASSOCIATE relay address received"));
        return;
    }

    char ipBuf[INET6_ADDRSTRLEN]{};
    if (atyp == ATYP_IPV4) {
        inet_ntop(AF_INET, addrAndPort.data(), ipBuf, sizeof(ipBuf));
    } else {
        inet_ntop(AF_INET6, addrAndPort.data(), ipBuf, sizeof(ipBuf));
    }
    quint16 port = qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(addrAndPort.data() + addrLen));

    m_relayAddress = QString::fromLatin1(ipBuf);
    m_relayPort = port;

    if (m_relayAddress.isEmpty() || m_relayAddress == QStringLiteral("0.0.0.0") || m_relayAddress == QStringLiteral("::")) {
        sockaddr_storage peerStorage{};
        socklen_t peerLen = sizeof(peerStorage);
        if (::getpeername(sock, reinterpret_cast<sockaddr *>(&peerStorage), &peerLen) == 0) {
            char peerBuf[INET6_ADDRSTRLEN]{};
            if (peerStorage.ss_family == AF_INET) {
                inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(&peerStorage)->sin_addr, peerBuf, sizeof(peerBuf));
            } else if (peerStorage.ss_family == AF_INET6) {
                inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6 *>(&peerStorage)->sin6_addr, peerBuf, sizeof(peerBuf));
            }
            m_relayAddress = QString::fromLatin1(peerBuf);
        }
    }

    if (m_relayPort == 0) {
        fail(QStringLiteral("Proxy returned an invalid relay port"));
        return;
    }

    m_established = true;
    qCInfo(LogVoice) << "SOCKS5 UDP relay established at" << m_relayAddress << ":" << m_relayPort;

    QMetaObject::invokeMethod(this, &Socks5UdpAssociation::established, Qt::QueuedConnection);

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
        fail(QStringLiteral("Proxy closed the UDP association"));
    }
}

void Socks5UdpAssociation::fail(const QString &error)
{
    m_established = false;
    if (m_closing)
        return;

    qCWarning(LogVoice) << "SOCKS5 UDP association failed:" << error;
    QMetaObject::invokeMethod(this, [this, error]() {
        emit failed(error);
    }, Qt::QueuedConnection);
}

QByteArray Socks5UdpAssociation::encapsulate(const QByteArray &payload, const QString &destination,
                                             quint16 destinationPort)
{
    if (destination.isEmpty())
        return QByteArray();

    QByteArray datagram(3, '\0');
    int before = datagram.size();
    appendAddress(datagram, destination, destinationPort);
    if (datagram.size() == before) {
        return QByteArray();
    }
    datagram.append(payload);
    return datagram;
}

QByteArray Socks5UdpAssociation::decapsulate(const QByteArray &datagram)
{
    if (datagram.size() < 4 || datagram.at(2) != 0)
        return QByteArray();

    const int tail = addressFieldLength(datagram, 3);
    if (tail < 0 || datagram.size() < 3 + tail)
        return QByteArray();

    return datagram.mid(3 + tail);
}

} // namespace Voice
} // namespace Discord
} // namespace Styx
