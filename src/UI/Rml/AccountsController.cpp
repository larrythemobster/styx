#include "AccountsController.hpp"

#include "Shell.hpp"
#include "RmlView.hpp"
#include "RmlRuntime.hpp"
#include "ConfirmModal.hpp"

#include "Core/Logging.hpp"
#include "Core/Session.hpp"
#include "Core/TokenStore.hpp"
#include "Core/TokenUtils.hpp"
#include "Discord/CdnUrls.hpp"
#include "Discord/Entities.hpp"
#include "Storage/AccountRepository.hpp"
#include "UI/Accounts/AccountsModel.hpp"

#include "QrImage.hpp"
#include "Platform/Utf8.hpp"
#include <charconv>
#include <format>

namespace {
template<class T = int> T number(std::string_view text) { T value = 0; std::from_chars(text.data(), text.data() + text.size(), value); return value; }
}

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Context.h>

namespace Styx {
namespace UI {
namespace Rml {

AccountsController::AccountsController(Shell &shell) : Controller(shell, nullptr)
{
    track(shell.session()->connectFailed.connect([this](Core::Snowflake, const std::string &reason) {
        if (visible && modalDocument) if (auto *banner = modalDocument->GetElementById("accounts-status-banner")) {
            banner->SetInnerRML("Connection failed: " + escape(reason));
            banner->SetProperty("display", "block");
            requestRender();
        }
    }));
    if (auto *model = shell.accounts()) {
        auto refresh = [this] { if (visible && currentView == ViewMode::Accounts) rebuildAccountsList(); };
        track(model->changed.connect([refresh](int) { refresh(); }));
        track(model->reset.connect(refresh));
    }
}

AccountsController::~AccountsController()
{
    stopQrClient();
    removeListeners();
    if (modalDocument) {
        modalDocument->Close();
        modalDocument = nullptr;
    }
}

void AccountsController::ensureDocument()
{
    if (modalDocument)
        return;

    auto *view = shell.view();
    if (!view)
        return;
    auto *ctx = view->context();
    if (!ctx)
        return;

    modalDocument = ctx->LoadDocument("accounts.rml");
    if (!modalDocument) {
        Core::log(Core::LogLevel::Warning, LogUI) << "AccountsController: could not load accounts.rml";
        return;
    }

    regionRoot = modalDocument;
    setupEventListeners();
}

void AccountsController::showAccounts()
{
    ensureDocument();
    if (!modalDocument)
        return;

    currentView = ViewMode::Accounts;
    switchView(ViewMode::Accounts);
    rebuildAccountsList();
    clearErrors();

    visible = true;
    modalDocument->Show(::Rml::ModalFlag::Modal);
    requestRender(100);
}

void AccountsController::showLogin(LoginTab tab)
{
    ensureDocument();
    if (!modalDocument)
        return;

    currentView = ViewMode::Login;
    switchView(ViewMode::Login);
    switchLoginTab(tab);
    clearErrors();

    // Hide Back to Accounts button if no accounts exist
    if (auto *backBtn = modalDocument->GetElementById("btn-login-back-to-accounts")) {
        const bool hasAccounts = shell.accounts() && shell.accounts()->rowCount() > 0;
        backBtn->SetProperty("display", hasAccounts ? "inline-block" : "none");
    }

    visible = true;
    modalDocument->Show(::Rml::ModalFlag::Modal);
    requestRender(100);
}

void AccountsController::hide()
{
    if (modalDocument)
        modalDocument->Hide();

    visible = false;
    stopQrClient();
    clearErrors();
    requestRender(100);
}

void AccountsController::setupEventListeners()
{
    if (!modalDocument)
        return;

    listen("click", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        if (!target)
            return;

        // Check backdrop click
        if (target->GetId() == "accounts-backdrop") {
            const bool hasAccounts = shell.accounts() && shell.accounts()->rowCount() > 0;
            if (hasAccounts)
                hide();
            return;
        }

        // Check tab click
        if (auto *tabEl = closest(target, "data-tab")) {
            const std::string tabName = attribute(tabEl, "data-tab");
            if (tabName == std::string_view("password"))
                switchLoginTab(LoginTab::Password);
            else if (tabName == std::string_view("qr"))
                switchLoginTab(LoginTab::Qr);
            else if (tabName == std::string_view("token"))
                switchLoginTab(LoginTab::Token);
            return;
        }

        // Check action button click
        if (auto *actionEl = closest(target, "data-action")) {
            const std::string action = attribute(actionEl, "data-action");
            const std::string idStr = attribute(actionEl, "data-id");
            const Core::Snowflake accountId(number<uint64_t>(idStr));

            if (action == std::string_view("close")) {
                hide();
            } else if (action == std::string_view("close-login")) {
                if (shell.accounts() && shell.accounts()->rowCount() > 0)
                    switchView(ViewMode::Accounts);
                else
                    hide();
            } else if (action == std::string_view("back-to-accounts")) {
                switchView(ViewMode::Accounts);
            } else if (action == std::string_view("open-login")) {
                showLogin(LoginTab::Password);
            } else if (action == std::string_view("submit-login")) {
                handleLoginSubmit();
            } else if (action == std::string_view("submit-token")) {
                handleTokenSubmit();
            } else if (action == std::string_view("mfa-verify")) {
                handleMfaVerify();
            } else if (action == std::string_view("mfa-back")) {
                if (modalDocument) {
                    if (auto *pf = modalDocument->GetElementById("password-form-view"))
                        pf->SetProperty("display", "block");
                    if (auto *mf = modalDocument->GetElementById("password-mfa-view"))
                        mf->SetProperty("display", "none");
                }
            } else if (action == std::string_view("mfa-send-sms")) {
                handleMfaSms();
            } else if (action == std::string_view("qr-refresh")) {
                startQrClient();
            } else if (action == std::string_view("close-proxy-edit")) {
                switchView(ViewMode::Accounts);
            } else if (action == std::string_view("save-proxy")) {
                handleSaveProxy();
            } else if (action == std::string_view("close-token-edit")) {
                switchView(ViewMode::Accounts);
            } else if (action == std::string_view("save-token")) {
                handleSaveToken();
            } else if (action == std::string_view("connect")) {
                handleConnect(accountId);
            } else if (action == std::string_view("disconnect")) {
                handleDisconnect(accountId);
            } else if (action == std::string_view("toggle-autoconnect")) {
                handleToggleAutoConnect(accountId);
            } else if (action == std::string_view("open-proxy")) {
                openEditProxy(accountId);
            } else if (action == std::string_view("open-token")) {
                openSetToken(accountId);
            } else if (action == std::string_view("remove")) {
                handleRemoveAccount(accountId);
            } else if (action == std::string_view("close-confirm-logout")) {
                switchView(ViewMode::Accounts);
            } else if (action == std::string_view("do-logout")) {
                if (activeTargetAccountId.isValid()) {
                    auto *m = shell.accounts();
                    if (m) {
                        const int row = m->rowForId(activeTargetAccountId);
                        if (row >= 0)
                            m->removeAccount(row);
                    }
                }
                switchView(ViewMode::Accounts);
            }
        }
    });

