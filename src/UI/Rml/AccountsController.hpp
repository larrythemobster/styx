#pragma once

#include "Controller.hpp"
#include "Core/AccountInfo.hpp"
#include "Core/ProxyConfig.hpp"
#include "Discord/LoginClient.hpp"
#include "Discord/RemoteAuthClient.hpp"

#include <QPointer>
#include <QString>

namespace Rml {
class ElementDocument;
}

namespace Styx {
namespace UI {

class Shell;

namespace Rml {

class AccountsController final : public Controller
{
    Q_OBJECT
public:
    enum class LoginTab {
        Password,
        Qr,
        Token
    };

    explicit AccountsController(Shell &shell, QObject *parent = nullptr);
    ~AccountsController() override;

    void showAccounts();
    void showLogin(LoginTab tab = LoginTab::Password);
    void hide();
    [[nodiscard]] bool isOpen() const { return visible; }

private:
    enum class ViewMode {
        Accounts,
        Login,
        EditProxy,
        SetToken,
        ConfirmLogout
    };

    void ensureDocument();
    void setupEventListeners();
    void switchView(ViewMode mode);
    void switchLoginTab(LoginTab tab);

    void rebuildAccountsList();
    void clearErrors();

    // Login actions
    void handleLoginSubmit();
    void handleMfaVerify();
    void handleMfaSms();
    void handleMfaMethodChanged();
    void setLoginBusy(bool busy);

    // QR actions
    void startQrClient();
    void stopQrClient();
    void renderQr(const QString &fingerprint);

    // Token actions
    void handleTokenSubmit();

    // Account actions
    bool addAccountWithToken(const QString &token, const QString &username, const Core::ProxyConfig &proxy);
    void handleConnect(Core::Snowflake accountId);
    void handleDisconnect(Core::Snowflake accountId);
    void handleToggleAutoConnect(Core::Snowflake accountId);
    void handleRemoveAccount(Core::Snowflake accountId);
    void openEditProxy(Core::Snowflake accountId);
    void handleSaveProxy();
    void openSetToken(Core::Snowflake accountId);
    void handleSaveToken();

    [[nodiscard]] Discord::MfaMethod currentMfaMethod() const;

    ::Rml::ElementDocument *modalDocument = nullptr;
    bool visible = false;
    ViewMode currentView = ViewMode::Accounts;
    LoginTab currentTab = LoginTab::Password;

    Core::Snowflake activeTargetAccountId;
    Discord::MfaChallenge currentChallenge;

    QPointer<Discord::LoginClient> loginClient;
    QPointer<Discord::RemoteAuthClient> qrClient;
    bool qrStarted = false;
    QString lastQrUsername;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
