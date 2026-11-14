#include <catch2/catch_test_macros.hpp>

#include "Core/OrderedJson.hpp"
#include "Core/ProxyConfig.hpp"
#include "Discord/ClientIdentity.hpp"
#include "Discord/Gateway.hpp"
#include "Discord/Voice/VoiceGateway.hpp"
#include "Platform/Base64.hpp"
#include "Platform/Dispatcher.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <stdexcept>
#include <string_view>
#include <openssl/sha.h>
#include <zlib.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {
constexpr char messageMarker[] = "private-message-marker-6793";
constexpr char gatewayToken[] = "gateway-token-marker-6793";
constexpr char voiceToken[] = "voice-token-marker-6793";
constexpr char voiceSdp[] = "voice-sdp-marker-6793";
constexpr char binaryKey[] = "voice-key-marker-6793";

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalidSocket = INVALID_SOCKET;
#else
using Socket = int;
constexpr Socket invalidSocket = -1;
#endif

struct NetworkLifetime {
    NetworkLifetime()
    {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            throw std::runtime_error("Could not initialize test sockets.");
#endif
    }
    ~NetworkLifetime()
    {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

struct SocketHandle {
    explicit SocketHandle(Socket value) : value(value) {}
    ~SocketHandle()
    {
        if (value == invalidSocket) return;
#ifdef _WIN32
        closesocket(value);
#else
        close(value);
#endif
    }
    SocketHandle(const SocketHandle &) = delete;
    SocketHandle &operator=(const SocketHandle &) = delete;
    Socket value;
};

class GatewayServer {
public:
    GatewayServer() : listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP))
    {
        if (listener.value == invalidSocket)
            throw std::runtime_error("Could not create loopback socket.");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener.value, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
            listen(listener.value, 1) != 0)
            throw std::runtime_error("Could not bind loopback socket.");
        socklen_t size = sizeof(address);
        if (getsockname(listener.value, reinterpret_cast<sockaddr *>(&address), &size) != 0)
            throw std::runtime_error("Could not read loopback port.");
        port = ntohs(address.sin_port);
        worker = std::thread([this] {
            try { serve(); }
            catch (const std::exception &error) { failure = error.what(); }
        });
    }
    ~GatewayServer() { stop(); }
    void stop()
    {
        stopping = true;
        if (worker.joinable()) worker.join();
    }
    std::string url() const { return "ws://127.0.0.1:" + std::to_string(port); }
    const std::string &error() const { return failure; }

