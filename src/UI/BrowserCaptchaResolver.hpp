#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <RmlUi/Core/EventListener.h>
#include <deque>
#include <optional>

#include "Discord/CaptchaResolver.hpp"

class QTimer;

namespace Rml {
class Element;
class ElementDocument;
class Event;
}

namespace Styx {
namespace UI {

class Shell;
namespace Rml {
class View;
}

class BrowserCaptchaResolver : public QObject, public Discord::CaptchaResolver
{
    Q_OBJECT
public:
    explicit BrowserCaptchaResolver(Shell &shell, QObject *parent = nullptr);
    ~BrowserCaptchaResolver() override;

    void resolve(const Discord::CaptchaChallenge &challenge,
                 const Core::ProxyConfig &proxy,
                 Discord::CaptchaResolver::Callback cb) override;
    void notifyConcluded() override;

private:
    enum class State {
        Idle, // nothing is happening
        Waiting, // modal open, waiting for a solution
        Verifying, // token delivered, waiting for replay of original request
    };

    struct PendingJob
    {
        Discord::CaptchaChallenge challenge;
        Core::ProxyConfig proxy;
        Discord::CaptchaResolver::Callback cb;
    };

    void startNext();
    void beginChallenge(bool isRetry);
    void deliverSolution(const Discord::CaptchaSolution &solution);
    void cancelCurrent();
    void pollSockets();
    void handleClient(intptr_t clientSock);

    bool ensureModal();
    void updateModal(const QString &message, bool canOpen);
    void openBrowser();
    void processEvent(::Rml::Event &event);
    void documentDetached(::Rml::Element *element);

    struct ModalListener final : ::Rml::EventListener
    {
        explicit ModalListener(BrowserCaptchaResolver &owner) : owner(owner) {}
        void ProcessEvent(::Rml::Event &event) override;
        void OnDetach(::Rml::Element *element) override;
        BrowserCaptchaResolver &owner;
    };
    void teardownServer();
    void stopTtl();
    void closeModal();

    QByteArray buildHarnessHtml() const;
    QString harnessUrl() const;

    Shell &shell;

    std::deque<PendingJob> queue;
    State state = State::Idle;

    intptr_t serverSocket{-1};
    uint64_t timerId{0};
    quint16 port = 0;
    QString nonce;
    Discord::CaptchaChallenge current;
    Core::ProxyConfig currentProxy;
    Discord::CaptchaResolver::Callback currentCb;

    ::Rml::ElementDocument *modal = nullptr;
    QPointer<Rml::View> view;
    bool viewDestroyed = false;
    ModalListener modalListener{*this};
    bool proxyWarning = false;
    bool shuttingDown = false;
    QString currentMessage;
    QTimer *ttlTimer = nullptr;

    QHash<intptr_t, QByteArray> buffers;
};

} // namespace UI
} // namespace Styx
