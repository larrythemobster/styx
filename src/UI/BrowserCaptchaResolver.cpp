#include "BrowserCaptchaResolver.hpp"

#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

#include "Core/Logging.hpp"
#include "Platform/Dispatcher.hpp"
#include "UI/Rml/RmlView.hpp"
#include "UI/Rml/Shell.hpp"

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

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using native_socket_t = SOCKET;
constexpr native_socket_t kInvalidNativeSocket = INVALID_SOCKET;
inline int closeNativeSocket(native_socket_t s) { return ::closesocket(s); }
inline bool setNonBlocking(native_socket_t s) {
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
}
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
using native_socket_t = int;
constexpr native_socket_t kInvalidNativeSocket = -1;
inline int closeNativeSocket(native_socket_t s) { return ::close(s); }
inline bool setNonBlocking(native_socket_t s) {
    int flags = fcntl(s, F_GETFL, 0);
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif

static void sendHttp(intptr_t clientSock, int code, const QByteArray &contentType,
                     const QByteArray &body)
{
    QByteArray status;
    switch (code) {
    case 200:
        status = "200 OK";
        break;
    case 403:
        status = "403 Forbidden";
        break;
    case 404:
        status = "404 Not Found";
        break;
    default:
        status = "400 Bad Request";
        break;
    }

    QByteArray resp = "HTTP/1.1 " + status + "\r\n";
    resp += "Content-Type: " + contentType + "\r\n";
    resp += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    resp += "Cache-Control: no-store\r\n";
    resp += "Connection: close\r\n";
    resp += "\r\n";
    resp += body;

    ::send(static_cast<native_socket_t>(clientSock), resp.constData(), static_cast<int>(resp.size()), 0);
    closeNativeSocket(static_cast<native_socket_t>(clientSock));
}

BrowserCaptchaResolver::BrowserCaptchaResolver(Shell &shell, QObject *parent)
    : QObject(parent), shell(shell)
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
    QMetaObject::invokeMethod(
            this,
            [this, challenge, proxy, cb]() {
                // reuse since it got rejected
                if (state == State::Verifying) {
                    current = challenge;
                    currentProxy = proxy;
                    currentCb = cb;
                    beginChallenge(true);
                    return;
                }

                queue.push_back({ challenge, proxy, cb });
                if (state == State::Idle)
                    startNext();
            },
            Qt::QueuedConnection);
}

void BrowserCaptchaResolver::notifyConcluded()
{
    QMetaObject::invokeMethod(
            this,
            [this]() {
                if (state != State::Verifying)
                    return;
                state = State::Idle;
                currentCb = nullptr;
                stopTtl();
                teardownServer();
                closeModal();
                if (!queue.empty())
                    startNext();
            },
            Qt::QueuedConnection);
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
    if (current.service != QLatin1String("hcaptcha")) {
        qCWarning(LogUI) << "Unsupported captcha service:" << current.service;
        Discord::CaptchaResolver::Callback cb = std::move(currentCb);
        currentCb = nullptr;
        if (cb)
            cb(std::nullopt);
        startNext();
        return;
    }

    state = State::Waiting;
    if (!ensureModal()) {
        qCWarning(LogUI) << "Unable to open captcha prompt";
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
    nonce = QUuid::createUuid().toString(QUuid::WithoutBraces) + QString::number(QRandomGenerator::global()->generate64(), 16);

    native_socket_t sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == kInvalidNativeSocket) {
        qCWarning(LogUI) << "Failed to create captcha loopback socket";
        cancelCurrent();
        return;
    }

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bindAddr.sin_port = 0;

    if (::bind(sock, reinterpret_cast<sockaddr *>(&bindAddr), sizeof(bindAddr)) != 0 ||
        ::listen(sock, 5) != 0) {
        qCWarning(LogUI) << "Failed to bind/listen captcha loopback server";
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
                    ? tr("Failed. Click below to open your browser and try again.")
                    : tr("Discord wants you to complete a captcha to continue. "
                         "Click below to open it in your browser."),
                true);

    stopTtl();
    ttlTimer = new QTimer(this);
    ttlTimer->setSingleShot(true);
    connect(ttlTimer, &QTimer::timeout, this, [this]() {
        qCWarning(LogUI) << "Captcha timed out";
        cancelCurrent();
    });
    ttlTimer->start(120000);
}

