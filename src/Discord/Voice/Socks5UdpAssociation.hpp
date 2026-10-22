#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <atomic>
#include <cstdint>
#include <thread>

#include "Core/ProxyConfig.hpp"

namespace Styx {
namespace Discord {
namespace Voice {

// RFC 1928 UDP ASSOCIATE
class Socks5UdpAssociation : public QObject
{
    Q_OBJECT
public:
    explicit Socks5UdpAssociation(QObject *parent = nullptr);
    ~Socks5UdpAssociation() override;

    void open(const Core::ProxyConfig &proxy);
    void close();

    [[nodiscard]] bool isEstablished() const { return m_established.load(); }
    [[nodiscard]] QString relayAddress() const { return m_relayAddress; }
    [[nodiscard]] quint16 relayPort() const { return m_relayPort; }

    static QByteArray encapsulate(const QByteArray &payload, const QString &destination, quint16 destinationPort);
    static QByteArray decapsulate(const QByteArray &datagram);

signals:
    void established();
    void failed(const QString &error);

private:
    void runHandshake(Core::ProxyConfig config);
    void fail(const QString &error);

    std::atomic<bool> m_established{false};
    std::atomic<bool> m_closing{false};
    QString m_relayAddress;
    quint16 m_relayPort{0};

    intptr_t m_controlSocket{-1};
    std::thread m_workerThread;
};

} // namespace Voice
} // namespace Discord
} // namespace Styx
