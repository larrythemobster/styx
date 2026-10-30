#include "BrowserCaptchaResolver.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>
#include <nlohmann/json.hpp>
#include "Platform/Desktop.hpp"
#include "Platform/Native.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

#include "Core/Logging.hpp"
#include "Platform/Dispatcher.hpp"
#include "UI/Rml/RmlView.hpp"
#include "UI/Rml/Shell.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using native_socket_t = SOCKET;
constexpr native_socket_t kInvalidNativeSocket = INVALID_SOCKET;
inline int closeNativeSocket(native_socket_t s) { return ::closesocket(s); }
inline bool setNonBlocking(native_socket_t s) { u_long mode = 1; return ioctlsocket(s, FIONBIO, &mode) == 0; }
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
using native_socket_t = int;
constexpr native_socket_t kInvalidNativeSocket = -1;
inline int closeNativeSocket(native_socket_t s) { return ::close(s); }
inline bool setNonBlocking(native_socket_t s) { int flags = fcntl(s, F_GETFL, 0); return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0; }
#endif

namespace Styx {
namespace UI {

static const char *kHarnessTemplate = R"HTML(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>Styx Captcha Challenge</title>
<script src="https://js.hcaptcha.com/1/api.js?render=explicit" async defer></script>
<script id="cfg" type="application/json">__CONFIG_JSON__</script>
<style>
  body { font-family: sans-serif; background: #313338; color: #dbdee1;
         display: flex; flex-direction: column; align-items: center;
         justify-content: center; min-height: 90vh; }
</style>
</head>
<body>
<p>Complete the captcha below to make Discord happy</p>
<div id="hcaptcha-container"></div>
<script>
function cfg() { return JSON.parse(document.getElementById('cfg').textContent); }
function postBack(payload) {
  fetch(cfg().tokenUrl, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload)
  }).then(function () {
    document.body.innerHTML = '<p>You can close this tab and return to Styx.</p>';
  });
}
function onSolved(token) { postBack({ token: token }); }
function onCaptchaError(err) { postBack({ error: String(err) }); }
function renderWidget() {
  var c = cfg();
  var params = { sitekey: c.sitekey, callback: onSolved, 'error-callback': onCaptchaError };
  if (c.rqdata || c.invisible) params.size = 'invisible';
  var id = hcaptcha.render('hcaptcha-container', params);
  if (c.rqdata)
    hcaptcha.execute(id, { rqdata: c.rqdata });
  else if (c.invisible)
    hcaptcha.execute(id);
}
var _poll = setInterval(function () {
  if (window.hcaptcha && window.hcaptcha.render) {
    clearInterval(_poll);
    renderWidget();
  }
}, 100);
</script>
</body>
</html>
)HTML";


static void sendHttp(intptr_t clientSock, int code, const std::string &contentType,
                     const std::string &body)
{
    const char *status = code == 200 ? "200 OK" : code == 403 ? "403 Forbidden" :
                         code == 404 ? "404 Not Found" : "400 Bad Request";
    std::string resp = std::string("HTTP/1.1 ") + status + "\r\nContent-Type: " + contentType +
                       "\r\nContent-Length: " + std::to_string(body.size()) +
                       "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body;
    ::send(static_cast<native_socket_t>(clientSock), resp.data(), static_cast<int>(resp.size()), 0);
    closeNativeSocket(static_cast<native_socket_t>(clientSock));
}

BrowserCaptchaResolver::BrowserCaptchaResolver(Shell &shell)
    : shell(shell)
{
}

BrowserCaptchaResolver::~BrowserCaptchaResolver()
{
    shuttingDown = true;
    stopTtl();
    teardownServer();
    if (modal) {
        auto *document = modal;
        modal = nullptr;
        document->RemoveEventListener("click", &modalListener);
        document->RemoveEventListener("keydown", &modalListener);
        document->Close();
    }
    auto cb = std::move(currentCb);
    if (cb)
        cb(std::nullopt);
    for (auto &job : queue) {
        if (job.cb)
            job.cb(std::nullopt);
    }
}

void BrowserCaptchaResolver::resolve(const Discord::CaptchaChallenge &challenge,
                                     const Core::ProxyConfig &proxy,
                                     Discord::CaptchaResolver::Callback cb)
{
    Platform::Guard<BrowserCaptchaResolver> guard(this);
    Platform::Dispatcher::main().post([guard, challenge, proxy, cb = std::move(cb)]() mutable {
        if (!guard) return;
        if (guard->state == State::Verifying) {
            guard->current = challenge;
            guard->currentProxy = proxy;
            guard->currentCb = std::move(cb);
            guard->beginChallenge(true);
            return;
        }
        guard->queue.push_back({challenge, proxy, std::move(cb)});
        if (guard->state == State::Idle) guard->startNext();
    });
}

