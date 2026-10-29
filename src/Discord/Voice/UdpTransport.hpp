#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include "Platform/Trackable.hpp"
#include "Platform/WorkerLoop.hpp"
#include "Core/ProxyConfig.hpp"
#include "Socks5UdpAssociation.hpp"

namespace Styx::Discord::Voice {

class UdpTransport : public Platform::Trackable
{
public:
    explicit UdpTransport(const Core::ProxyConfig &proxy, Platform::WorkerLoop *loop = nullptr);
    void setWorkerLoop(Platform::WorkerLoop *loop) { m_loop = loop; }
    ~UdpTransport() override;

    void startIpDiscovery(const std::string &ip, int port, std::uint32_t ssrc);
    void send(const std::string &data);
    [[nodiscard]] bool isBound() const;
    [[nodiscard]] std::uint16_t localPort() const;

    Platform::Signal<std::string, int> ipDiscovered;
    Platform::Signal<std::string> ipDiscoveryFailed;
    Platform::Signal<std::string> datagramReceived;

private:
    void onReadyRead();
    void parseIpDiscoveryResponse(const std::string &data);
    bool openSocket();
    void sendDiscoveryPacket();
    void startDiscoveryTimeout();

    intptr_t m_socket{-1};
    Platform::WorkerLoop *m_loop = nullptr;
    Platform::TimerId m_discoveryTimerId{0};
    std::string serverAddress;
    std::uint16_t serverPort = 0;
    std::uint32_t ssrc = 0;
    bool discoveryPending = false;
    Core::ProxyConfig proxy;
    std::unique_ptr<Socks5UdpAssociation> association;
    static constexpr int DISCOVERY_TIMEOUT_MS = 5000;
};

} // namespace Styx::Discord::Voice
