#include "Http.hpp"
#include "Dispatcher.hpp"
#include "Utf8.hpp"

#include <curl/curl.h>

#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace Styx::Platform {

std::string HttpResponse::getHeader(const std::string &name) const
{
    for (const auto &[k, v] : headers) {
        if (equalsIgnoreCaseAscii(k, name)) {
            return v;
        }
    }
    return {};
}

struct TransferContext {
    uint64_t id{0};
    HttpRequest request;
    HttpCallback callback;
    HttpResponse response;
    CURL *easy{nullptr};
    curl_slist *headerList{nullptr};
    std::atomic<bool> cancelled{false};
};

struct HttpClient::Impl {
    CURLM *multi{nullptr};
    std::atomic<bool> running{true};
    std::thread thread;

    std::mutex mutex;
    std::deque<std::unique_ptr<TransferContext>> pending;
    std::unordered_map<uint64_t, TransferContext *> active;
    uint64_t nextId{1};

    Impl()
    {
        static std::once_flag initFlag;
        std::call_once(initFlag, []() {
            curl_global_init(CURL_GLOBAL_ALL);
        });

        multi = curl_multi_init();
        if (multi) {
            curl_multi_setopt(multi, CURLMOPT_PIPELINING, static_cast<long>(CURLPIPE_MULTIPLEX));
        }
        thread = std::thread(&Impl::workerLoop, this);
    }

    ~Impl()
    {
        shutdown();
    }

    void shutdown()
    {
        if (!thread.joinable()) {
            return;
        }
        running.store(false);
        if (multi) {
            curl_multi_wakeup(multi);
        }
        thread.join();

        if (multi) {
            curl_multi_cleanup(multi);
            multi = nullptr;
        }
    }

    static size_t writeCallback(char *ptr, size_t size, size_t nmemb, void *userdata)
    {
        auto *ctx = static_cast<TransferContext *>(userdata);
        size_t total = size * nmemb;
        ctx->response.body.append(ptr, total);
        return total;
    }