void BrowserCaptchaResolver::notifyConcluded()
{
    Platform::Guard<BrowserCaptchaResolver> guard(this);
    Platform::Dispatcher::main().post([guard]() {
        if (!guard || guard->state != State::Verifying) return;
        guard->state = State::Idle;
        guard->currentCb = nullptr;
        guard->stopTtl();
        guard->teardownServer();
        guard->closeModal();
        if (!guard->queue.empty()) guard->startNext();
    });
}

void BrowserCaptchaResolver::startNext()
{
    if (state != State::Idle || queue.empty())
        return;

    PendingJob job = std::move(queue.front());
    queue.pop_front();

    current = job.challenge;
    currentProxy = job.proxy;
    currentCb = job.cb;

    // hcaptcha only for now
    if (current.service != "hcaptcha") {
        Core::log(Core::LogLevel::Warning, LogUI) << "Unsupported captcha service:" << current.service;
        Discord::CaptchaResolver::Callback cb = std::move(currentCb);
        currentCb = nullptr;
        if (cb)
            cb(std::nullopt);
        startNext();
        return;
    }

    state = State::Waiting;
    if (!ensureModal()) {
        Core::log(Core::LogLevel::Warning, LogUI) << "Unable to open captcha prompt";
        cancelCurrent();
        return;
    }
    proxyWarning = false;
    modal->Show(::Rml::ModalFlag::Modal);
    modal->PullToFront();
    modal->SetClass("open", true);
    view->requestRender();

    beginChallenge(false);
}


void BrowserCaptchaResolver::beginChallenge(bool isRetry)
{
    teardownServer();

    state = State::Waiting;
    nonce = Platform::randomUuid() + Platform::randomUuid();

    native_socket_t sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == kInvalidNativeSocket) {
        Core::log(Core::LogLevel::Warning, LogUI) << "Failed to create captcha loopback socket";
        cancelCurrent();
        return;
    }

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bindAddr.sin_port = 0;

    if (::bind(sock, reinterpret_cast<sockaddr *>(&bindAddr), sizeof(bindAddr)) != 0 ||
        ::listen(sock, 5) != 0) {
        Core::log(Core::LogLevel::Warning, LogUI) << "Failed to bind/listen captcha loopback server";
        closeNativeSocket(sock);
        cancelCurrent();
        return;
    }

    setNonBlocking(sock);

    sockaddr_in boundAddr{};
    socklen_t len = sizeof(boundAddr);
    if (::getsockname(sock, reinterpret_cast<sockaddr *>(&boundAddr), &len) == 0) {
        port = ntohs(boundAddr.sin_port);
    }

    serverSocket = static_cast<intptr_t>(sock);

    timerId = Platform::Dispatcher::main().setTimer(std::chrono::milliseconds(20), true, [this]() {
        pollSockets();
    });
    proxyWarning = false;
    updateModal(isRetry
                    ? "Failed. Click below to open your browser and try again."
                    : "Discord wants you to complete a captcha to continue. Click below to open it in your browser.",
                true);
    ttlTimer.start(std::chrono::seconds(120), false, [this]() {
        Core::log(Core::LogLevel::Warning, LogUI) << "Captcha timed out";
        cancelCurrent();
    });
}

void BrowserCaptchaResolver::deliverSolution(const Discord::CaptchaSolution &solution)
{
    if (state != State::Waiting)
        return;

    state = State::Verifying;

    teardownServer();
    stopTtl();

    proxyWarning = false;
    updateModal("Verifying...", false);

    Discord::CaptchaResolver::Callback cb = std::move(currentCb);
    currentCb = nullptr;
    if (cb)
        cb(solution);
}

void BrowserCaptchaResolver::cancelCurrent()
{
    if (state == State::Idle)
        return;

    state = State::Idle;

    Discord::CaptchaResolver::Callback cb = std::move(currentCb);
    currentCb = nullptr;

    stopTtl();
    teardownServer();
    closeModal();

    if (cb)
        cb(std::nullopt);

    if (!queue.empty())
        startNext();
}

void BrowserCaptchaResolver::pollSockets()
{
    if (serverSocket == -1)
        return;

    native_socket_t srv = static_cast<native_socket_t>(serverSocket);

    while (true) {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        native_socket_t client = ::accept(srv, reinterpret_cast<sockaddr *>(&clientAddr), &clientLen);
        if (client == kInvalidNativeSocket)
            break;
        setNonBlocking(client);
        buffers.emplace(static_cast<intptr_t>(client), std::string());
    }

    std::vector<intptr_t> clients;
    for (const auto &[socket, buffer] : buffers) clients.push_back(socket);
    for (intptr_t clientSock : clients) {
        char buf[4096];
        int n = ::recv(static_cast<native_socket_t>(clientSock), buf, sizeof(buf), 0);
        if (n > 0) {
            buffers[clientSock].append(buf, n);
            handleClient(clientSock);
        } else if (n == 0) {
            closeNativeSocket(static_cast<native_socket_t>(clientSock));
            buffers.erase(clientSock);
        } else {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK) {
                closeNativeSocket(static_cast<native_socket_t>(clientSock));
                buffers.erase(clientSock);
            }
#else
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                closeNativeSocket(static_cast<native_socket_t>(clientSock));
                buffers.erase(clientSock);
            }
#endif
        }
    }
}

