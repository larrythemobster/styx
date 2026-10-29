#pragma once

#include <RmlUi/Core/EventListener.h>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>

#include "Discord/CaptchaResolver.hpp"
#include "Platform/Trackable.hpp"
#include "Platform/Timer.hpp"

namespace Rml { class Element; class ElementDocument; class Event; }
namespace Styx::UI {
class Shell;
namespace Rml { class View; }

class BrowserCaptchaResolver : public Platform::Trackable, public Discord::CaptchaResolver {
public:
    explicit BrowserCaptchaResolver(Shell &shell);
    ~BrowserCaptchaResolver() override;
    void resolve(const Discord::CaptchaChallenge &, const Core::ProxyConfig &, Callback) override;
    void notifyConcluded() override;
private:
    enum class State { Idle, Waiting, Verifying };
    struct PendingJob { Discord::CaptchaChallenge challenge; Core::ProxyConfig proxy; Callback cb; };
    void startNext();
    void beginChallenge(bool isRetry);
    void deliverSolution(const Discord::CaptchaSolution &solution);
    void cancelCurrent();
    void pollSockets();
    void handleClient(intptr_t clientSock);
    bool ensureModal();
    void updateModal(const std::string &message, bool canOpen);
    void openBrowser();
    void processEvent(::Rml::Event &event);
    void documentDetached(::Rml::Element *element);
    struct ModalListener final : ::Rml::EventListener {
        explicit ModalListener(BrowserCaptchaResolver &owner) : owner(owner) {}
        void ProcessEvent(::Rml::Event &event) override;
        void OnDetach(::Rml::Element *element) override;
        BrowserCaptchaResolver &owner;
    };
    void teardownServer();
    void stopTtl();
    void closeModal();
    std::string buildHarnessHtml() const;
    std::string harnessUrl() const;

    Shell &shell;
    std::deque<PendingJob> queue;
    State state = State::Idle;
    intptr_t serverSocket{-1};
    uint64_t timerId{0};
    uint16_t port = 0;
    std::string nonce;
    Discord::CaptchaChallenge current;
    Core::ProxyConfig currentProxy;
    Callback currentCb;
    ::Rml::ElementDocument *modal = nullptr;
    Rml::View *view = nullptr;
    ModalListener modalListener{*this};
    bool proxyWarning = false;
    bool shuttingDown = false;
    std::string currentMessage;
    Platform::Timer ttlTimer;
    std::unordered_map<intptr_t, std::string> buffers;
};
}
