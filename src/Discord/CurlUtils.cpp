#include "CurlUtils.hpp"

#include "ClientIdentity.hpp"
#include "Core/Logging.hpp"
#include "Platform/Base64.hpp"
#include "Platform/Native.hpp"
#include "Platform/Paths.hpp"
#include "Platform/Http.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <locale>
#include <regex>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/select.h>
#endif

namespace Styx::Discord::CurlUtils {
namespace {
std::mutex buildMutex;
int cachedBuildNumber = 0;
bool buildNumberFetchInFlight = false;
std::vector<std::function<void()>> buildNumberWaiters;
constexpr int fallbackBuildNumber = 600539;
constexpr int buildNumberTimeoutMs = 10000;

void resolveBuildNumberWaiters()
{
    std::vector<std::function<void()>> waiters;
    {
        std::lock_guard lock(buildMutex);
        buildNumberFetchInFlight = false;
        waiters.swap(buildNumberWaiters);
    }
    for (auto &waiter : waiters)
        waiter();
}

std::string systemTimezone()
{
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
    try {
        return std::string(std::chrono::current_zone()->name());
    } catch (...) {}
#else
    if (const char *tz = std::getenv("TZ"); tz && *tz)
        return *tz == ':' ? tz + 1 : tz;
    std::error_code ec;
    const auto target = std::filesystem::read_symlink("/etc/localtime", ec).generic_string();
    const auto pos = target.find("zoneinfo/");
    if (!ec && pos != std::string::npos)
        return target.substr(pos + 9);
#endif
    return "UTC";
}
}

void ensureBuildNumber(const Core::ProxyConfig &proxy, std::function<void()> done)
{
    bool cached;
    {
        std::lock_guard lock(buildMutex);
        cached = cachedBuildNumber > 0;
        if (!cached) {
            buildNumberWaiters.push_back(std::move(done));
            if (buildNumberFetchInFlight)
                return;
            buildNumberFetchInFlight = true;
        }
    }
    if (cached) {
        done();
        return;
    }

    Core::log(Core::LogLevel::Info, LogNetwork) << "Fetching Discord build number";
    Platform::HttpRequest req;
    req.url = "https://discord.com/app";
    req.timeout = std::chrono::milliseconds(buildNumberTimeoutMs);
    req.headers.emplace_back("User-Agent", getUserAgent());
    if (proxy.enabled())
        req.proxy = proxy.toCurlProxyString();
    Platform::HttpClient::instance().request(std::move(req), [proxy](const Platform::HttpResponse &appReply) {
        if (!appReply.ok()) {
            Core::log(Core::LogLevel::Warning, LogNetwork) << "Failed to fetch Discord app page:" << appReply.error;
            resolveBuildNumberWaiters();
            return;
        }
        std::smatch match;
        static const std::regex sentryRegex(R"(/assets/sentry.*?\.js)");
        if (!std::regex_search(appReply.body, match, sentryRegex)) {
            Core::log(Core::LogLevel::Warning, LogNetwork) << "Failed to find sentry JS path";
            resolveBuildNumberWaiters();
            return;
        }
        Platform::HttpRequest sentryReq;
        sentryReq.url = "https://discord.com" + match.str();
        sentryReq.timeout = std::chrono::milliseconds(buildNumberTimeoutMs);
        sentryReq.headers.emplace_back("User-Agent", getUserAgent());
        if (proxy.enabled())
            sentryReq.proxy = proxy.toCurlProxyString();
        Platform::HttpClient::instance().request(std::move(sentryReq), [](const Platform::HttpResponse &reply) {
            if (!reply.ok()) {
                Core::log(Core::LogLevel::Warning, LogNetwork) << "Failed to fetch sentry JS:" << reply.error;
            } else {
                std::smatch match;
                static const std::regex buildRegex("buildNumber\",\"([0-9]+)");
                if (std::regex_search(reply.body, match, buildRegex)) {
                    try {
                        int number = std::stoi(match[1].str());
                        std::lock_guard lock(buildMutex);
                        cachedBuildNumber = number;
                        Core::log(Core::LogLevel::Info, LogNetwork) << "Discord build number:" << number;
                    } catch (const std::exception &) {
                        Core::log(Core::LogLevel::Warning, LogNetwork) << "Invalid Discord build number";
                    }
                } else {
                    Core::log(Core::LogLevel::Warning, LogNetwork) << "Failed to extract build number";
                }
            }
            resolveBuildNumberWaiters();
        });
    });
}

int getBuildNumber()
{
    std::lock_guard lock(buildMutex);
    return cachedBuildNumber > 0 ? cachedBuildNumber : fallbackBuildNumber;
}

std::string getCertificatePath()
{
    const auto path = Platform::Resources::path("certs/cacert.pem");
    if (std::filesystem::exists(path))
        return path.string();
    Core::log(Core::LogLevel::Warning, LogNetwork) << "Certificate file not found at" << path.string();
    return {};
}

std::string getUserAgent()
{
    return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
           "Chrome/142.0.0.0 Safari/537.36";
}
std::string getImpersonateTarget() { return "chrome142"; }
UserAgentProps getUserAgentProps() { return { "Windows", "Chrome", "142.0.0.0", "10" }; }

std::string getSystemLocale()
{
#ifdef _WIN32
    wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH)) {
        int size = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
        if (size > 1) {
            std::string result(static_cast<size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, name, -1, result.data(), size, nullptr, nullptr);
            result.pop_back();
            return result;
        }
    }
