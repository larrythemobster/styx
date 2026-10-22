#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Styx::Platform {

struct HttpRequest {
    std::string url;
    std::string method{"GET"};
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::string proxy; // http://... or socks5://...
    std::chrono::milliseconds timeout{30000};
    bool followRedirects{true};
};

struct HttpResponse {
    int statusCode{0};
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string error;
    bool cancelled{false};

    bool ok() const { return error.empty() && !cancelled && statusCode >= 200 && statusCode < 300; }

    std::string getHeader(const std::string &name) const;
};

using HttpCallback = std::function<void(const HttpResponse &)>;

class HttpClient {
public:
    static HttpClient &instance();

    HttpClient();
    ~HttpClient();

    HttpClient(const HttpClient &) = delete;
    HttpClient &operator=(const HttpClient &) = delete;

    // Asynchronously executes an HTTP request. Callback is dispatched via Dispatcher.
    uint64_t request(HttpRequest req, HttpCallback callback);

    // Convenience GET request
    uint64_t get(std::string url, HttpCallback callback)
    {
        HttpRequest req;
        req.url = std::move(url);
        req.method = "GET";
        return request(std::move(req), std::move(callback));
    }

    // Convenience POST request
    uint64_t post(std::string url, std::string body, std::string contentType, HttpCallback callback)
    {
        HttpRequest req;
        req.url = std::move(url);
        req.method = "POST";
        req.body = std::move(body);
        if (!contentType.empty()) {
            req.headers.emplace_back("Content-Type", std::move(contentType));
        }
        return request(std::move(req), std::move(callback));
    }

    // Cancel an in-flight request by ID
    void cancel(uint64_t requestId);

    // Shut down worker thread and cancel all active transfers
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Styx::Platform