    listen("keydown", [this](::Rml::Event &event) {
        const int key = event.GetParameter<int>("key_identifier", 0);
        if (key == ::Rml::Input::KI_ESCAPE) {
            if (currentView == ViewMode::EditProxy || currentView == ViewMode::SetToken || currentView == ViewMode::ConfirmLogout) {
                switchView(ViewMode::Accounts);
                event.StopPropagation();
            } else if (currentView == ViewMode::Login) {
                if (shell.accounts() && shell.accounts()->rowCount() > 0) {
                    switchView(ViewMode::Accounts);
                    event.StopPropagation();
                } else {
                    hide();
                    event.StopPropagation();
                }
            } else {
                hide();
                event.StopPropagation();
            }
        } else if (key == ::Rml::Input::KI_RETURN || key == ::Rml::Input::KI_NUMPADENTER) {
            auto *target = event.GetTargetElement();
            if (!target)
                return;
            const ::Rml::String tagId = target->GetId();
            if (tagId == "login-email" || tagId == "login-password" || tagId == "login-proxy") {
                handleLoginSubmit();
                event.StopPropagation();
            } else if (tagId == "token-input" || tagId == "token-proxy") {
                handleTokenSubmit();
                event.StopPropagation();
            } else if (tagId == "mfa-code-input") {
                handleMfaVerify();
                event.StopPropagation();
            } else if (tagId == "edit-proxy-input") {
                handleSaveProxy();
                event.StopPropagation();
            } else if (tagId == "set-token-input") {
                handleSaveToken();
                event.StopPropagation();
            }
        }
    });

