#pragma once

#include "ClientIdentity.hpp"
#include "HttpClient.hpp"
#include "Core/OrderedJson.hpp"
#include "Core/ProxyConfig.hpp"
#include "Platform/Timer.hpp"
#include "Platform/Trackable.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <memory>
#include <thread>
#include <nlohmann/json.hpp>
#include <curl/curl.h>

struct evp_pkey_st;
struct evp_pkey_ctx_st;

namespace Styx::Discord {

enum class RemoteAuthError {
    KeyGenerationFailed,
    ConnectionFailed,
    HandshakeFailed,
    TimedOut,
    CaptchaRequired,
    LoginFailed,
};

class RemoteAuthClient : public Platform::Trackable {
public:
    explicit RemoteAuthClient(const Core::ProxyConfig &proxy, CaptchaResolver *captchaResolver = nullptr);
    ~RemoteAuthClient() override;

    void start();
    void stop();

    Platform::Signal<const std::string &> fingerprintReady;
    Platform::Signal<const std::string &, const std::string &, const std::string &> pendingTicket;
    Platform::Signal<const std::string &> authenticated;
    Platform::Signal<RemoteAuthError> failed;
    Platform::Signal<> canceled;

private:
    bool generateKey();
    void freeKey();
    std::string publicKeyDer() const;
    std::string decrypt(const std::string &in) const;
    void networkLoop(uint64_t generation);
    void handleMessage(const std::string &text);
    void handleHello(const nlohmann::json &obj);
    void handleNonceProof(const nlohmann::json &obj);
    void handlePendingRemoteInit(const nlohmann::json &obj);
    void handlePendingTicket(const nlohmann::json &obj);
    void handlePendingLogin(const nlohmann::json &obj);
    void send(const Core::OrderedJson &obj);
    void fail(RemoteAuthError error);
    void succeed(const std::string &token);
    void cancel();
    void finish();

    evp_pkey_st *pkey = nullptr;
    evp_pkey_ctx_st *keyCtx = nullptr;
    evp_pkey_ctx_st *decCtx = nullptr;
    std::atomic<bool> running{false};
    bool done = false;
    bool loginPending = false;
    uint64_t generation = 0;
    std::mutex curlMutex;
    CURL *curl = nullptr;
    std::thread networkThread;
    Platform::Timer heartbeatTimer;
    Platform::Timer timeoutTimer;
    Core::ProxyConfig proxy;
    CaptchaResolver *captchaResolver;
    ClientIdentity identity;
    std::unique_ptr<HttpClient> http;
};

} // namespace Styx::Discord