void BrowserCaptchaResolver::deliverSolution(const Discord::CaptchaSolution &solution)
{
    if (state != State::Waiting)
        return;

    state = State::Verifying;

    teardownServer();
    stopTtl();

    proxyWarning = false;
    updateModal(tr("Verifying..."), false);

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
        buffers.insert(static_cast<intptr_t>(client), QByteArray());
    }

    QList<intptr_t> clients = buffers.keys();
    for (intptr_t clientSock : clients) {
        char buf[4096];
        int n = ::recv(static_cast<native_socket_t>(clientSock), buf, sizeof(buf), 0);
        if (n > 0) {
            buffers[clientSock].append(buf, n);
            handleClient(clientSock);
        } else if (n == 0) {
            closeNativeSocket(static_cast<native_socket_t>(clientSock));
            buffers.remove(clientSock);
        } else {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK) {
                closeNativeSocket(static_cast<native_socket_t>(clientSock));
                buffers.remove(clientSock);
            }
#else
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                closeNativeSocket(static_cast<native_socket_t>(clientSock));
                buffers.remove(clientSock);
            }
#endif
        }
    }
}

void BrowserCaptchaResolver::handleClient(intptr_t clientSock)
{
    QByteArray &buf = buffers[clientSock];

    if (buf.size() > 64 * 1024) {
        buffers.remove(clientSock);
        sendHttp(clientSock, 400, "text/plain", "too large");
        return;
    }

    const int headerEnd = buf.indexOf("\r\n\r\n");
    if (headerEnd < 0)
        return;

    const QByteArray header = buf.left(headerEnd);
    const QList<QByteArray> lines = header.split('\n');
    if (lines.isEmpty())
        return;

    const QList<QByteArray> reqLine = lines.first().trimmed().split(' ');
    if (reqLine.size() < 2) {
        buffers.remove(clientSock);
        sendHttp(clientSock, 400, "text/plain", "bad request");
        return;
    }
    const QByteArray method = reqLine[0];
    const QByteArray path = reqLine[1];

    int contentLength = 0;
    QByteArray origin;
    for (const QByteArray &line : lines) {
        const QByteArray l = line.trimmed();
        if (l.toLower().startsWith("content-length:"))
            contentLength = l.mid(15).trimmed().toInt();
        else if (l.toLower().startsWith("origin:"))
            origin = l.mid(7).trimmed();
    }

    const int bodyStart = headerEnd + 4;
    if (buf.size() - bodyStart < contentLength)
        return;

    const QByteArray body = buf.mid(bodyStart, contentLength);
    buffers.remove(clientSock);

    const QByteArray expectedPath = "/" + nonce.toUtf8();
    if (nonce.isEmpty() || !path.startsWith(expectedPath)) {
        sendHttp(clientSock, 404, "text/plain", "not found");
        return;
    }

    if (method == "GET" && path == expectedPath) {
        sendHttp(clientSock, 200, "text/html; charset=utf-8", buildHarnessHtml());
        return;
    }

    if (method == "POST" && path == expectedPath + "/token") {
        const QByteArray expectedOrigin = "http://127.0.0.1:" + QByteArray::number(port);
        if (!origin.isEmpty() && origin != expectedOrigin) {
            sendHttp(clientSock, 403, "text/plain", "bad origin");
            return;
        }

        sendHttp(clientSock, 200, "application/json", "{\"ok\":true}");

        const QJsonObject obj = QJsonDocument::fromJson(body).object();
        const QString token = obj.value("token").toString();
        if (token.isEmpty()) {
            qCWarning(LogUI) << "Captcha harness reported an error:"
                             << obj.value("error").toString();
            updateModal(tr("The captcha could not be completed. Open your browser and try again."), true);
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
    for (intptr_t clientSock : buffers.keys()) {
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
    if (ttlTimer) {
        ttlTimer->stop();
        ttlTimer->deleteLater();
        ttlTimer = nullptr;
    }
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
        QMetaObject::invokeMethod(this, [this]() {
            if (!modal && state != State::Idle)
                cancelCurrent();
        }, Qt::QueuedConnection);
    }
}

bool BrowserCaptchaResolver::ensureModal()
{
    if (modal)
        return true;
    if (viewDestroyed)
        return false;
    if (!view) {
        view = shell.view();
        if (view)
            connect(view, &QObject::destroyed, this, [this]() { viewDestroyed = true; });
    }
    if (!view || !view->context())
        return false;
    modal = view->context()->LoadDocument(":/ui/captcha_modal.rml");
    if (!modal)
        return false;
    modal->AddEventListener("click", &modalListener);
    modal->AddEventListener("keydown", &modalListener);
    return true;
}

void BrowserCaptchaResolver::updateModal(const QString &message, bool canOpen)
{
    currentMessage = message;
    if (!modal)
        return;
    if (auto *title = modal->GetElementById("captcha-title"))
        title->SetInnerRML(tr("Verification required").toHtmlEscaped().toStdString());
    if (auto *element = modal->GetElementById("captcha-message"))
        element->SetInnerRML(message.toHtmlEscaped().toStdString());
    if (auto *button = modal->GetElementById("captcha-open")) {
        button->SetClass("disabled", !canOpen);
        button->SetPseudoClass("disabled", !canOpen);
    }
    if (auto *button = modal->GetElementById("captcha-open"))
        button->SetInnerRML(tr("Open in browser").toHtmlEscaped().toStdString());
    if (view)
        view->requestRender();
}

void BrowserCaptchaResolver::openBrowser()
{
    if (state != State::Waiting)
        return;
    proxyWarning = false;
    if (QDesktopServices::openUrl(QUrl(harnessUrl())))
        updateModal(tr("Waiting for you to finish in your browser..."), true);
    else
        updateModal(tr("Could not open the browser. Please try again."), true);
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
            title->SetInnerRML(tr("Proxy will be bypassed").toHtmlEscaped().toStdString());
        if (auto *message = modal->GetElementById("captcha-message")) {
            message->SetInnerRML(
                tr("This account connects through <b>%1</b>, but the captcha opens in your "
                   "normal browser, which ignores that proxy.<br/><br/>"
                   "The captcha provider will see your real IP address. Continue?")
                    .arg(currentProxy.toDisplayString().toHtmlEscaped()).toStdString());
        }
        target->SetInnerRML(tr("Open Anyway").toHtmlEscaped().toStdString());
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

QString BrowserCaptchaResolver::harnessUrl() const
{
    return QStringLiteral("http://127.0.0.1:%1/%2").arg(port).arg(nonce);
}

QByteArray BrowserCaptchaResolver::buildHarnessHtml() const
{
    QJsonObject cfg;
    cfg["sitekey"] = current.sitekey;
    cfg["rqdata"] = current.rqdata;
    cfg["invisible"] = current.shouldServeInvisible;
    cfg["tokenUrl"] = QStringLiteral("http://127.0.0.1:%1/%2/token").arg(port).arg(nonce);

    const QByteArray cfgJson = QJsonDocument(cfg).toJson(QJsonDocument::Compact);

    QByteArray html = kHarnessTemplate;
    html.replace(QByteArrayLiteral("__CONFIG_JSON__"), cfgJson);
    return html;
}

} // namespace UI
} // namespace Styx
