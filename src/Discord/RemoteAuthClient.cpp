#include "RemoteAuthClient.hpp"

#include "CurlUtils.hpp"
#include "Core/Logging.hpp"
#include "Platform/Base64.hpp"
#include "Platform/Dispatcher.hpp"

#ifdef _WIN32
#undef X509_NAME
#undef X509_EXTENSIONS
#undef X509_CERT_PAIR
#undef PKCS7_ISSUER_AND_SERIAL
#undef PKCS7_SIGNER_INFO
#undef OCSP_REQUEST
#undef OCSP_RESPONSE
#endif
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>

namespace Styx::Discord {
namespace {
constexpr const char *gatewayUrl = "wss://remote-auth-gateway.discord.gg/?v=2";

std::string field(const nlohmann::json &object, const char *name)
{
    const auto it = object.find(name);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

int integerField(const nlohmann::json &object, const char *name)
{
    const auto it = object.find(name);
    return it != object.end() && it->is_number_integer() ? it->get<int>() : 0;
}

std::string urlBase64(std::string_view bytes)
{
    auto result = Platform::toBase64(bytes);
    std::replace(result.begin(), result.end(), '+', '-');
    std::replace(result.begin(), result.end(), '/', '_');
    while (!result.empty() && result.back() == '=') result.pop_back();
    return result;
}

std::string decodeBase64(const std::string &encoded)
{
    auto decoded = Platform::fromBase64(encoded);
    if (!decoded || decoded->empty()) return {};
    return std::string(reinterpret_cast<const char *>(decoded->data()), decoded->size());
}
}

RemoteAuthClient::RemoteAuthClient(const Core::ProxyConfig &proxy, CaptchaResolver *resolver)
    : proxy(proxy), captchaResolver(resolver)
{
}

RemoteAuthClient::~RemoteAuthClient()
{
    stop();
    freeKey();
}

void RemoteAuthClient::start()
{
    if (running.load()) return;
    stop();
    freeKey();
    done = false;
    loginPending = false;
    running = true;
    if (!generateKey()) {
        freeKey();
        fail(RemoteAuthError::KeyGenerationFailed);
        return;
    }
    http = std::make_unique<HttpClient>("https://discord.com/api/v9", "", identity, proxy, captchaResolver);
    http->setReferer("https://discord.com/login");
    networkThread = std::thread(&RemoteAuthClient::networkLoop, this, generation);
}

void RemoteAuthClient::stop()
{
    ++generation;
    running = false;
    done = true;
    heartbeatTimer.stop();
    timeoutTimer.stop();
    if (networkThread.joinable()) networkThread.join();
    http.reset();
}

bool RemoteAuthClient::generateKey()
{
    keyCtx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!keyCtx || EVP_PKEY_keygen_init(keyCtx) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(keyCtx, 2048) <= 0 ||
        EVP_PKEY_keygen(keyCtx, &pkey) <= 0) return false;
    decCtx = EVP_PKEY_CTX_new(pkey, nullptr);
    return decCtx && EVP_PKEY_decrypt_init(decCtx) > 0 &&
           EVP_PKEY_CTX_set_rsa_padding(decCtx, RSA_PKCS1_OAEP_PADDING) > 0 &&
           EVP_PKEY_CTX_set_rsa_oaep_md(decCtx, EVP_sha256()) > 0 &&
           EVP_PKEY_CTX_set_rsa_mgf1_md(decCtx, EVP_sha256()) > 0;
}

void RemoteAuthClient::freeKey()
{
    EVP_PKEY_CTX_free(decCtx); decCtx = nullptr;
    EVP_PKEY_free(pkey); pkey = nullptr;
    EVP_PKEY_CTX_free(keyCtx); keyCtx = nullptr;
}

std::string RemoteAuthClient::publicKeyDer() const
{
    const int size = i2d_PUBKEY(pkey, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    auto *data = reinterpret_cast<unsigned char *>(result.data());
    if (i2d_PUBKEY(pkey, &data) <= 0) return {};
    return result;
}

std::string RemoteAuthClient::decrypt(const std::string &input) const
{
    if (!decCtx || input.empty()) return {};
    const auto *data = reinterpret_cast<const unsigned char *>(input.data());
    size_t size = 0;
    if (EVP_PKEY_decrypt(decCtx, nullptr, &size, data, input.size()) <= 0) return {};
    std::string result(size, '\0');
    if (EVP_PKEY_decrypt(decCtx, reinterpret_cast<unsigned char *>(result.data()), &size,
                         data, input.size()) <= 0) return {};
    result.resize(size);
    return result;
}

void RemoteAuthClient::networkLoop(uint64_t run)
{
    Platform::Guard<RemoteAuthClient> self(this);
    auto reportFailure = [self, run](RemoteAuthError error) {
        Platform::postToMain([self, run, error] {
            if (self && self->generation == run && self->running) self->fail(error);
        });
    };
    CURL *handle = curl_easy_init();
    if (!handle) {
        reportFailure(RemoteAuthError::ConnectionFailed);
        return;
    }
    curl_easy_setopt(handle, CURLOPT_URL, gatewayUrl);
    curl_easy_setopt(handle, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 15L);
    CurlUtils::applyCommonOptions(handle);
    CurlUtils::applyProxy(handle, proxy);
    curl_slist *headers = curl_slist_append(nullptr, "Origin: https://discord.com");
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    CURLcode result = curl_easy_perform(handle);
    curl_slist_free_all(headers);
    if (result != CURLE_OK) {
        Core::log(Core::LogLevel::Warning, LogDiscord) << "Remote auth connect failed:" << curl_easy_strerror(result);
        curl_easy_cleanup(handle);
        reportFailure(RemoteAuthError::ConnectionFailed);
        return;
    }
    {
        std::lock_guard lock(curlMutex);
        curl = handle;
    }

    char chunk[8192];
    std::string message;
    while (running) {
        size_t received = 0;
        const curl_ws_frame *meta = nullptr;
        int flags = 0;
        curl_off_t bytesLeft = 0;
        {
            std::lock_guard lock(curlMutex);
            result = curl_ws_recv(handle, chunk, sizeof(chunk), &received, &meta);
            if (meta) { flags = meta->flags; bytesLeft = meta->bytesleft; }
        }
        if (result == CURLE_AGAIN || (result == CURLE_OK && !meta)) {
            CurlUtils::wsRecvWait(curl, curlMutex);
            continue;
        }
        if (result != CURLE_OK) {
            Core::log(Core::LogLevel::Warning, LogDiscord) << "Remote auth recv error:" << curl_easy_strerror(result);
            reportFailure(RemoteAuthError::ConnectionFailed);
            break;
        }
        if (flags & CURLWS_CLOSE) {
            int code = received >= 2 ? (static_cast<uint8_t>(chunk[0]) << 8) | static_cast<uint8_t>(chunk[1]) : 1000;
            Core::log(Core::LogLevel::Info, LogDiscord) << "Remote auth closed with code" << code;
            if (code >= 4000) {
                const auto error = code == 4001 || code == 4002 ? RemoteAuthError::HandshakeFailed :
                                   code == 4003 ? RemoteAuthError::TimedOut : RemoteAuthError::ConnectionFailed;
                reportFailure(error);
            }
            break;
        }
        if (flags & (CURLWS_PING | CURLWS_PONG)) continue;
        message.append(chunk, received);
        if (bytesLeft == 0) {
            Platform::postToMain([self, run, text = std::move(message)] {
                if (self && self->generation == run && self->running) self->handleMessage(text);
            });
            message.clear();
        }
    }
    {
        std::lock_guard lock(curlMutex);
        if (curl) {
            const uint8_t payload[] = {0x03, 0xE8};
            size_t sent = 0;
            curl_ws_send(curl, payload, sizeof(payload), &sent, 0, CURLWS_CLOSE);
            curl_easy_cleanup(curl);
            curl = nullptr;
        }
    }
}

void RemoteAuthClient::send(const Core::OrderedJson &object)
{
    const auto data = object.toBytes();
    CurlUtils::wsSend(curl, curlMutex, data.data(), data.size(), CURLWS_TEXT, "remote auth");
}

void RemoteAuthClient::handleMessage(const std::string &text)
{
    if (done) return;
    const auto object = nlohmann::json::parse(text, nullptr, false);
    if (!object.is_object()) return;
    const auto op = field(object, "op");
    if (op == "hello") handleHello(object);
    else if (op == "nonce_proof") handleNonceProof(object);
    else if (op == "pending_remote_init") handlePendingRemoteInit(object);
    else if (op == "pending_ticket") handlePendingTicket(object);
    else if (op == "pending_login") handlePendingLogin(object);
    else if (op == "cancel") cancel();
    else if (op != "heartbeat_ack") Core::log(Core::LogLevel::Debug, LogDiscord) << "Unknown remote auth opcode:" << op;
}

void RemoteAuthClient::handleHello(const nlohmann::json &object)
{
    const int interval = integerField(object, "heartbeat_interval");
    if (interval > 0) {
        heartbeatTimer.start(std::chrono::milliseconds(interval), true, [this] {
            send(Core::OrderedJson().insert("op", "heartbeat"));
        });
    }
    const int timeout = integerField(object, "timeout_ms");
    if (timeout > 0) {
        timeoutTimer.start(std::chrono::milliseconds(static_cast<int64_t>(timeout) + 2000), false,
                           [this] { fail(RemoteAuthError::TimedOut); });
    }
    Core::OrderedJson init;
    init.insert("op", "init");
    init.insert("encoded_public_key", Platform::toBase64(publicKeyDer()));
    send(init);
}

void RemoteAuthClient::handleNonceProof(const nlohmann::json &object)
{
    const auto nonce = decrypt(decodeBase64(field(object, "encrypted_nonce")));
    if (nonce.empty()) { fail(RemoteAuthError::HandshakeFailed); return; }
    Core::OrderedJson reply;
    reply.insert("op", "nonce_proof");
    reply.insert("nonce", urlBase64(nonce));
    send(reply);
}

void RemoteAuthClient::handlePendingRemoteInit(const nlohmann::json &object)
{
    const auto fingerprint = field(object, "fingerprint");
    const auto der = publicKeyDer();
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_Digest(der.data(), der.size(), digest, &length, EVP_sha256(), nullptr) != 1 ||
        urlBase64(std::string_view(reinterpret_cast<const char *>(digest), length)) != fingerprint) {
        Core::log(Core::LogLevel::Warning, LogDiscord) << "Remote auth fingerprint mismatch or digest failure";
        fail(RemoteAuthError::HandshakeFailed);
        return;
    }
    fingerprintReady.publish(fingerprint);
}

void RemoteAuthClient::handlePendingTicket(const nlohmann::json &object)
{
    const auto payload = decrypt(decodeBase64(field(object, "encrypted_user_payload")));
    if (payload.empty()) return;
    std::string parts[4];
    size_t begin = 0;
    for (auto &part : parts) {
        const auto end = payload.find(':', begin);
        part = payload.substr(begin, end == std::string::npos ? end : end - begin);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    pendingTicket.publish(parts[0], parts[3], parts[2]);
}

void RemoteAuthClient::handlePendingLogin(const nlohmann::json &object)
{
    if (loginPending) return;
    loginPending = true;
    const uint64_t run = generation;
    Platform::Guard<RemoteAuthClient> self(this);
    http->post("/users/@me/remote-auth/login", nlohmann::json{{"ticket", field(object, "ticket")}},
              [self, run](const HttpResponse &response) {
        if (!self || self->generation != run || self->done) return;
        if (!response.success) {
            const bool captcha = CaptchaChallenge::fromResponseBody(response.body).has_value();
            Core::log(Core::LogLevel::Warning, LogNetwork) << "Remote auth login failed, HTTP" << response.statusCode;
            self->fail(captcha ? RemoteAuthError::CaptchaRequired : RemoteAuthError::LoginFailed);
            return;
        }
        const auto root = nlohmann::json::parse(response.body, nullptr, false);
        const auto token = root.is_object() ? self->decrypt(decodeBase64(field(root, "encrypted_token"))) : std::string();
        if (token.empty()) { self->fail(RemoteAuthError::LoginFailed); return; }
        self->succeed(token);
    });
}

void RemoteAuthClient::finish()
{
    done = true;
    running = false;
    heartbeatTimer.stop();
    timeoutTimer.stop();
}

void RemoteAuthClient::fail(RemoteAuthError error)
{
    if (done) return;
    finish();
    failed.publish(error);
}

void RemoteAuthClient::succeed(const std::string &token)
{
    if (done) return;
    finish();
    authenticated.publish(token);
}

void RemoteAuthClient::cancel()
{
    if (done) return;
    finish();
    canceled.publish();
}

} // namespace Styx::Discord
