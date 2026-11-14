#include "AccountsModel.hpp"

#include "Storage/AccountRepository.hpp"
#include "Core/TokenStore.hpp"
#include "Core/Session.hpp"
#include "Discord/CdnUrls.hpp"

#include <utility>

namespace Styx::UI {

using namespace Styx::Core;
using namespace Styx::Storage;

AccountsModel::AccountsModel(Session *session) : session(session)
{
    AccountRepository repo;
    accounts = repo.getAllAccounts();

    track(session->connectionStateChanged.connect([this](Snowflake id, ConnectionState state) {
        const int row = rowForId(id);
        if (row < 0) return;
        accounts[row].state = state;
        changed.publish(row);
    }));
    track(session->accountDetailsUpdated.connect([this](const AccountInfo &info) {
        const int row = rowForId(info.id);
        if (row < 0) return;
        accounts[row].username = info.username;
        accounts[row].displayName = info.displayName;
        accounts[row].avatar = info.avatar;
        changed.publish(row);
    }));
    track(session->getImageManager()->imageFetched.connect(
        [this](const std::string &url, const Platform::Size &, const Platform::ImageBuffer &) {
            avatarTracker.notify(url, [this](Snowflake id) {
                const int row = rowForId(id);
                if (row >= 0) avatarChanged.publish(row);
            });
        }));
}

Platform::ImageBuffer AccountsModel::avatar(int row) const
{
    const auto *account = accountAt(row);
    if (!account) return {};
    constexpr Platform::Size size{32, 32};
    const auto url = Discord::Cdn::userAvatar(account->id, account->avatar, size.width);
    return avatarTracker.fetch(session->getImageManager(), url, size, account->id, account->id);
}

Result<void> AccountsModel::addAccount(const AccountInfo &account)
{
    Result<void> saved = TokenStore::saveToken(account.id, account.token);
    if (!saved.success()) return saved;

    AccountInfo newAccount = account;
    int maxOrder = -1;
    for (const auto &acc : accounts)
        if (acc.displayOrder > maxOrder) maxOrder = acc.displayOrder;
    newAccount.displayOrder = maxOrder + 1;

    AccountRepository repo;
    saved = repo.saveAccount(newAccount);
    if (!saved.success()) {
        TokenStore::deleteToken(account.id);
        return saved;
    }
    repo.clearHeartbeatSession(newAccount.id);
    session->getImageManager()->setAccountProxy(newAccount.id, newAccount.proxy);
    newAccount.token.clear();
    accounts.push_back(std::move(newAccount));
    reset.publish();
    return saved;
}

void AccountsModel::removeAccount(int row)
{
    if (!accountAt(row)) return;
    const auto id = accounts[row].id;
    session->disconnectAccount(id);
    AccountRepository repo;
    repo.removeAccount(id);
    accounts.erase(accounts.begin() + row);
    reset.publish();
}

void AccountsModel::setConnectionState(int row, ConnectionState state)
{
    if (!accountAt(row)) return;
    accounts[row].state = state;
    changed.publish(row);
}

void AccountsModel::setAutoConnect(int row, bool enabled)
{
    if (!accountAt(row) || accounts[row].autoConnect == enabled) return;
    accounts[row].autoConnect = enabled;
    AccountRepository repo;
    repo.updateAutoConnect(accounts[row].id, enabled);
    changed.publish(row);
}

Result<void> AccountsModel::setProxy(int row, const ProxyConfig &proxy)
{
    if (!accountAt(row) || accounts[row].state != ConnectionState::Disconnected)
        return Result<void>::makeError("Disconnect this account before changing its proxy.");
    if (accounts[row].proxy == proxy && accounts[row].proxyError.empty()) return Result<void>::makeOk();
    auto result = session->setAccountProxy(accounts[row].id, proxy);
    if (!result.success()) return result;
    accounts[row].proxy = proxy;
    accounts[row].proxyError.clear();
    changed.publish(row);
    return result;
}

bool AccountsModel::moveAccount(int sourceRow, int targetRow)
{
    if (sourceRow < 0 || sourceRow >= rowCount() || targetRow < 0 || targetRow > rowCount() ||
        sourceRow == targetRow || sourceRow + 1 == targetRow) return false;
    const int destination = targetRow - (sourceRow < targetRow ? 1 : 0);
    auto account = std::move(accounts[sourceRow]);
    accounts.erase(accounts.begin() + sourceRow);
    accounts.insert(accounts.begin() + destination, std::move(account));

    AccountRepository repo;
    for (int i = 0; i < rowCount(); ++i) {
        accounts[i].displayOrder = i;
        repo.updateDisplayOrder(accounts[i].id, i);
    }
    reset.publish();
    return true;
}

} // namespace Styx::UI