    listen("change", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        if (target && target->GetId() == "mfa-method-select")
            handleMfaMethodChanged();
    });
}

void AccountsController::switchView(ViewMode mode)
{
    currentView = mode;
    if (!modalDocument)
        return;

    auto show = [this](const char *id, bool s) {
        if (auto *el = modalDocument->GetElementById(id))
            el->SetProperty("display", s ? "flex" : "none");
    };

    show("modal-switch-accounts", mode == ViewMode::Accounts);
    show("modal-login", mode == ViewMode::Login);
    show("modal-edit-proxy", mode == ViewMode::EditProxy);
    show("modal-set-token", mode == ViewMode::SetToken);
    show("modal-confirm-logout", mode == ViewMode::ConfirmLogout);

    if (mode == ViewMode::Login && currentTab == LoginTab::Qr) {
        startQrClient();
    } else {
        stopQrClient();
    }

    if (mode == ViewMode::Accounts) {
        rebuildAccountsList();
    }

    requestRender(100);
}

void AccountsController::switchLoginTab(LoginTab tab)
{
    currentTab = tab;
    if (!modalDocument)
        return;

    // Update tab classes
    ::Rml::ElementList tabs;
    if (auto *tabContainer = modalDocument->GetElementById("login-tabs"))
        tabContainer->GetElementsByTagName(tabs, "div");

    for (auto *t : tabs) {
        const std::string tabAttr = attribute(t, "data-tab");
        const bool active = (tab == LoginTab::Password && tabAttr == std::string_view("password")) ||
                            (tab == LoginTab::Qr && tabAttr == std::string_view("qr")) ||
                            (tab == LoginTab::Token && tabAttr == std::string_view("token"));
        t->SetClass("active", active);
    }

    auto show = [this](const char *id, bool s) {
        if (auto *el = modalDocument->GetElementById(id))
            el->SetProperty("display", s ? "block" : "none");
    };

    show("tab-password", tab == LoginTab::Password);
    show("tab-qr", tab == LoginTab::Qr);
    show("tab-token", tab == LoginTab::Token);

    if (tab == LoginTab::Qr)
        startQrClient();
    else
        stopQrClient();

    requestRender(100);
}

void AccountsController::clearErrors()
{
    if (!modalDocument)
        return;

    const char *errorIds[] = { "accounts-status-banner", "login-status-error", "mfa-status-error",
                               "token-status-error", "edit-proxy-error", "set-token-error" };
    for (const char *id : errorIds) {
        if (auto *el = modalDocument->GetElementById(id)) {
            el->SetInnerRML("");
            el->SetProperty("display", "none");
        }
    }
}

void AccountsController::rebuildAccountsList()
{
    if (!modalDocument) return;
    auto *container = modalDocument->GetElementById("accounts-list-container");
    auto *model = shell.accounts();
    if (!container || !model) return;
    if (model->allAccounts().empty()) {
        container->SetInnerRML("<div style='text-align:center;padding:24dp 0'><div class='accounts-title' style='font-size:16dp;margin-bottom:6dp'>No Accounts Saved</div><div class='accounts-subtitle'>Add your Discord account below to get started.</div></div>");
        return;
    }
    std::string html;
    for (const auto &account : model->allAccounts()) {
        const auto id = account.id.toString();
        bool connected = account.state == Core::ConnectionState::Connected;
        bool connecting = account.state == Core::ConnectionState::Connecting;
        const auto avatar = shell.imageSource(Discord::Cdn::userAvatar(account.id, account.avatar, 64), {44, 44});
        const auto name = escape(account.displayName.empty() ? account.username : account.displayName);
        const auto proxy = account.proxy.enabled() ? "Proxy: " + escape(account.proxy.toDisplayString()) : "Direct connection";
        html += std::format(
            "<div class='account-card{0}'><div class='account-card-main'><img class='account-avatar' src='{1}'/>"
            "<div class='account-info'><div class='account-header-line'><span class='account-display-name'>{2}</span>"
            "<span class='account-badge {3}'>{4}</span></div><div class='account-username'>@{5}</div>"
            "<div class='account-proxy-tag'>{6}</div></div></div><div class='account-card-bottom'>"
            "<div class='account-toggle-label' data-action='toggle-autoconnect' data-id='{7}'><span class='autoconnect-text'>Launch at startup</span>"
            "<div class='accounts-switch{8}'><div class='accounts-switch-thumb'></div></div></div><div class='account-actions'>",
            connected ? " active-account" : "", escape(avatar), name,
            connected ? "badge-active" : connecting ? "badge-connecting" : "badge-offline",
            connected ? "Active" : connecting ? "Connecting..." : "Offline",
            escape(account.username), proxy, id, account.autoConnect ? " checked" : "");
        html += std::format("<button class='accounts-btn accounts-btn-sm accounts-btn-{}' data-action='{}' data-id='{}'>{}</button>",
            connected ? "secondary" : "primary", connected ? "disconnect" : "connect", id, connected ? "Disconnect" : "Switch");
        html += std::format(
            "<button class='accounts-btn accounts-btn-sm accounts-btn-secondary' data-action='open-proxy' data-id='{0}'>Proxy</button>"
            "<button class='accounts-btn accounts-btn-sm accounts-btn-secondary' data-action='open-token' data-id='{0}'>Token</button>"
            "<button class='accounts-btn accounts-btn-sm accounts-btn-danger' data-action='remove' data-id='{0}'>Log Out</button></div></div></div>", id);
    }
    container->SetInnerRML(html);
    requestRender(100);
}

void AccountsController::setLoginBusy(bool busy)
{
    if (!modalDocument)
        return;

    if (auto *btn = modalDocument->GetElementById("btn-login-submit")) {
        btn->SetInnerRML(busy ? "Logging in..." : "Log In");
    }
    if (auto *btn = modalDocument->GetElementById("btn-mfa-verify")) {
        btn->SetInnerRML(busy ? "Verifying..." : "Verify");
    }
    requestRender(100);
}

void AccountsController::handleLoginSubmit()
{
    if (!modalDocument)
        return;

    auto *emailInput = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(modalDocument->GetElementById("login-email"));
    auto *passInput = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(modalDocument->GetElementById("login-password"));
    auto *proxyInput = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(modalDocument->GetElementById("login-proxy"));
    auto *errLabel = modalDocument->GetElementById("login-status-error");

    if (!emailInput || !passInput || !errLabel)
        return;

    const std::string email(Platform::trim(emailInput->GetValue()));
    const std::string pass = std::string(passInput->GetValue());

    if (email.empty() || pass.empty()) {
        errLabel->SetInnerRML("Enter your email or phone number, and your password.");
        errLabel->SetProperty("display", "block");
        return;
    }

    Core::ProxyConfig proxy;
    if (proxyInput) {
        const std::string proxyStr(Platform::trim(proxyInput->GetValue()));
        if (!proxyStr.empty()) {
            auto parsed = Core::ProxyConfig::parse(proxyStr);
            if (!parsed) {
                errLabel->SetInnerRML("Invalid proxy format. Enter scheme://[user:pass@]host:port (http, https, socks5).");
                errLabel->SetProperty("display", "block");
                return;
            }
            proxy = *parsed;
        }
    }

    errLabel->SetProperty("display", "none");
    setLoginBusy(true);

    loginClient = std::make_unique<Discord::LoginClient>(proxy, shell.session()->getCaptchaResolver());

    track((loginClient)->smsSent.connect([this](bool ok) {
        setLoginBusy(false);
        if (modalDocument) {
            if (auto *errEl = modalDocument->GetElementById("mfa-status-error")) {
                errEl->SetInnerRML(ok ? "SMS code sent to your phone." : "Failed to send SMS code. Please try again.");
                errEl->SetProperty("display", "block");
            }
        }
    }));

    track((loginClient)->mfaRequired.connect([this](const Discord::MfaChallenge &challenge) {
        setLoginBusy(false);
        currentChallenge = challenge;
    
        if (!modalDocument)
            return;
    
        if (auto *pf = modalDocument->GetElementById("password-form-view"))
            pf->SetProperty("display", "none");
        if (auto *mf = modalDocument->GetElementById("password-mfa-view"))
            mf->SetProperty("display", "block");
    
        if (auto *t = modalDocument->GetElementById("mfa-title-text")) {
            if (challenge.sms && !challenge.totp && !challenge.backup)
                t->SetInnerRML("SMS Verification Code");
            else
                t->SetInnerRML("Two-Factor Authentication");
        }
    
        if (auto *sel = rmlui_dynamic_cast<::Rml::ElementFormControlSelect *>(modalDocument->GetElementById("mfa-method-select"))) {
            // Re-populate options
            std::string selHtml;
            if (challenge.totp)
                selHtml += std::format("<option value=\"{}\">Authenticator App</option>", static_cast<int>(Discord::MfaMethod::Totp));
            if (challenge.backup)
                selHtml += std::format("<option value=\"{}\">Backup Code</option>", static_cast<int>(Discord::MfaMethod::Backup));
            if (challenge.sms)
                selHtml += std::format("<option value=\"{}\">Text Message (SMS)</option>", static_cast<int>(Discord::MfaMethod::Sms));
            sel->SetInnerRML(selHtml);
        }
    
        if (auto *wrap = modalDocument->GetElementById("mfa-method-wrapper")) {
            int count = (challenge.totp ? 1 : 0) + (challenge.backup ? 1 : 0) + (challenge.sms ? 1 : 0);
            wrap->SetProperty("display", count > 1 ? "block" : "none");
        }
    
        handleMfaMethodChanged();
    }));

    track((loginClient)->authenticated.connect([this, proxy](const std::string &token) {
        setLoginBusy(false);
        if (addAccountWithToken(token, std::string(), proxy))
            switchView(ViewMode::Accounts);
    }));

    track((loginClient)->failed.connect([this](Discord::LoginError error) {
        setLoginBusy(false);
        std::string msg;
        switch (error) {
        case Discord::LoginError::Network:
            msg = std::string("Couldn't reach Discord. Check your network connection.");
            break;
        case Discord::LoginError::InvalidCredentials:
            msg = std::string("The email or password you entered is incorrect.");
            break;
        case Discord::LoginError::CaptchaRequired:
            msg = std::string("Discord requested a captcha. Captcha solve was cancelled or failed.");
            break;
        case Discord::LoginError::InvalidMfaCode:
            msg = std::string("The 2FA verification code didn't work. Check and try again.");
            break;
        case Discord::LoginError::TooManyRequests:
            msg = std::string("Too many login attempts. Please wait a bit and try again.");
            break;
        case Discord::LoginError::AccountLocked:
            msg = std::string("This account is disabled. Enable it on discord.com and try again.");
            break;
        case Discord::LoginError::Suspended:
            msg = std::string("This account has been suspended.");
            break;
        case Discord::LoginError::RequiresVerification:
            msg = std::string("Discord requires verification. Check your email or messages.");
            break;
        case Discord::LoginError::PasskeyOnly:
            msg = std::string("This account only allows passkeys. Use QR code or token login instead.");
            break;
        case Discord::LoginError::Unknown:
            msg = std::string("Login failed. Please try again.");
            break;
        }
    
        if (modalDocument) {
            auto *pf = modalDocument->GetElementById("password-form-view");
            const bool inMfa = (pf && pf->GetProperty<::Rml::String>("display") == "none");
            auto *errEl = modalDocument->GetElementById(inMfa ? "mfa-status-error" : "login-status-error");
            if (errEl) {
                errEl->SetInnerRML(escape(msg));
                errEl->SetProperty("display", "block");
            }
        }
    }));

    loginClient->login(email, pass);
}

Discord::MfaMethod AccountsController::currentMfaMethod() const
{
    if (!modalDocument)
        return Discord::MfaMethod::Totp;

    auto *sel = rmlui_dynamic_cast<::Rml::ElementFormControlSelect *>(modalDocument->GetElementById("mfa-method-select"));
    if (!sel)
        return Discord::MfaMethod::Totp;

    const int val = number(sel->GetValue());
    return static_cast<Discord::MfaMethod>(val);
}

void AccountsController::handleMfaMethodChanged()
{
    if (!modalDocument)
        return;

    const bool isSms = (currentMfaMethod() == Discord::MfaMethod::Sms);
    if (auto *smsBtn = modalDocument->GetElementById("btn-mfa-sms"))
        smsBtn->SetProperty("display", isSms ? "inline-block" : "none");
}

void AccountsController::handleMfaSms()
{
    if (!loginClient)
        return;
    setLoginBusy(true);
    loginClient->requestSmsCode();
}

void AccountsController::handleMfaVerify()
{
    if (!loginClient || !modalDocument)
        return;

    auto *codeEl = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(modalDocument->GetElementById("mfa-code-input"));
    auto *errEl = modalDocument->GetElementById("mfa-status-error");
    if (!codeEl)
        return;

    const std::string code(Platform::trim(codeEl->GetValue()));
    if (code.empty()) {
        if (errEl) {
            errEl->SetInnerRML("Enter your verification code.");
            errEl->SetProperty("display", "block");
        }
        return;
    }

    if (errEl)
        errEl->SetProperty("display", "none");

    setLoginBusy(true);
    loginClient->submitMfa(currentMfaMethod(), code);
}

void AccountsController::startQrClient()
{
    stopQrClient();

    Core::ProxyConfig proxy;
    if (modalDocument) {
        if (auto *proxyEl = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(modalDocument->GetElementById("qr-proxy"))) {
            const std::string pStr(Platform::trim(proxyEl->GetValue()));
            if (!pStr.empty()) {
                auto parsed = Core::ProxyConfig::parse(pStr);
                if (!parsed) {
                    if (auto *status = modalDocument->GetElementById("qr-status-text"))
                        status->SetInnerRML("Invalid proxy address. Correct it before refreshing the QR code.");
                    return;
                }
                proxy = *parsed;
            }
        }
        if (auto *st = modalDocument->GetElementById("qr-status-text"))
            st->SetInnerRML("Connecting to remote authentication...");
    }

    qrStarted = true;
    qrClient = std::make_unique<Discord::RemoteAuthClient>(proxy, shell.session()->getCaptchaResolver());
    track(qrClient->fingerprintReady.connect([this](const std::string &fingerprint) { renderQr(fingerprint); }));

    track((qrClient)->pendingTicket.connect([this](const std::string &, const std::string &name, const std::string &) {
        lastQrUsername = name;
        if (modalDocument) {
            if (auto *st = modalDocument->GetElementById("qr-status-text"))
                st->SetInnerRML("Scan detected for " + escape(name) + "! Please confirm on your mobile app.");
        }
        requestRender(100);
    }));

    track((qrClient)->authenticated.connect([this, proxy](const std::string &token) {
        const std::string uname = lastQrUsername;
        stopQrClient();
        if (addAccountWithToken(token, uname, proxy))
            switchView(ViewMode::Accounts);
    }));

    track((qrClient)->failed.connect([this](Discord::RemoteAuthError) {
        if (modalDocument) {
            if (auto *st = modalDocument->GetElementById("qr-status-text"))
                st->SetInnerRML("QR authentication session expired or failed. Click 'Refresh QR Code' to try again.");
        }
        requestRender(100);
    }));

    track((qrClient)->canceled.connect([this]() {
        if (modalDocument) {
            if (auto *st = modalDocument->GetElementById("qr-status-text"))
                st->SetInnerRML("Login was canceled on your phone.");
        }
        requestRender(100);
    }));

    qrClient->start();
}

void AccountsController::stopQrClient()
{
    if (qrClient) {
        qrClient->stop();
        auto *retired = qrClient.release();
        Platform::Dispatcher::main().post([retired] { delete retired; });
    }
    qrStarted = false;
    Runtime::instance().clearFrame(std::string("ra-qr"));
}

void AccountsController::renderQr(const std::string &fingerprint)
{
    const auto image = qrImage("https://discord.com/ra/" + fingerprint, 256);
    if (image) Runtime::instance().setFrame("ra-qr", *image);
    if (modalDocument)
        if (auto *st = modalDocument->GetElementById("qr-status-text"))
            st->SetInnerRML(image ? "Ready to scan" : "Failed to generate QR code.");
    requestRender(100);
}

void AccountsController::handleTokenSubmit()
{
    if (!modalDocument)
        return;

    auto *tokenInput = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(modalDocument->GetElementById("token-input"));
    auto *proxyInput = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(modalDocument->GetElementById("token-proxy"));
    auto *errLabel = modalDocument->GetElementById("token-status-error");

    if (!tokenInput || !errLabel)
        return;

    const std::string token(Platform::trim(tokenInput->GetValue()));
    if (token.empty()) {
        errLabel->SetInnerRML("Please enter a token.");
        errLabel->SetProperty("display", "block");
        return;
    }

    Core::ProxyConfig proxy;
    if (proxyInput) {
        const std::string proxyStr(Platform::trim(proxyInput->GetValue()));
        if (!proxyStr.empty()) {
            auto parsed = Core::ProxyConfig::parse(proxyStr);
            if (!parsed) {
                errLabel->SetInnerRML("Invalid proxy format. Enter scheme://[user:pass@]host:port (http, https, socks5).");
                errLabel->SetProperty("display", "block");
                return;
            }
            proxy = *parsed;
        }
    }

    const Core::Snowflake userId = Core::TokenUtils::getIdAndCheckToken(token);
    if (!userId.isValid()) {
        errLabel->SetInnerRML("Invalid token: could not extract a valid Discord user ID.");
        errLabel->SetProperty("display", "block");
        return;
    }

    errLabel->SetProperty("display", "none");
    if (addAccountWithToken(token, std::string(), proxy))
        switchView(ViewMode::Accounts);
}

bool AccountsController::addAccountWithToken(const std::string &token, const std::string &username, const Core::ProxyConfig &proxy)
{
    auto fail = [this](const std::string &message) {
        if (modalDocument) {
            const char *errorId = currentTab == LoginTab::Token ? "token-status-error"
                                 : currentTab == LoginTab::Qr ? "qr-status-text" : "login-status-error";
            if (currentTab == LoginTab::Password) {
                auto *form = modalDocument->GetElementById("password-form-view");
                if (form && form->GetProperty<::Rml::String>("display") == "none")
                    errorId = "mfa-status-error";
            }
            if (auto *error = modalDocument->GetElementById(errorId)) {
                error->SetInnerRML(escape(message));
                error->SetProperty("display", "block");
            }
        }
        return false;
    };
    const Core::Snowflake userId = Core::TokenUtils::getIdAndCheckToken(token);
    if (!userId.isValid())
        return fail(std::string("The login token does not contain a valid account ID."));

    Core::AccountInfo acc;
    acc.id = userId;
    acc.token = token;
    acc.username = username.empty() ? std::string("User") : username;
    acc.displayName = acc.username;
    acc.proxy = proxy;

    auto *model = shell.accounts();
    if (!model)
        return fail(std::string("Account storage is unavailable."));

    const auto result = model->addAccount(acc);
    if (!result.success())
        return fail("Failed to save account: " + result.error);

    // Connect if this was the very first account added
    if (model->rowCount() == 1) {
        shell.session()->connectAccount(userId);
    }
    return true;
}

void AccountsController::handleConnect(Core::Snowflake accountId)
{
    if (!accountId.isValid())
        return;
    shell.session()->connectAccount(accountId);
}

void AccountsController::handleDisconnect(Core::Snowflake accountId)
{
    if (!accountId.isValid())
        return;
    shell.session()->disconnectAccount(accountId);
}

void AccountsController::handleToggleAutoConnect(Core::Snowflake accountId)
{
    auto *model = shell.accounts();
    if (!model)
        return;
    const int row = model->rowForId(accountId);
    if (row < 0)
        return;
    const auto *acc = model->accountAt(row);
    if (!acc)
        return;
    model->setAutoConnect(row, !acc->autoConnect);
    rebuildAccountsList();
}

void AccountsController::handleRemoveAccount(Core::Snowflake accountId)
{
    activeTargetAccountId = accountId;
    auto *model = shell.accounts();
    if (!model || !modalDocument)
        return;

    const auto *acc = model->accountById(accountId);
    const std::string name = acc ? (acc->displayName.empty() ? acc->username : acc->displayName) : std::string("this account");
    if (auto *desc = modalDocument->GetElementById("confirm-logout-desc"))
        desc->SetInnerRML("Are you sure you want to log out of " + escape(name) + "?");

    switchView(ViewMode::ConfirmLogout);
}

void AccountsController::openEditProxy(Core::Snowflake accountId)
{
    activeTargetAccountId = accountId;
    auto *model = shell.accounts();
    if (!model || !modalDocument)
        return;

    const auto *acc = model->accountById(accountId);
    if (!acc)
        return;

    if (auto *sub = modalDocument->GetElementById("edit-proxy-account-name")) {
        const std::string name = acc->displayName.empty() ? acc->username : acc->displayName;
        sub->SetInnerRML("Proxy for " + escape(name));
    }

    if (auto *inp = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(modalDocument->GetElementById("edit-proxy-input"))) {
        inp->SetValue(acc->proxy.toString());
    }

    if (auto *err = modalDocument->GetElementById("edit-proxy-error")) {
        err->SetInnerRML("");
        err->SetProperty("display", "none");
    }

    switchView(ViewMode::EditProxy);
}

void AccountsController::handleSaveProxy()
{
    if (!modalDocument)
        return;

    auto *inp = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(modalDocument->GetElementById("edit-proxy-input"));
    auto *err = modalDocument->GetElementById("edit-proxy-error");
    if (!inp)
        return;

    const std::string proxyStr(Platform::trim(inp->GetValue()));
    auto parsed = Core::ProxyConfig::parse(proxyStr);
    if (!parsed) {
        if (err) {
            err->SetInnerRML("Invalid proxy format. Enter scheme://[user:pass@]host:port (http, https, socks5).");
            err->SetProperty("display", "block");
        }
        return;
    }

    auto *model = shell.accounts();
    if (!model)
        return;
    const auto *account = model->accountById(activeTargetAccountId);
    if (!account)
        return;
    if (account->state != Core::ConnectionState::Disconnected) {
        if (err) {
            err->SetInnerRML("Disconnect this account before changing its proxy.");
            err->SetProperty("display", "block");
        }
        return;
    }
    model->setProxy(model->rowForId(activeTargetAccountId), *parsed);
    switchView(ViewMode::Accounts);
}

void AccountsController::openSetToken(Core::Snowflake accountId)
{
    activeTargetAccountId = accountId;
    auto *model = shell.accounts();
    if (!model || !modalDocument)
        return;

    const auto *acc = model->accountById(accountId);
    if (!acc)
        return;

    if (auto *sub = modalDocument->GetElementById("set-token-account-name")) {
        const std::string name = acc->displayName.empty() ? acc->username : acc->displayName;
        sub->SetInnerRML("Update token for " + escape(name));
    }

    if (auto *inp = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(modalDocument->GetElementById("set-token-input"))) {
        inp->SetValue("");
    }

    if (auto *err = modalDocument->GetElementById("set-token-error")) {
        err->SetInnerRML("");
        err->SetProperty("display", "none");
    }

    switchView(ViewMode::SetToken);
}

void AccountsController::handleSaveToken()
{
    if (!modalDocument)
        return;

    auto *inp = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(modalDocument->GetElementById("set-token-input"));
    auto *err = modalDocument->GetElementById("set-token-error");
    if (!inp)
        return;

    const std::string newToken(Platform::trim(inp->GetValue()));
    if (newToken.empty()) {
        if (err) {
            err->SetInnerRML("Please enter the new token.");
            err->SetProperty("display", "block");
        }
        return;
    }

    const Core::Snowflake tokenUserId = Core::TokenUtils::getIdAndCheckToken(newToken);
    if (!tokenUserId.isValid()) {
        if (err) {
            err->SetInnerRML("The token couldn't be parsed. Make sure you copied the entire token.");
            err->SetProperty("display", "block");
        }
        return;
    }

    if (tokenUserId != activeTargetAccountId) {
        if (err) {
            err->SetInnerRML("This token belongs to user ID " + tokenUserId.toString() + ", but this account is " + activeTargetAccountId.toString() + ".");
            err->SetProperty("display", "block");
        }
        return;
    }

    const auto saved = Core::TokenStore::saveToken(activeTargetAccountId, newToken);
    if (!saved.success()) {
        if (err) {
            err->SetInnerRML("Failed to save token: " + escape(saved.error));
            err->SetProperty("display", "block");
        }
        return;
    }

    Storage::AccountRepository().clearHeartbeatSession(activeTargetAccountId);
    switchView(ViewMode::Accounts);
}

} // namespace Rml
} // namespace UI
} // namespace Styx