void BrowserCaptchaResolver::handleClient(intptr_t clientSock)
{
    auto &buf = buffers[clientSock];
    if (buf.size() > 64 * 1024) {
        buffers.erase(clientSock);
        sendHttp(clientSock, 400, "text/plain", "too large");
        return;
    }
    const auto headerEnd = buf.find("\r\n\r\n");
    if (headerEnd == std::string::npos) return;
    std::istringstream headers(buf.substr(0, headerEnd));
    std::string line, method, path;
    if (!std::getline(headers, line)) return;
    std::istringstream request(line);
    if (!(request >> method >> path)) {
        buffers.erase(clientSock);
        sendHttp(clientSock, 400, "text/plain", "bad request");
        return;
    }
    size_t contentLength = 0;
    std::string origin;
    while (std::getline(headers, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
        auto value = line.substr(colon + 1);
        const auto first = value.find_first_not_of(" \t\r");
        const auto last = value.find_last_not_of(" \t\r");
        value = first == std::string::npos ? "" : value.substr(first, last - first + 1);
        if (key == "content-length") {
            try { contentLength = std::stoull(value); }
            catch (...) {
                buffers.erase(clientSock);
                sendHttp(clientSock, 400, "text/plain", "bad request");
                return;
            }
            if (contentLength > 64 * 1024) {
                buffers.erase(clientSock);
                sendHttp(clientSock, 400, "text/plain", "too large");
                return;
            }
        } else if (key == "origin") origin = value;
    }
    const auto bodyStart = headerEnd + 4;
    if (buf.size() - bodyStart < contentLength) return;
    const auto body = buf.substr(bodyStart, contentLength);
    buffers.erase(clientSock);
    const std::string expectedPath = "/" + nonce;
    if (nonce.empty() || !path.starts_with(expectedPath)) {
        sendHttp(clientSock, 404, "text/plain", "not found");
        return;
    }

    if (method == "GET" && path == expectedPath) {
        sendHttp(clientSock, 200, "text/html; charset=utf-8", buildHarnessHtml());
        return;
    }

    if (method == "POST" && path == expectedPath + "/token") {
        const std::string expectedOrigin = "http://127.0.0.1:" + std::to_string(port);
        if (!origin.empty() && origin != expectedOrigin) {
            sendHttp(clientSock, 403, "text/plain", "bad origin");
            return;
        }

        sendHttp(clientSock, 200, "application/json", "{\"ok\":true}");

        const auto obj = nlohmann::json::parse(body, nullptr, false);
        const auto token = obj.is_object() && obj.contains("token") && obj["token"].is_string()
            ? obj["token"].get<std::string>() : std::string();
        if (token.empty()) {
            Core::log(Core::LogLevel::Warning, LogUI) << "Captcha harness reported an error";
            updateModal("The captcha could not be completed. Open your browser and try again.", true);
            return;
        }

        Discord::CaptchaSolution sol;
        sol.token = token;
        sol.sessionId = current.sessionId;
        sol.rqtoken = current.rqtoken;
        deliverSolution(sol);
        return;
    }

    sendHttp(clientSock, 404, "text/plain", "not found");
}

void BrowserCaptchaResolver::teardownServer()
{
    if (timerId != 0) {
        Platform::Dispatcher::main().cancelTimer(timerId);
        timerId = 0;
    }
    for (const auto &[clientSock, buffer] : buffers) {
        closeNativeSocket(static_cast<native_socket_t>(clientSock));
    }
    buffers.clear();

    if (serverSocket != -1) {
        closeNativeSocket(static_cast<native_socket_t>(serverSocket));
        serverSocket = -1;
    }
    port = 0;
    nonce.clear();
}

void BrowserCaptchaResolver::stopTtl()
{
    ttlTimer.stop();
}

void BrowserCaptchaResolver::ModalListener::ProcessEvent(::Rml::Event &event)
{
    owner.processEvent(event);
}

void BrowserCaptchaResolver::ModalListener::OnDetach(::Rml::Element *element)
{
    owner.documentDetached(element);
}

void BrowserCaptchaResolver::documentDetached(::Rml::Element *element)
{
    if (element != modal)
        return;
    modal = nullptr;
    if (!shuttingDown) {
        Platform::Guard<BrowserCaptchaResolver> guard(this);
        Platform::Dispatcher::main().post([guard]() {
            if (guard && !guard->modal && guard->state != State::Idle) guard->cancelCurrent();
        });
    }
}

bool BrowserCaptchaResolver::ensureModal()
{
    if (modal)
        return true;
    view = shell.view();
    if (!view || !view->context())
        return false;
    modal = view->context()->LoadDocument(":/ui/captcha_modal.rml");
    if (!modal)
        return false;
    modal->AddEventListener("click", &modalListener);
    modal->AddEventListener("keydown", &modalListener);
    return true;
}

void BrowserCaptchaResolver::updateModal(const std::string &message, bool canOpen)
{
    currentMessage = message;
    if (!modal)
        return;
    if (auto *title = modal->GetElementById("captcha-title"))
        title->SetInnerRML("Verification required");
    if (auto *element = modal->GetElementById("captcha-message"))
        element->SetInnerRML(Platform::htmlEscape(message));
    if (auto *button = modal->GetElementById("captcha-open")) {
        button->SetClass("disabled", !canOpen);
        button->SetPseudoClass("disabled", !canOpen);
    }
    if (auto *button = modal->GetElementById("captcha-open"))
        button->SetInnerRML("Open in browser");
    if (view)
        view->requestRender();
}

void BrowserCaptchaResolver::openBrowser()
{
    if (state != State::Waiting)
        return;
    proxyWarning = false;
    if (Platform::Desktop::openUrl(harnessUrl()))
        updateModal("Waiting for you to finish in your browser...", true);
    else
        updateModal("Could not open the browser. Please try again.", true);
}

void BrowserCaptchaResolver::processEvent(::Rml::Event &event)
{
    if (!modal || state == State::Idle)
        return;
    if (event.GetType() == "keydown") {
        const int key = event.GetParameter<int>("key_identifier", 0);
        if (key == ::Rml::Input::KI_ESCAPE) {
            event.StopPropagation();
            cancelCurrent();
        } else if (key == ::Rml::Input::KI_RETURN || key == ::Rml::Input::KI_NUMPADENTER || key == ::Rml::Input::KI_SPACE) {
            auto *target = event.GetTargetElement();
            if (target && (target->GetId() == "captcha-open" || target->GetId() == "captcha-cancel")) {
                event.StopPropagation();
                target->DispatchEvent("click", {});
            }
        }
        return;
    }
    auto *target = event.GetTargetElement();
    if (!target)
        return;
    const auto &id = target->GetId();
    if (id == "captcha-cancel" || id == "captcha-backdrop") {
        event.StopPropagation();
        if (proxyWarning && id == "captcha-cancel") {
            proxyWarning = false;
            updateModal(currentMessage, true);
        } else {
            cancelCurrent();
        }
    } else if (id == "captcha-open" && state == State::Waiting) {
        event.StopPropagation();
        if (proxyWarning || !currentProxy.enabled()) {
            openBrowser();
            return;
        }
        proxyWarning = true;
        if (auto *title = modal->GetElementById("captcha-title"))
            title->SetInnerRML("Proxy will be bypassed");
        if (auto *message = modal->GetElementById("captcha-message")) {
            message->SetInnerRML(
                "This account connects through <b>" + Platform::htmlEscape(currentProxy.toDisplayString()) +
                "</b>, but the captcha opens in your normal browser, which ignores that proxy.<br/><br/>"
                "The captcha provider will see your real IP address. Continue?");
        }
        target->SetInnerRML("Open Anyway");
        if (view)
            view->requestRender();
    }
}

void BrowserCaptchaResolver::closeModal()
{
    proxyWarning = false;
    if (modal) {
        modal->SetClass("open", false);
        modal->Hide();
        if (view)
            view->requestRender();
    }
}

std::string BrowserCaptchaResolver::harnessUrl() const
{
    return "http://127.0.0.1:" + std::to_string(port) + "/" + nonce;
}

std::string BrowserCaptchaResolver::buildHarnessHtml() const
{
    nlohmann::json cfg = {{"sitekey", current.sitekey}, {"rqdata", current.rqdata},
                          {"invisible", current.shouldServeInvisible}, {"tokenUrl", harnessUrl() + "/token"}};
    auto json = cfg.dump();
    for (size_t pos = 0; (pos = json.find('<', pos)) != std::string::npos; pos += 6)
        json.replace(pos, 1, "\\u003c");
    std::string html = kHarnessTemplate;
    html.replace(html.find("__CONFIG_JSON__"), sizeof("__CONFIG_JSON__") - 1, json);
    return html;
}

} // namespace UI
} // namespace Styx
