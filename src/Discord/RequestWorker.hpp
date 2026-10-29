#pragma once

#include <cstdio>
#include <cstdint>
#include <vector>
#include <utility>
#include "Platform/Trackable.hpp"

#include <curl/curl.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>

#include "CaptchaResolver.hpp"
#include "Core/ProxyConfig.hpp"
#include "HttpClient.hpp"

namespace Styx {
namespace Discord {

class ClientIdentity;
class RequestWorker;

struct RequestDescriptor
{
    HttpClient::Method method = HttpClient::Method::GET;
    std::string url;
    std::string body; // json body or payload_json for multipart
    bool multipart = false;
    bool external = false; // no headers (gcp)
    std::string contentType; // external Content-Type
    std::string uploadFilePath; // stream from disk
    std::vector<FileUpload> files; // multipart
    std::vector<std::pair<std::string, std::string>> formFields;
    std::string referer;
    std::string fingerprint;
    std::string contextProperties;
    std::optional<std::string> auditLogReason;
    std::string originalMd5;
    HttpCallback callback;
    std::function<void(int64_t sent, int64_t total)> progressCallback; // worker thread!
    std::shared_ptr<std::atomic<bool>> cancelFlag;
    int captchaAttempt = 0;
    std::optional<CaptchaSolution> solution;
};

struct TransferContext
{
    CURL *easy = nullptr;
    curl_slist *headers = nullptr;
    curl_mime *mime = nullptr; // multipart
    std::FILE *uploadFile = nullptr; // streamed body, worker thread only
    std::string url;
    HttpResponse response;
    RequestDescriptor descriptor;
    RequestWorker *worker = nullptr;
    int64_t lastProgressSent = -1;

    ~TransferContext() { if (uploadFile) std::fclose(uploadFile); }
};

class RequestWorker
{
public:
    RequestWorker(HttpClient *owner, std::string token, ClientIdentity &identity,
                  const Core::ProxyConfig &proxy, CaptchaResolver *captchaResolver);
    ~RequestWorker();

    void submit(RequestDescriptor descriptor);

    void shutdown();

private:
    void threadLoop();
    void drainSubmissions();
    CURL *buildEasyHandle(TransferContext *ctx);
    void handleCompletion(TransferContext *ctx, CURLcode result);
    void failTransfer(TransferContext *ctx, const char *error);
    void dispatchCompletion(RequestDescriptor descriptor, HttpResponse response,
                            std::optional<CaptchaChallenge> challenge);
    void abortAllInFlight();

    HttpClient *owner;
    Platform::Guard<HttpClient> ownerGuard; // null-check
    std::string token;
    ClientIdentity &identity;
    Core::ProxyConfig proxy;
    CaptchaResolver *captchaResolver;

    CURLM *multi = nullptr;
    CURLSH *share = nullptr;

    std::thread thread;
    std::atomic<bool> running{ false };

    std::mutex mutex;
    std::deque<RequestDescriptor> pending;
    bool acceptingSubmissions = true;

    std::unordered_set<TransferContext *> active; // worker thread only
};

} // namespace Discord
} // namespace Styx
