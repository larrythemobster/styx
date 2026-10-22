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

#include "qrcodegen.hpp"

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

AccountsController::AccountsController(Shell &shell, QObject *parent)
    : Controller(shell, nullptr, parent)
{
    if (auto *sess = shell.session()) {
        connect(sess, &Core::Session::connectFailed, this, [this](Core::Snowflake, const QString &reason) {
            if (visible && modalDocument) {
                if (auto *banner = modalDocument->GetElementById("accounts-status-banner")) {
                    banner->SetInnerRML(QStringLiteral("Connection failed: %1").arg(escape(reason)).toUtf8().constData());
                    banner->SetProperty("display", "block");
                }
            }
        });
    }

    if (auto *model = shell.accounts()) {
        connect(model, &QAbstractItemModel::dataChanged, this, [this]() {
            if (visible && currentView == ViewMode::Accounts)
                rebuildAccountsList();
        });
        connect(model, &QAbstractItemModel::rowsInserted, this, [this]() {
            if (visible && currentView == ViewMode::Accounts)
                rebuildAccountsList();
        });
        connect(model, &QAbstractItemModel::rowsRemoved, this, [this]() {
            if (visible && currentView == ViewMode::Accounts)
                rebuildAccountsList();
        });
    }
}

AccountsController::~AccountsController()
{
    stopQrClient();
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
        qCWarning(LogUI) << "AccountsController: could not load accounts.rml";
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
            const QString tabName = attribute(tabEl, "data-tab");
            if (tabName == QLatin1String("password"))
                switchLoginTab(LoginTab::Password);
            else if (tabName == QLatin1String("qr"))
                switchLoginTab(LoginTab::Qr);
            else if (tabName == QLatin1String("token"))
                switchLoginTab(LoginTab::Token);
            return;
        }

        // Check action button click
        if (auto *actionEl = closest(target, "data-action")) {
            const QString action = attribute(actionEl, "data-action");
            const QString idStr = attribute(actionEl, "data-id");
            const Core::Snowflake accountId(idStr.toULongLong());

            if (action == QLatin1String("close")) {
                hide();
            } else if (action == QLatin1String("close-login")) {
                if (shell.accounts() && shell.accounts()->rowCount() > 0)
                    switchView(ViewMode::Accounts);
                else
                    hide();
            } else if (action == QLatin1String("back-to-accounts")) {
                switchView(ViewMode::Accounts);
            } else if (action == QLatin1String("open-login")) {
                showLogin(LoginTab::Password);
            } else if (action == QLatin1String("submit-login")) {
                handleLoginSubmit();
            } else if (action == QLatin1String("submit-token")) {
                handleTokenSubmit();
            } else if (action == QLatin1String("mfa-verify")) {
                handleMfaVerify();
            } else if (action == QLatin1String("mfa-back")) {
                if (modalDocument) {
                    if (auto *pf = modalDocument->GetElementById("password-form-view"))
                        pf->SetProperty("display", "block");
                    if (auto *mf = modalDocument->GetElementById("password-mfa-view"))
                        mf->SetProperty("display", "none");
                }
            } else if (action == QLatin1String("mfa-send-sms")) {
                handleMfaSms();
            } else if (action == QLatin1String("qr-refresh")) {
                startQrClient();
            } else if (action == QLatin1String("close-proxy-edit")) {
                switchView(ViewMode::Accounts);
            } else if (action == QLatin1String("save-proxy")) {
                handleSaveProxy();
            } else if (action == QLatin1String("close-token-edit")) {
                switchView(ViewMode::Accounts);
            } else if (action == QLatin1String("save-token")) {
                handleSaveToken();
            } else if (action == QLatin1String("connect")) {
                handleConnect(accountId);
            } else if (action == QLatin1String("disconnect")) {
                handleDisconnect(accountId);
            } else if (action == QLatin1String("toggle-autoconnect")) {
                handleToggleAutoConnect(accountId);
            } else if (action == QLatin1String("open-proxy")) {
                openEditProxy(accountId);
            } else if (action == QLatin1String("open-token")) {
                openSetToken(accountId);
            } else if (action == QLatin1String("remove")) {
                handleRemoveAccount(accountId);
            } else if (action == QLatin1String("close-confirm-logout")) {
                switchView(ViewMode::Accounts);
            } else if (action == QLatin1String("do-logout")) {
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
        const QString tabAttr = attribute(t, "data-tab");
        const bool active = (tab == LoginTab::Password && tabAttr == QLatin1String("password")) ||
                            (tab == LoginTab::Qr && tabAttr == QLatin1String("qr")) ||
                            (tab == LoginTab::Token && tabAttr == QLatin1String("token"));
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
    if (!modalDocument)
        return;
    auto *container = modalDocument->GetElementById("accounts-list-container");
    if (!container)
        return;

    auto *model = shell.accounts();
    if (!model) {
        container->SetInnerRML("<div class=\"accounts-subtitle\">No accounts model available.</div>");
        return;
    }

    const auto &accounts = model->allAccounts();
    if (accounts.isEmpty()) {
        container->SetInnerRML(
            "<div style=\"text-align: center; padding: 24dp 0;\">"
            "<div class=\"accounts-title\" style=\"font-size: 16dp; margin-bottom: 6dp;\">No Accounts Saved</div>"
            "<div class=\"accounts-subtitle\">Add your Discord account below to get started.</div>"
            "</div>");
        return;
    }

    QString html;
    for (const auto &acc : accounts) {
        const QString idStr = QString::number(acc.id);
        const bool isConnected = (acc.state == Core::ConnectionState::Connected);
        const bool isConnecting = (acc.state == Core::ConnectionState::Connecting);

        QString badgeClass;
        QString badgeText;
        if (isConnected) {
            badgeClass = QStringLiteral("badge-active");
            badgeText = QStringLiteral("Active");
        } else if (isConnecting) {
            badgeClass = QStringLiteral("badge-connecting");
            badgeText = QStringLiteral("Connecting...");
        } else {
            badgeClass = QStringLiteral("badge-offline");
            badgeText = QStringLiteral("Offline");
        }

        const QString cardClass = isConnected ? QStringLiteral("account-card active-account") : QStringLiteral("account-card");

        const QUrl avatarUrl = Discord::Cdn::userAvatar(acc.id, acc.avatar, 64);
        const QString avatarSrc = acc.avatar.isEmpty()
            ? QStringLiteral(":/resources/placeholder.png")
            : shell.imageSource(avatarUrl, QSize(44, 44));

        const QString displayName = escape(acc.displayName.isEmpty() ? acc.username : acc.displayName);
        const QString username = escape(acc.username);
        const QString proxyText = acc.proxy.enabled() ? QStringLiteral("Proxy: %1").arg(escape(acc.proxy.toDisplayString())) : QStringLiteral("Direct connection");

        const QString switchCheckClass = acc.autoConnect ? QStringLiteral(" checked") : QString();

        html += QStringLiteral(
            "<div class=\"%1\">"
            "  <div class=\"account-card-main\">"
            "    <img class=\"account-avatar\" src=\"%2\"/>"
            "    <div class=\"account-info\">"
            "      <div class=\"account-header-line\">"
            "        <span class=\"account-display-name\">%3</span>"
            "        <span class=\"account-badge %4\">%5</span>"
            "      </div>"
            "      <div class=\"account-username\">@%6</div>"
            "      <div class=\"account-proxy-tag\">%7</div>"
            "    </div>"
            "  </div>"
            "  <div class=\"account-card-bottom\">"
            "    <div class=\"account-toggle-label\" data-action=\"toggle-autoconnect\" data-id=\"%8\">"
            "      <span class=\"autoconnect-text\">Launch at startup</span>"
            "      <div class=\"accounts-switch%9\"><div class=\"accounts-switch-thumb\"></div></div>"
            "    </div>"
            "    <div class=\"account-actions\">")
            .arg(cardClass, avatarSrc, displayName, badgeClass, badgeText, username, proxyText, idStr, switchCheckClass);

        if (isConnected) {
            html += QStringLiteral("<button class=\"accounts-btn accounts-btn-sm accounts-btn-secondary\" data-action=\"disconnect\" data-id=\"%1\">Disconnect</button>").arg(idStr);
        } else {
            html += QStringLiteral("<button class=\"accounts-btn accounts-btn-sm accounts-btn-primary\" data-action=\"connect\" data-id=\"%1\">Switch</button>").arg(idStr);
        }

        html += QStringLiteral(
            "      <button class=\"accounts-btn accounts-btn-sm accounts-btn-secondary\" data-action=\"open-proxy\" data-id=\"%1\">Proxy</button>"
            "      <button class=\"accounts-btn accounts-btn-sm accounts-btn-secondary\" data-action=\"open-token\" data-id=\"%1\">Token</button>"
            "      <button class=\"accounts-btn accounts-btn-sm accounts-btn-danger\" data-action=\"remove\" data-id=\"%1\">Log Out</button>"
            "    </div>"
            "  </div>"
            "</div>")
            .arg(idStr);
    }

    container->SetInnerRML(html.toUtf8().constData());
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

    const QString email = QString::fromStdString(emailInput->GetValue()).trimmed();
    const QString pass = QString::fromStdString(passInput->GetValue());

    if (email.isEmpty() || pass.isEmpty()) {
        errLabel->SetInnerRML("Enter your email or phone number, and your password.");
        errLabel->SetProperty("display", "block");
        return;
    }

    Core::ProxyConfig proxy;
    if (proxyInput) {
        const QString proxyStr = QString::fromStdString(proxyInput->GetValue()).trimmed();
        if (!proxyStr.isEmpty()) {
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

    if (loginClient)
        delete loginClient;

    loginClient = new Discord::LoginClient(proxy, shell.session()->getCaptchaResolver(), this);

    connect(loginClient, &Discord::LoginClient::smsSent, this, [this](bool ok) {
        setLoginBusy(false);
        if (modalDocument) {
            if (auto *errEl = modalDocument->GetElementById("mfa-status-error")) {
                errEl->SetInnerRML(ok ? "SMS code sent to your phone." : "Failed to send SMS code. Please try again.");
                errEl->SetProperty("display", "block");
            }
        }
    });

    connect(loginClient, &Discord::LoginClient::mfaRequired, this, [this](const Discord::MfaChallenge &challenge) {
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
            QString selHtml;
            if (challenge.totp)
                selHtml += QStringLiteral("<option value=\"%1\">Authenticator App</option>").arg(static_cast<int>(Discord::MfaMethod::Totp));
            if (challenge.backup)
                selHtml += QStringLiteral("<option value=\"%1\">Backup Code</option>").arg(static_cast<int>(Discord::MfaMethod::Backup));
            if (challenge.sms)
                selHtml += QStringLiteral("<option value=\"%1\">Text Message (SMS)</option>").arg(static_cast<int>(Discord::MfaMethod::Sms));
            sel->SetInnerRML(selHtml.toUtf8().constData());
        }

        if (auto *wrap = modalDocument->GetElementById("mfa-method-wrapper")) {
            int count = (challenge.totp ? 1 : 0) + (challenge.backup ? 1 : 0) + (challenge.sms ? 1 : 0);
            wrap->SetProperty("display", count > 1 ? "block" : "none");
        }

        handleMfaMethodChanged();
    });

    connect(loginClient, &Discord::LoginClient::authenticated, this, [this, proxy](const QString &token) {
        setLoginBusy(false);
        if (addAccountWithToken(token, QString(), proxy))
            switchView(ViewMode::Accounts);
    });

    connect(loginClient, &Discord::LoginClient::failed, this, [this](Discord::LoginError error) {
        setLoginBusy(false);
        QString msg;
        switch (error) {
        case Discord::LoginError::Network:
            msg = QStringLiteral("Couldn't reach Discord. Check your network connection.");
            break;
        case Discord::LoginError::InvalidCredentials:
            msg = QStringLiteral("The email or password you entered is incorrect.");
            break;
        case Discord::LoginError::CaptchaRequired:
            msg = QStringLiteral("Discord requested a captcha. Captcha solve was cancelled or failed.");
            break;
        case Discord::LoginError::InvalidMfaCode:
            msg = QStringLiteral("The 2FA verification code didn't work. Check and try again.");
            break;
        case Discord::LoginError::TooManyRequests:
            msg = QStringLiteral("Too many login attempts. Please wait a bit and try again.");
            break;
        case Discord::LoginError::AccountLocked:
            msg = QStringLiteral("This account is disabled. Enable it on discord.com and try again.");
            break;
        case Discord::LoginError::Suspended:
            msg = QStringLiteral("This account has been suspended.");
            break;
        case Discord::LoginError::RequiresVerification:
            msg = QStringLiteral("Discord requires verification. Check your email or messages.");
            break;
        case Discord::LoginError::PasskeyOnly:
            msg = QStringLiteral("This account only allows passkeys. Use QR code or token login instead.");
            break;
        case Discord::LoginError::Unknown:
            msg = QStringLiteral("Login failed. Please try again.");
            break;
        }

        if (modalDocument) {
            auto *pf = modalDocument->GetElementById("password-form-view");
            const bool inMfa = (pf && pf->GetProperty<::Rml::String>("display") == "none");
            auto *errEl = modalDocument->GetElementById(inMfa ? "mfa-status-error" : "login-status-error");
            if (errEl) {
                errEl->SetInnerRML(escape(msg).toUtf8().constData());
                errEl->SetProperty("display", "block");
            }
        }
    });

    loginClient->login(email, pass);
}

Discord::MfaMethod AccountsController::currentMfaMethod() const
{
    if (!modalDocument)
        return Discord::MfaMethod::Totp;

    auto *sel = rmlui_dynamic_cast<::Rml::ElementFormControlSelect *>(modalDocument->GetElementById("mfa-method-select"));
    if (!sel)
        return Discord::MfaMethod::Totp;

    const int val = QString::fromStdString(sel->GetValue()).toInt();
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

    const QString code = QString::fromStdString(codeEl->GetValue()).trimmed();
    if (code.isEmpty()) {
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
            const QString pStr = QString::fromStdString(proxyEl->GetValue()).trimmed();
            if (!pStr.isEmpty()) {
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
    qrClient = new Discord::RemoteAuthClient(proxy, shell.session()->getCaptchaResolver(), this);

    connect(qrClient, &Discord::RemoteAuthClient::fingerprintReady, this, &AccountsController::renderQr);

    connect(qrClient, &Discord::RemoteAuthClient::pendingTicket, this, [this](const QString &, const QString &name, const QString &) {
        lastQrUsername = name;
        if (modalDocument) {
            if (auto *st = modalDocument->GetElementById("qr-status-text"))
                st->SetInnerRML(QStringLiteral("Scan detected for %1! Please confirm on your mobile app.").arg(escape(name)).toUtf8().constData());
        }
    });

    connect(qrClient, &Discord::RemoteAuthClient::authenticated, this, [this, proxy](const QString &token) {
        const QString uname = lastQrUsername;
        stopQrClient();
        if (addAccountWithToken(token, uname, proxy))
            switchView(ViewMode::Accounts);
    });

    connect(qrClient, &Discord::RemoteAuthClient::failed, this, [this](Discord::RemoteAuthError) {
        if (modalDocument) {
            if (auto *st = modalDocument->GetElementById("qr-status-text"))
                st->SetInnerRML("QR authentication session expired or failed. Click 'Refresh QR Code' to try again.");
        }
    });

    connect(qrClient, &Discord::RemoteAuthClient::canceled, this, [this]() {
        if (modalDocument) {
            if (auto *st = modalDocument->GetElementById("qr-status-text"))
                st->SetInnerRML("Login was canceled on your phone.");
        }
    });

    qrClient->start();
}

void AccountsController::stopQrClient()
{
    if (qrClient) {
        qrClient->deleteLater();
        qrClient = nullptr;
    }
    qrStarted = false;
    Runtime::instance().clearFrame(QStringLiteral("ra-qr"));
}

void AccountsController::renderQr(const QString &fingerprint)
{
    const QString url = QStringLiteral("https://discord.com/ra/") + fingerprint;
    try {
        const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(url.toUtf8().constData(), qrcodegen::QrCode::Ecc::QUARTILE);
        const int n = qr.getSize();
        const int border = 4;
        const int dim = n + border * 2;

        QImage image(dim, dim, QImage::Format_RGB32);
        image.fill(Qt::white);
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                if (qr.getModule(x, y))
                    image.setPixel(x + border, y + border, qRgb(0, 0, 0));
            }
        }

        Runtime::instance().setFrame(QStringLiteral("ra-qr"), image.scaled(256, 256, Qt::KeepAspectRatio, Qt::FastTransformation));

        if (modalDocument) {
            if (auto *st = modalDocument->GetElementById("qr-status-text"))
                st->SetInnerRML("Ready to scan");
        }
        requestRender(100);
    } catch (...) {
        if (modalDocument) {
            if (auto *st = modalDocument->GetElementById("qr-status-text"))
                st->SetInnerRML("Failed to generate QR code.");
        }
    }
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

    const QString token = QString::fromStdString(tokenInput->GetValue()).trimmed();
    if (token.isEmpty()) {
        errLabel->SetInnerRML("Please enter a token.");
        errLabel->SetProperty("display", "block");
        return;
    }

    Core::ProxyConfig proxy;
    if (proxyInput) {
        const QString proxyStr = QString::fromStdString(proxyInput->GetValue()).trimmed();
        if (!proxyStr.isEmpty()) {
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
    if (addAccountWithToken(token, QString(), proxy))
        switchView(ViewMode::Accounts);
}

bool AccountsController::addAccountWithToken(const QString &token, const QString &username, const Core::ProxyConfig &proxy)
{
    auto fail = [this](const QString &message) {
        if (modalDocument) {
            const char *errorId = currentTab == LoginTab::Token ? "token-status-error"
                                 : currentTab == LoginTab::Qr ? "qr-status-text" : "login-status-error";
            if (currentTab == LoginTab::Password) {
                auto *form = modalDocument->GetElementById("password-form-view");
                if (form && form->GetProperty<::Rml::String>("display") == "none")
                    errorId = "mfa-status-error";
            }
            if (auto *error = modalDocument->GetElementById(errorId)) {
                error->SetInnerRML(escape(message).toStdString());
                error->SetProperty("display", "block");
            }
        }
        return false;
    };
    const Core::Snowflake userId = Core::TokenUtils::getIdAndCheckToken(token);
    if (!userId.isValid())
        return fail(QStringLiteral("The login token does not contain a valid account ID."));

    Core::AccountInfo acc;
    acc.id = userId;
    acc.token = token;
    acc.username = username.isEmpty() ? QStringLiteral("User") : username;
    acc.displayName = acc.username;
    acc.proxy = proxy;

    auto *model = shell.accounts();
    if (!model)
        return fail(QStringLiteral("Account storage is unavailable."));

    const auto result = model->addAccount(acc);
    if (!result.success())
        return fail(QStringLiteral("Failed to save account: %1").arg(result.error));

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
    const QString name = acc ? (acc->displayName.isEmpty() ? acc->username : acc->displayName) : QStringLiteral("this account");
    if (auto *desc = modalDocument->GetElementById("confirm-logout-desc"))
        desc->SetInnerRML(QStringLiteral("Are you sure you want to log out of %1?").arg(escape(name)).toUtf8().constData());

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
        const QString name = acc->displayName.isEmpty() ? acc->username : acc->displayName;
        sub->SetInnerRML(QStringLiteral("Proxy for %1").arg(escape(name)).toUtf8().constData());
    }

    if (auto *inp = rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(modalDocument->GetElementById("edit-proxy-input"))) {
        inp->SetValue(acc->proxy.toString().toStdString());
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

    const QString proxyStr = QString::fromStdString(inp->GetValue()).trimmed();
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
        const QString name = acc->displayName.isEmpty() ? acc->username : acc->displayName;
        sub->SetInnerRML(QStringLiteral("Update token for %1").arg(escape(name)).toUtf8().constData());
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

    const QString newToken = QString::fromStdString(inp->GetValue()).trimmed();
    if (newToken.isEmpty()) {
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
            err->SetInnerRML(QStringLiteral("This token belongs to user ID %1, but this account is %2.")
                                 .arg(static_cast<quint64>(tokenUserId))
                                 .arg(static_cast<quint64>(activeTargetAccountId))
                                 .toUtf8().constData());
            err->SetProperty("display", "block");
        }
        return;
    }

    const auto saved = Core::TokenStore::saveToken(activeTargetAccountId, newToken);
    if (!saved.success()) {
        if (err) {
            err->SetInnerRML(QStringLiteral("Failed to save token: %1").arg(escape(saved.error)).toUtf8().constData());
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