private:
    bool readable(Socket socket)
    {
        while (!stopping && std::chrono::steady_clock::now() < deadline) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(socket, &set);
            timeval timeout{0, 100000};
            const int ready = select(static_cast<int>(socket + 1), &set, nullptr, nullptr, &timeout);
            if (ready > 0) return true;
            if (ready < 0) throw std::runtime_error("Loopback socket wait failed.");
        }
        return false;
    }
    void receive(Socket socket, void *destination, std::size_t size)
    {
        auto *output = static_cast<char *>(destination);
        while (size) {
            if (!readable(socket)) throw std::runtime_error("Loopback receive stopped or timed out.");
            const int count = recv(socket, output, static_cast<int>(size), 0);
            if (count <= 0) throw std::runtime_error("Loopback client disconnected.");
            output += count;
            size -= count;
        }
    }
    static void sendAll(Socket socket, std::string_view bytes)
    {
        while (!bytes.empty()) {
#ifdef MSG_NOSIGNAL
            constexpr int flags = MSG_NOSIGNAL;
#else
            constexpr int flags = 0;
#endif
            const int count = static_cast<int>(send(socket, bytes.data(), static_cast<int>(bytes.size()), flags));
            if (count <= 0) throw std::runtime_error("Loopback send failed.");
            bytes.remove_prefix(count);
        }
    }
    void serve()
    {
        if (!readable(listener.value)) return;
        SocketHandle client(accept(listener.value, nullptr, nullptr));
        if (client.value == invalidSocket) throw std::runtime_error("Loopback accept failed.");
        std::string request;
        while (!request.ends_with("\r\n\r\n")) {
            char byte;
            receive(client.value, &byte, 1);
            request += byte;
            if (request.size() > 16384) throw std::runtime_error("Oversized WebSocket handshake.");
        }
        constexpr std::string_view keyHeader = "Sec-WebSocket-Key: ";
        const auto keyStart = request.find(keyHeader);
        if (keyStart == std::string::npos) throw std::runtime_error("Missing WebSocket key.");
        const auto start = keyStart + keyHeader.size();
        auto challenge = request.substr(start, request.find("\r\n", start) - start);
        challenge += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        std::array<unsigned char, SHA_DIGEST_LENGTH> digest{};
        SHA1(reinterpret_cast<const unsigned char *>(challenge.data()), challenge.size(), digest.data());
        sendAll(client.value, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Accept: " + Styx::Platform::toBase64(digest) + "\r\n\r\n");

        z_stream stream{};
        if (deflateInit(&stream, Z_DEFAULT_COMPRESSION) != Z_OK)
            throw std::runtime_error("Could not initialize fixture compression.");
        struct EndCompression {
            z_stream &stream;
            ~EndCompression() { deflateEnd(&stream); }
        } end{stream};
        auto sendJson = [&](std::string_view json) {
            std::array<char, 2048> compressed{};
            stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(json.data()));
            stream.avail_in = static_cast<uInt>(json.size());
            stream.next_out = reinterpret_cast<Bytef *>(compressed.data());
            stream.avail_out = static_cast<uInt>(compressed.size());
            if (deflate(&stream, Z_SYNC_FLUSH) != Z_OK || stream.avail_out == 0)
                throw std::runtime_error("Could not compress fixture payload.");
            const auto size = compressed.size() - stream.avail_out;
            std::string frame(1, char(0x82));
            if (size < 126) frame += static_cast<char>(size);
            else {
                frame += char(126);
                frame += static_cast<char>(size >> 8);
                frame += static_cast<char>(size & 0xff);
            }
            frame.append(compressed.data(), size);
            sendAll(client.value, frame);
        };
        sendJson(R"({"op":10,"d":{"heartbeat_interval":60000}})");
        for (;;) {
            std::array<unsigned char, 2> header;
            receive(client.value, header.data(), header.size());
            if ((header[0] & 15) == 8) throw std::runtime_error("Client closed before Identify.");
            std::size_t size = header[1] & 127;
            if (size == 126) {
                std::array<unsigned char, 2> extended;
                receive(client.value, extended.data(), extended.size());
                size = (extended[0] << 8) | extended[1];
            }
            if (!(header[1] & 128) || size > 4096)
                throw std::runtime_error("Invalid client WebSocket frame.");
            std::array<unsigned char, 4> mask;
            receive(client.value, mask.data(), mask.size());
            std::string payload(size, '\0');
            receive(client.value, payload.data(), payload.size());
            for (std::size_t i = 0; i < size; ++i) payload[i] ^= mask[i % 4];
            if ((header[0] & 15) != 1) continue;
            auto json = nlohmann::json::parse(payload);
            if (json.value("op", -1) != 2) continue;
            if (json["d"].value("token", "") != gatewayToken)
                throw std::runtime_error("Identify did not deliver the fixture token.");
            break;
        }
        sendJson(R"({"op":0,"t":"MESSAGE_CREATE","s":1,"d":{"id":"1","channel_id":"2","content":"private-message-marker-6793"}})");
        while (!stopping) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    NetworkLifetime network;
    SocketHandle listener;
    std::uint16_t port = 0;
    std::atomic<bool> stopping{false};
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::string failure;
    std::thread worker;
};

class CaptureLog {
public:
    CaptureLog() : previous(std::clog.rdbuf(stream.rdbuf())) {}
    ~CaptureLog() { std::clog.rdbuf(previous); }
    std::string text() const { return stream.str(); }
private:
    std::ostringstream stream;
    std::streambuf *previous;
};
}

TEST_CASE("Gateway logs do not contain message or voice payload data", "[gateway-logging]")
{
    GatewayServer server;
    CaptureLog capture;
    bool messageReceived = false;
    {
        Styx::Core::ProxyConfig proxy;
        Styx::Discord::ClientIdentity identity;
        Styx::Discord::Gateway gateway(gatewayToken, server.url(), identity, proxy);
        auto connection = gateway.gatewayMessageCreate.connect([&](const Styx::Discord::Message &message) {
            messageReceived = message.content.hasValue() && message.content.get() == messageMarker;
        });
        gateway.start();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!messageReceived && std::chrono::steady_clock::now() < deadline) {
            Styx::Platform::Dispatcher::main().runPending();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        gateway.hardStop();

        Styx::Discord::Voice::VoiceGateway voice("unused", Styx::Core::Snowflake(1),
            Styx::Core::Snowflake(2), Styx::Core::Snowflake(3), "session-marker-6793", voiceToken, proxy);
        voice.sendSelectProtocolWebRtc(voiceSdp, {}, "rtc-marker-6793");
        voice.sendBinaryPayload(26, binaryKey);
    }
    server.stop();
    REQUIRE(server.error().empty());
    const std::string log = capture.text();
    REQUIRE(messageReceived);
    REQUIRE(log.find("Received gateway opcode 0") != std::string::npos);
    REQUIRE(log.find("Voice >>> opcode 1 bytes") != std::string::npos);
    REQUIRE(log.find("Voice binary >>> opcode = 26 size =") != std::string::npos);
    for (const char *secret : {messageMarker, gatewayToken, voiceSdp, binaryKey}) {
        INFO(secret);
        REQUIRE(log.find(secret) == std::string::npos);
    }
}