#else
    try {
        auto result = std::locale("").name();
        if (result != "C" && result != "POSIX") {
            const auto end = result.find_first_of(".@");
            result.resize(end == std::string::npos ? result.size() : end);
            for (char &c : result) if (c == '_') c = '-';
            return result;
        }
    } catch (const std::exception &) {}
#endif
    return "en-US";
}

void applyCommonOptions(CURL *curl)
{
    static const std::string caBundle = [] {
        auto path = getCertificatePath();
        return path.empty() ? std::string{} : Platform::Resources::readFile(path);
    }();
    if (!caBundle.empty()) {
        curl_blob blob{const_cast<char *>(caBundle.data()), caBundle.size(), CURL_BLOB_NOCOPY};
        curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
    }
#ifdef IS_CURL_IMPERSONATE
    curl_easy_impersonate(curl, getImpersonateTarget().c_str(), 1);
#endif
    curl_easy_setopt(curl, CURLOPT_USERAGENT, getUserAgent().c_str());
}

void applyProxy(CURL *curl, const Core::ProxyConfig &proxy)
{
    if (!proxy.enabled()) return;
    curl_easy_setopt(curl, CURLOPT_PROXY, proxy.toCurlUrl().c_str());
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "");
    if (proxy.type == Core::ProxyConfig::Type::Https) {
        CURLcode rc = curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_HTTPS);
        if (rc != CURLE_OK)
            Core::log(Core::LogLevel::Warning, LogNetwork) << "libcurl lacks HTTPS-proxy support:" << curl_easy_strerror(rc);
    }
    if (!proxy.username.empty()) {
        curl_easy_setopt(curl, CURLOPT_PROXYUSERNAME, proxy.username.c_str());
        curl_easy_setopt(curl, CURLOPT_PROXYPASSWORD, proxy.password.c_str());
    }
}

void appendDiscordHeaders(curl_slist **headers, const ClientIdentity &identity, const std::string &referer)
{
    static const std::string tz = systemTimezone();
    ClientPropertiesBuildParams params{};
    params.includeClientHeartbeatSessionId = true;
    const auto properties = Platform::toBase64(identity.buildClientProperties(params).toJson().toBytes());
    *headers = curl_slist_append(*headers, ("X-Discord-Timezone: " + tz).c_str());
    *headers = curl_slist_append(*headers, ("X-Discord-Locale: " + identity.discordLocale()).c_str());
    *headers = curl_slist_append(*headers, ("X-Super-Properties: " + properties).c_str());
    *headers = curl_slist_append(*headers, "X-Debug-Options: bugReporterEnabled");
    *headers = curl_slist_append(*headers, ("Referer: " + referer).c_str());
}

size_t writeToByteArray(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    const size_t bytes = size * nmemb;
    static_cast<std::string *>(userdata)->append(ptr, bytes);
    return bytes;
}
curl_socket_t getActiveSocket(CURL *curl)
{
    curl_socket_t sockfd = CURL_SOCKET_BAD;
    if (curl) curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sockfd);
    return sockfd;
}
void waitOnSocket(curl_socket_t sockfd, bool forWrite, long timeoutMs)
{
    if (sockfd == CURL_SOCKET_BAD) return;
    timeval timeout{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sockfd, &fds);
    if (forWrite) select(static_cast<int>(sockfd) + 1, nullptr, &fds, nullptr, &timeout);
    else select(static_cast<int>(sockfd) + 1, &fds, nullptr, nullptr, &timeout);
}
bool wsSend(CURL *&curl, std::mutex &mutex, const char *data, size_t len, unsigned flags, const char *what)
{
    std::lock_guard lock(mutex);
    if (!curl) return false;
    size_t total = 0;
    while (total < len) {
        size_t sent = 0;
        CURLcode res = curl_ws_send(curl, data + total, len - total, &sent, 0, flags);
        if (res == CURLE_OK) total += sent;
        else if (res == CURLE_AGAIN) waitOnSocket(getActiveSocket(curl), true, 100);
        else {
            Core::log(Core::LogLevel::Warning, LogNetwork) << "Error sending" << what << "WebSocket payload:" << curl_easy_strerror(res);
            return false;
        }
    }
    return true;
}
void wsRecvWait(CURL *&curl, std::mutex &mutex)
{
    curl_socket_t sockfd;
    {
        std::lock_guard lock(mutex);
        sockfd = getActiveSocket(curl);
    }
    waitOnSocket(sockfd, false, 10);
}
} // namespace Styx::Discord::CurlUtils
