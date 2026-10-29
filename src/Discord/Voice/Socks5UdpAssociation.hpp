#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include "Core/ProxyConfig.hpp"
#include "Platform/Trackable.hpp"
#include "Platform/WorkerLoop.hpp"

namespace Styx::Discord::Voice {

class Socks5UdpAssociation : public Platform::Trackable
{
public:
    explicit Socks5UdpAssociation(Platform::WorkerLoop *loop = nullptr) : m_loop(loop) {}
    ~Socks5UdpAssociation() override;
    void open(const Core::ProxyConfig &proxy);
    void close();
    [[nodiscard]] bool isEstablished() const { return m_established.load(); }
    [[nodiscard]] std::string relayAddress() const { return m_relayAddress; }
    [[nodiscard]] std::uint16_t relayPort() const { return m_relayPort; }
    static std::string encapsulate(const std::string &payload, const std::string &destination, std::uint16_t destinationPort);
    static std::string decapsulate(const std::string &datagram);
    Platform::Signal<> established;
    Platform::Signal<std::string> failed;

private:
    void runHandshake(Core::ProxyConfig config);
    void fail(const std::string &error);
    void post(std::function<void()> callback);
    std::atomic<bool> m_established{false};
    std::atomic<bool> m_closing{false};
    std::atomic<std::uint64_t> m_generation{0};
    std::string m_relayAddress;
    std::uint16_t m_relayPort{0};
    std::atomic<intptr_t> m_controlSocket{-1};
    std::thread m_workerThread;
    Platform::WorkerLoop *m_loop;
};

} // namespace Styx::Discord::Voice
