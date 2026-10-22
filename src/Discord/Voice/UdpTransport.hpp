#pragma once

#include <QByteArray>
#include <QObject>
#include "Platform/WorkerLoop.hpp"

#include "Core/ProxyConfig.hpp"
#include "Socks5UdpAssociation.hpp"

namespace Styx {
namespace Discord {
namespace Voice {

class UdpTransport : public QObject
{
    Q_OBJECT
public:
    explicit UdpTransport(const Core::ProxyConfig &proxy, Platform::WorkerLoop *loop = nullptr, QObject *parent = nullptr);
    void setWorkerLoop(Platform::WorkerLoop *loop) { m_loop = loop; }
    ~UdpTransport() override;

    void startIpDiscovery(const QString &ip, int port, quint32 ssrc);
    void send(const QByteArray &data);

    [[nodiscard]] bool isBound() const;
    [[nodiscard]] quint16 localPort() const;

signals:
    void ipDiscovered(const QString &externalIp, int externalPort);
    void ipDiscoveryFailed(const QString &error);
    void datagramReceived(const QByteArray &data);

private slots:
    void onReadyRead();

private:
    void parseIpDiscoveryResponse(const QByteArray &data);
    bool openSocket();
    void sendDiscoveryPacket();
    void startDiscoveryTimeout();

    intptr_t m_socket{-1};
    Platform::WorkerLoop *m_loop = nullptr;
    Platform::TimerId m_discoveryTimerId{0};
    QString serverAddress;
    quint16 serverPort = 0;
    quint32 ssrc = 0;
    bool discoveryPending = false;

    Core::ProxyConfig proxy;
    Socks5UdpAssociation *association = nullptr;

    static constexpr int DISCOVERY_TIMEOUT_MS = 5000;
};

} // namespace Voice
} // namespace Discord
} // namespace Styx
