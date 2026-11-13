#pragma once

#include "Platform/Trackable.hpp"
#include "Platform/Native.hpp"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <vector>
#include <utility>

#include <curl/curl.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

#include "CaptchaResolver.hpp"
#include "ContextProperties.hpp"
#include "Core/OrderedJson.hpp"
#include "Core/ProxyConfig.hpp"

namespace Styx {
namespace Discord {

class ClientIdentity;
class RequestWorker;
struct RequestDescriptor;

struct HttpResponse
{
    int statusCode = 0;
    std::string body;
    std::string error;
    bool success = false;

    [[nodiscard]] bool rateLimited() const { return statusCode == 429; }
};

struct FileUpload
{
    std::string filename;
    std::string data;
    std::string mimeType;
    std::string fieldName;
};

using HttpCallback = std::function<void(const HttpResponse &)>;

inline std::string encodeUriComponent(const std::string &text)
{
    return Platform::urlEncode(text);
}

struct RequestOptions
{
    std::optional<ContextProperties> context;
    std::optional<std::string> auditLogReason;
    std::string originalMd5;
    // Short-lived verification from POST /mfa/finish for actions that need multi-factor confirmation.
    std::string mfaAuthorization;
};

class HttpClient : public Platform::Trackable
{
public:
    explicit HttpClient(const std::string &baseUrl, const std::string &token, ClientIdentity &identity,
                        const Core::ProxyConfig &proxy, CaptchaResolver *captchaResolver = nullptr);
    ~HttpClient() override;

    enum class Method {
        GET,
        POST,
        PUT,
        PATCH,
        DELETE_, // thanks windows.h for the DELETE macro
    };

    void setReferer(const std::string &referer) { this->referer = referer; }
    void setFingerprint(const std::string &fingerprint) { this->fingerprint = fingerprint; }

    void get(const std::string &endpoint, const std::vector<std::pair<std::string, std::string>> &query, HttpCallback callback);
    void post(const std::string &endpoint, const nlohmann::json &body, HttpCallback callback);
    void post(const std::string &endpoint, const nlohmann::json &body, const ContextProperties &context, HttpCallback callback);
    void patch(const std::string &endpoint, const nlohmann::json &body, HttpCallback callback);
    void put(const std::string &endpoint, const nlohmann::json &body, HttpCallback callback);
    void delete_(const std::string &endpoint, HttpCallback callback);
    void delete_(const std::string &endpoint, const nlohmann::json &body, HttpCallback callback);

    void post(const std::string &endpoint, const Core::OrderedJson &body, HttpCallback callback);
    void put(const std::string &endpoint, const Core::OrderedJson &body, HttpCallback callback);
    void patch(const std::string &endpoint, const Core::OrderedJson &body, HttpCallback callback);
    void patch(const std::string &endpoint, const Core::OrderedJson::Array &body, HttpCallback callback);
    void send(Method method, const std::string &endpoint, const std::string &jsonBody, const RequestOptions &options, HttpCallback callback);
    void postMultipart(const std::string &endpoint, const nlohmann::json &jsonPayload,
                       const std::vector<FileUpload> &files, HttpCallback callback);
    void postForm(const std::string &endpoint, const std::vector<std::pair<std::string, std::string>> &fields,
                  const std::vector<FileUpload> &files, const RequestOptions &options, HttpCallback callback);
    // for gcp uploads
    void putExternalFile(const std::string &absoluteUrl, const std::string &filePath,
                         const std::string &contentType, HttpCallback callback,
                         std::function<void(int64_t sent, int64_t total)> progress = {},
                         std::shared_ptr<std::atomic<bool>> cancelFlag = {});
    void putExternal(const std::string &absoluteUrl, const std::string &data,
                     const std::string &contentType, HttpCallback callback,
                     std::function<void(int64_t sent, int64_t total)> progress = {},
                     std::shared_ptr<std::atomic<bool>> cancelFlag = {});

private:
    void executeRequest(Method method, const std::string &url, const std::string &data, HttpCallback callback, const RequestOptions &options = {});
    void executeMultipartRequest(const std::string &url, const std::string &jsonData,
                                 const std::vector<FileUpload> &files, HttpCallback callback,
                                 const std::vector<std::pair<std::string, std::string>> &formFields = {},
                                 const RequestOptions &options = {});
    void submitExternalPut(RequestDescriptor &descriptor,
                           std::function<void(int64_t, int64_t)> progress);

    void onRequestComplete(RequestDescriptor descriptor, HttpResponse response,
                           std::optional<CaptchaChallenge> challenge);

    std::string baseUrl;
    std::string token;
    ClientIdentity &identity;
    Core::ProxyConfig proxy;
    CaptchaResolver *captchaResolver;
    std::string referer = "https://discord.com/channels/@me";
    std::string fingerprint;

    std::unique_ptr<RequestWorker> worker;

    friend class RequestWorker;
};

} // namespace Discord
} // namespace Styx
