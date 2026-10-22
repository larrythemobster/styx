#include "BrowserCaptchaResolver.hpp"

#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>

#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QRandomGenerator>
#include "Platform/Dispatcher.hpp"
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include "Core/Logging.hpp"
#include "UI/Dialogs/ConfirmPopup.hpp"

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

BrowserCaptchaResolver::BrowserCaptchaResolver(QWidget *parentWindow, QObject *parent)
    : QObject(parent), parentWindow(parentWindow)
{
}

BrowserCaptchaResolver::~BrowserCaptchaResolver()
{
    if (modal)
        modal->deleteLater();
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

    QDialog *dlg = new QDialog(parentWindow);
    modal = dlg;
    dlg->setWindowTitle(tr("Verification required"));
    dlg->setModal(true);

    QVBoxLayout *layout = new QVBoxLayout(dlg);
    statusLabel = new QLabel(dlg);
    statusLabel->setWordWrap(true);
    statusLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(statusLabel);

    openButton = new QPushButton(tr("Open in browser"), dlg);
    layout->addWidget(openButton);
    connect(openButton, &QPushButton::clicked, this, [this]() {
        if (!confirmBrowserBypassesProxy())
            return;

        QDesktopServices::openUrl(QUrl(harnessUrl()));
        if (statusLabel)
            statusLabel->setText(tr("Waiting for you to finish in your browser..."));
    });

    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, dlg);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    connect(dlg, &QDialog::rejected, this, [this]() { cancelCurrent(); });

    dlg->show();

    beginChallenge(false);
}

bool BrowserCaptchaResolver::confirmBrowserBypassesProxy()
{
    if (!currentProxy.enabled())
        return true;

    ConfirmPopup dialog(tr("Proxy will be bypassed"),
                        tr("This account connects through <b>%1</b>, but the captcha opens in your "
                           "normal browser, which ignores that proxy.<br><br>"
                           "The captcha provider will see your real IP address. Continue?")
                                .arg(currentProxy.toDisplayString().toHtmlEscaped()),
                        tr("Open Anyway"),
                        modal ? static_cast<QWidget *>(modal.data()) : parentWindow);

    return dialog.exec() == QDialog::Accepted;
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
    if (statusLabel) {
        if (isRetry)
            statusLabel->setText(tr("Failed. Click below to open your browser and try again."));
        else
            statusLabel->setText(tr("Discord wants you to complete a captcha to continue.\n"
                                    "Click below to open it in your browser."));
    }
    if (openButton)
        openButton->setEnabled(true);

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

    if (statusLabel)
        statusLabel->setText(tr("Verifying..."));
    if (openButton)
        openButton->setEnabled(false);

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

void BrowserCaptchaResolver::closeModal()
{
    statusLabel = nullptr;
    openButton = nullptr;
    if (modal) {
        modal->close();
        modal->deleteLater();
        modal = nullptr;
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
