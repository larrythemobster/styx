#include "HttpClient.hpp"
#include "RequestWorker.hpp"

#include "Platform/Dispatcher.hpp"

namespace Styx {
namespace Discord {

HttpClient::HttpClient(const std::string &baseUrl, const std::string &token, ClientIdentity &identity,
                       const Core::ProxyConfig &proxy, CaptchaResolver *captchaResolver)
    : baseUrl(baseUrl), token(token), identity(identity), proxy(proxy), captchaResolver(captchaResolver)
{
    worker = std::make_unique<RequestWorker>(this, token, identity, proxy, captchaResolver);
}

HttpClient::~HttpClient()
{
    if (worker)
        worker->shutdown();
}

void HttpClient::get(const std::string &endpoint, const std::vector<std::pair<std::string, std::string>> &query, HttpCallback callback)
{
    std::string url = baseUrl + endpoint;
    for (const auto &[key, value] : query) {
        url += (url.find('?') == std::string::npos ? "?" : "&");
        url += Platform::urlEncode(key) + "=" + Platform::urlEncode(value);
    }
    executeRequest(Method::GET, url, {}, callback);
}

void HttpClient::post(const std::string &endpoint, const nlohmann::json &body, HttpCallback callback)
{
    std::string url = baseUrl + endpoint;
    std::string data = body.dump();
    executeRequest(Method::POST, url, data, callback);
}

void HttpClient::post(const std::string &endpoint, const nlohmann::json &body, const ContextProperties &context,
                      HttpCallback callback)
{
    std::string url = baseUrl + endpoint;
    std::string data = body.dump();
    RequestOptions options;
    options.context = context;
    executeRequest(Method::POST, url, data, callback, options);
}

void HttpClient::patch(const std::string &endpoint, const nlohmann::json &body, HttpCallback callback)
{
    std::string url = baseUrl + endpoint;
    std::string data = body.dump();
    executeRequest(Method::PATCH, url, data, callback);
}

void HttpClient::put(const std::string &endpoint, const nlohmann::json &body, HttpCallback callback)
{
    std::string url = baseUrl + endpoint;
    std::string data = body.dump();
    executeRequest(Method::PUT, url, data, callback);
}

void HttpClient::delete_(const std::string &endpoint, HttpCallback callback)
{
    std::string url = baseUrl + endpoint;
    executeRequest(Method::DELETE_, url, {}, callback);
}

void HttpClient::delete_(const std::string &endpoint, const nlohmann::json &body, HttpCallback callback)
{
    std::string url = baseUrl + endpoint;
    std::string data = body.dump();
    executeRequest(Method::DELETE_, url, data, callback);
}

void HttpClient::post(const std::string &endpoint, const Core::OrderedJson &body, HttpCallback callback)
{
    executeRequest(Method::POST, baseUrl + endpoint, body.toBytes(), callback);
}

void HttpClient::put(const std::string &endpoint, const Core::OrderedJson &body, HttpCallback callback)
{
    executeRequest(Method::PUT, baseUrl + endpoint, body.toBytes(), callback);
}

void HttpClient::patch(const std::string &endpoint, const Core::OrderedJson &body, HttpCallback callback)
{
    executeRequest(Method::PATCH, baseUrl + endpoint, body.toBytes(), callback);
}

void HttpClient::patch(const std::string &endpoint, const Core::OrderedJson::Array &body, HttpCallback callback)
{
    executeRequest(Method::PATCH, baseUrl + endpoint, body.toBytes(), callback);
}

void HttpClient::send(Method method, const std::string &endpoint, const std::string &jsonBody, const RequestOptions &options, HttpCallback callback)
{
    executeRequest(method, baseUrl + endpoint, jsonBody, callback, options);
}

void HttpClient::postMultipart(const std::string &endpoint, const nlohmann::json &jsonPayload,
                               const std::vector<FileUpload> &files, HttpCallback callback)
{
    std::string url = baseUrl + endpoint;
    std::string jsonData = jsonPayload.dump();
    executeMultipartRequest(url, jsonData, files, callback);
}

void HttpClient::postForm(const std::string &endpoint, const std::vector<std::pair<std::string, std::string>> &fields,
                          const std::vector<FileUpload> &files, const RequestOptions &options, HttpCallback callback)
{
    executeMultipartRequest(baseUrl + endpoint, {}, files, callback, fields, options);
}

void HttpClient::putExternalFile(const std::string &absoluteUrl, const std::string &filePath,
                                 const std::string &contentType, HttpCallback callback,
                                 std::function<void(int64_t, int64_t)> progress,
                                 std::shared_ptr<std::atomic<bool>> cancelFlag)
{
    RequestDescriptor descriptor;
    descriptor.method = Method::PUT;
    descriptor.url = absoluteUrl;
    descriptor.uploadFilePath = filePath;
    descriptor.external = true;
    descriptor.contentType = contentType;
    descriptor.callback = std::move(callback);
    descriptor.cancelFlag = std::move(cancelFlag);
    submitExternalPut(descriptor, std::move(progress));
}

void HttpClient::putExternal(const std::string &absoluteUrl, const std::string &data,
                             const std::string &contentType, HttpCallback callback,
                             std::function<void(int64_t, int64_t)> progress,
                             std::shared_ptr<std::atomic<bool>> cancelFlag)
{
    RequestDescriptor descriptor;
    descriptor.method = Method::PUT;
    descriptor.url = absoluteUrl;
    descriptor.body = data;
    descriptor.external = true;
    descriptor.contentType = contentType;
    descriptor.callback = std::move(callback);
    descriptor.cancelFlag = std::move(cancelFlag);
    submitExternalPut(descriptor, std::move(progress));
}

void HttpClient::submitExternalPut(RequestDescriptor &descriptor,
                                   std::function<void(int64_t, int64_t)> progress)
{
    if (progress) {
        Platform::Guard<HttpClient> guard(this);
        descriptor.progressCallback = [this, guard,
                                       progress = std::move(progress)](int64_t sent, int64_t total) {
            Platform::Dispatcher::main().post([guard, progress, sent, total]() {
                if (guard)
                    progress(sent, total);
            });
        };
    }
    worker->submit(std::move(descriptor));
}

void HttpClient::executeRequest(Method method, const std::string &url, const std::string &data, HttpCallback callback, const RequestOptions &options)
{
    RequestDescriptor descriptor;
    descriptor.method = method;
    descriptor.url = url;
    descriptor.body = data;
    descriptor.multipart = false;
    descriptor.referer = referer;
    descriptor.fingerprint = fingerprint;
    if (options.context)
        descriptor.contextProperties = options.context->toHeaderValue();
    descriptor.auditLogReason = options.auditLogReason;
    descriptor.originalMd5 = options.originalMd5;
    descriptor.mfaAuthorization = options.mfaAuthorization;
    descriptor.callback = std::move(callback);
    worker->submit(std::move(descriptor));
}

void HttpClient::executeMultipartRequest(const std::string &url, const std::string &jsonData,
                                         const std::vector<FileUpload> &files, HttpCallback callback,
                                         const std::vector<std::pair<std::string, std::string>> &formFields,
                                         const RequestOptions &options)
{
    RequestDescriptor descriptor;
    descriptor.method = Method::POST;
    descriptor.url = url;
    descriptor.body = jsonData;
    descriptor.multipart = true;
    descriptor.files = files;
    descriptor.formFields = formFields;
    descriptor.referer = referer;
    descriptor.fingerprint = fingerprint;
    if (options.context)
        descriptor.contextProperties = options.context->toHeaderValue();
    descriptor.auditLogReason = options.auditLogReason;
    descriptor.originalMd5 = options.originalMd5;
    descriptor.mfaAuthorization = options.mfaAuthorization;
    descriptor.callback = std::move(callback);
    worker->submit(std::move(descriptor));
}

void HttpClient::onRequestComplete(RequestDescriptor descriptor, HttpResponse response,
                                   std::optional<CaptchaChallenge> challenge)
{
    if (!challenge || descriptor.captchaAttempt >= kMaxCaptchaAttempts || !captchaResolver) {
        if (captchaResolver && descriptor.captchaAttempt > 0)
            captchaResolver->notifyConcluded();
        descriptor.callback(response);
        return;
    }

    Platform::Guard<HttpClient> self(this);
    captchaResolver->resolve(*challenge, proxy, [this, self, descriptor = std::move(descriptor), response = std::move(response)](std::optional<CaptchaSolution> s) mutable {
        if (!self)
            return;
        if (!s) {
            HttpResponse err = response;
            err.success = false;
            if (err.error.empty())
                err.error = "Captcha canceled";
            descriptor.callback(err);
            return;
        }
        descriptor.captchaAttempt += 1;
        descriptor.solution = std::move(s);
        worker->submit(std::move(descriptor));
    });
}

} // namespace Discord
} // namespace Styx
