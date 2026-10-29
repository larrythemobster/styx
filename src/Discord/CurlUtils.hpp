#pragma once

#include <string>

#include "Core/ProxyConfig.hpp"

#include <curl/curl.h>

#include <functional>
#include <mutex>



namespace Styx {
namespace Discord {

class ClientIdentity;

namespace CurlUtils {

struct UserAgentProps
{
    std::string os;
    std::string browser;
    std::string browserVersion;
    std::string osVersion;
};

std::string getCertificatePath();
std::string getUserAgent();
std::string getImpersonateTarget();
UserAgentProps getUserAgentProps();
std::string getSystemLocale();

void ensureBuildNumber(const Core::ProxyConfig &proxy, std::function<void()> done);
int getBuildNumber();

// cacert, curl-impersonate, user-agent
void applyCommonOptions(CURL *curl);

void applyProxy(CURL *curl, const Core::ProxyConfig &proxy);

void appendDiscordHeaders(curl_slist **headers, const ClientIdentity &identity, const std::string &referer);

// CURLOPT_WRITEFUNCTION for binary strings
size_t writeToByteArray(char *ptr, size_t size, size_t nmemb, void *userdata);

// send full frame with retry
bool wsSend(CURL *&curl, std::mutex &mutex, const char *data, size_t len, unsigned flags, const char *what);

// shortcut for waiting on the socket
void wsRecvWait(CURL *&curl, std::mutex &mutex);

curl_socket_t getActiveSocket(CURL *curl);

void waitOnSocket(curl_socket_t sockfd, bool forWrite, long timeoutMs);

} // namespace CurlUtils
} // namespace Discord
} // namespace Styx