    static size_t headerCallback(char *buffer, size_t size, size_t nitems, void *userdata)
    {
        auto *ctx = static_cast<TransferContext *>(userdata);
        size_t total = size * nitems;
        std::string line(buffer, total);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = std::string(trim(line.substr(0, colon)));
            std::string val = std::string(trim(line.substr(colon + 1)));
            ctx->response.headers.emplace_back(std::move(key), std::move(val));
        }
        return total;
    }

    static int xferinfoCallback(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
    {
        auto *ctx = static_cast<TransferContext *>(clientp);
        if (ctx && ctx->cancelled.load()) {
            return 1; // Aborts transfer
        }
        return 0;
    }

    void workerLoop()
    {
        while (running.load()) {
            drainPending();

            int stillRunning = 0;
            if (multi) {
                curl_multi_perform(multi, &stillRunning);

                int msgsLeft = 0;
                CURLMsg *msg = nullptr;
                while ((msg = curl_multi_info_read(multi, &msgsLeft))) {

                    if (msg->msg == CURLMSG_DONE) {
                        CURL *easy = msg->easy_handle;
                        CURLcode res = msg->data.result;

                        TransferContext *ctx = nullptr;
                        curl_easy_getinfo(easy, CURLINFO_PRIVATE, &ctx);

                        if (ctx) {
                            completeTransfer(ctx, res);
                        }
                    }
                }
            }

            if (!running.load()) {
                break;
            }

            int numfds = 0;
            if (multi) {
                curl_multi_poll(multi, nullptr, 0, 200, &numfds);
            }
        }

        abortAll();
    }

    void drainPending()
    {
        std::deque<std::unique_ptr<TransferContext>> toStart;
        {
            std::lock_guard<std::mutex> lock(mutex);
            toStart.swap(pending);
        }

        for (auto &ctx : toStart) {
            if (ctx->cancelled.load()) {
                dispatchResult(ctx.get(), "Request cancelled before start", 0, true);
                continue;
            }


            CURL *easy = curl_easy_init();
            if (!easy) {
                dispatchResult(ctx.get(), "Failed to initialize CURL easy handle", 0, false);
                continue;
            }


            ctx->easy = easy;

            curl_easy_setopt(easy, CURLOPT_PRIVATE, ctx.get());

            curl_easy_setopt(easy, CURLOPT_URL, ctx->request.url.c_str());


            if (ctx->request.method == "POST") {
                curl_easy_setopt(easy, CURLOPT_POST, 1L);
                curl_easy_setopt(easy, CURLOPT_POSTFIELDS, ctx->request.body.data());
                curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(ctx->request.body.size()));
            } else if (ctx->request.method != "GET") {
                curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, ctx->request.method.c_str());
                if (!ctx->request.body.empty()) {
                    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, ctx->request.body.data());
                    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(ctx->request.body.size()));
                }
            }


            // Headers
            for (const auto &[k, v] : ctx->request.headers) {
                std::string headerStr = k + ": " + v;
                ctx->headerList = curl_slist_append(ctx->headerList, headerStr.c_str());
            }
            if (ctx->headerList) {
                curl_easy_setopt(easy, CURLOPT_HTTPHEADER, ctx->headerList);
            }


            // Proxy
            if (!ctx->request.proxy.empty()) {
                curl_easy_setopt(easy, CURLOPT_PROXY, ctx->request.proxy.c_str());
                curl_easy_setopt(easy, CURLOPT_NOPROXY, "");
            }


            // Timeout & redirects
            if (ctx->request.timeout.count() > 0) {
                curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(ctx->request.timeout.count()));
            }
            curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, ctx->request.followRedirects ? 1L : 0L);


            // Callbacks
            curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, writeCallback);
            curl_easy_setopt(easy, CURLOPT_WRITEDATA, ctx.get());
            curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, headerCallback);
            curl_easy_setopt(easy, CURLOPT_HEADERDATA, ctx.get());
            curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, xferinfoCallback);
            curl_easy_setopt(easy, CURLOPT_XFERINFODATA, ctx.get());
            curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);


            CURLMcode mc = curl_multi_add_handle(multi, easy);

            if (mc != CURLM_OK) {
                curl_easy_cleanup(easy);
                ctx->easy = nullptr;
                dispatchResult(ctx.get(), curl_multi_strerror(mc), 0, false);
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                uint64_t id = ctx->id;
                active[id] = ctx.release(); // Ownership transferred to active map
            }

        }
    }

    void completeTransfer(TransferContext *ctx, CURLcode result)
    {
        long statusCode = 0;
        if (ctx->easy) {
            curl_easy_getinfo(ctx->easy, CURLINFO_RESPONSE_CODE, &statusCode);
            curl_multi_remove_handle(multi, ctx->easy);
            curl_easy_cleanup(ctx->easy);
            ctx->easy = nullptr;
        }

        std::string errStr;
        bool cancelled = ctx->cancelled.load();
        if (cancelled || result == CURLE_ABORTED_BY_CALLBACK) {
            cancelled = true;
            errStr = "Request cancelled";
        } else if (result != CURLE_OK) {
            errStr = curl_easy_strerror(result);
        }

        dispatchResult(ctx, errStr, static_cast<int>(statusCode), cancelled);

        {
            std::lock_guard<std::mutex> lock(mutex);
            active.erase(ctx->id);
        }
        delete ctx;
    }

    void dispatchResult(TransferContext *ctx, const std::string &error, int statusCode, bool cancelled)
    {
        ctx->response.statusCode = statusCode;
        ctx->response.error = error;
        ctx->response.cancelled = cancelled;

        auto cb = std::move(ctx->callback);
        auto resp = std::move(ctx->response);
        if (cb) {
            Dispatcher::main().post([cb = std::move(cb), resp = std::move(resp)]() {
                cb(resp);
            });
        }

        if (ctx->headerList) {
            curl_slist_free_all(ctx->headerList);
            ctx->headerList = nullptr;
        }
    }

    void abortAll()
    {
        std::unordered_map<uint64_t, TransferContext *> toAbort;
        {
            std::lock_guard<std::mutex> lock(mutex);
            toAbort.swap(active);
        }

        for (auto &[id, ctx] : toAbort) {
            if (ctx->easy && multi) {
                curl_multi_remove_handle(multi, ctx->easy);
                curl_easy_cleanup(ctx->easy);
                ctx->easy = nullptr;
            }
            dispatchResult(ctx, "Worker shut down", 0, true);
            delete ctx;
        }
    }
};

HttpClient &HttpClient::instance()
{
    static HttpClient s_instance;
    return s_instance;
}

HttpClient::HttpClient()
    : m_impl(std::make_unique<Impl>())
{
}

HttpClient::~HttpClient() = default;

uint64_t HttpClient::request(HttpRequest req, HttpCallback callback)
{
    auto ctx = std::make_unique<TransferContext>();
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        id = m_impl->nextId++;
        ctx->id = id;
        ctx->request = std::move(req);
        ctx->callback = std::move(callback);
        m_impl->pending.push_back(std::move(ctx));
    }
    if (m_impl->multi) {
        curl_multi_wakeup(m_impl->multi);
    }
    return id;
}

void HttpClient::cancel(uint64_t requestId)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    auto it = m_impl->active.find(requestId);
    if (it != m_impl->active.end()) {
        it->second->cancelled.store(true);
        if (m_impl->multi) {
            curl_multi_wakeup(m_impl->multi);
        }
        return;
    }

    for (auto &p : m_impl->pending) {
        if (p->id == requestId) {
            p->cancelled.store(true);
            return;
        }
    }
}

void HttpClient::shutdown()
{
    m_impl->shutdown();
}

} // namespace Styx::Platform
