#include "Session.hpp"
#include "Discord/CurlUtils.hpp"
#include "Logging.hpp"
#include "TokenStore.hpp"
#include "Platform/Dispatcher.hpp"

namespace Styx::Core {

Session::Session()
    : imageManager(std::make_unique<ImageManager>()),
      animatedImageCache(std::make_unique<AnimatedImageCache>(imageManager.get()))
{
    for (const auto &acc : repo.getAllAccounts())
        imageManager->setAccountProxy(acc.id, acc.proxy);
}
Session::~Session() { shutdown(); }
void Session::start() { Core::log(Core::LogLevel::Debug, LogCore) << "Session started"; }
void Session::shutdown()
{
    Core::log(Core::LogLevel::Debug, LogCore) << "Session shutdown";
    connectingAccounts.clear();
    auto stopping = std::move(clients);
    for (auto &[id, instance] : stopping) instance->stop();
}
void Session::connectAccount(Snowflake accountId)
{
    if (connectingAccounts.contains(accountId)) return;
    if (auto it = clients.find(accountId); it != clients.end()) {
        if (it->second->state() != ConnectionState::Disconnected) {
            Core::log(Core::LogLevel::Warning, LogCore) << "Account already connected or connecting: " << accountId.toString();
            return;
        }
        clients.erase(it);
    }
    AccountInfo acc = repo.getAccount(accountId);
    if (acc.id == 0) {
        Core::log(Core::LogLevel::Warning, LogCore) << "Account not found: " << accountId.toString();
        return;
    }
    auto token = TokenStore::loadToken(accountId);
    if (!token.success()) {
        Core::log(Core::LogLevel::Warning, LogCore) << "No token found in keychain for account: " << accountId.toString();
        connectFailed(accountId, token.error);
        return;
    }
    acc.token = *token.value;
    imageManager->setAccountProxy(acc.id, acc.proxy);
    connectingAccounts.insert(accountId);
    Discord::CurlUtils::ensureBuildNumber(acc.proxy, [self = Platform::Guard<Session>(this), acc]() {
        if (self && self->connectingAccounts.erase(acc.id)) self->startInstance(acc);
    });
}
void Session::startInstance(const AccountInfo &acc)
{
    const Snowflake accountId = acc.id;
    auto owned = std::make_unique<ClientInstance>(acc, captchaResolver);
    auto *instance = owned.get();
    track(instance->stateChanged.connect([this, accountId, instance](ConnectionState state) {
        connectionStateChanged(accountId, state);
        if (state == ConnectionState::Disconnected) {
            Platform::Dispatcher::main().post([self = Platform::Guard<Session>(this), accountId, instance]() {
                if (!self) return;
                auto it = self->clients.find(accountId);
                if (it != self->clients.end() && it->second.get() == instance) self->clients.erase(it);
            });
        }
    }));
    track(instance->detailsUpdated.connect([this](const AccountInfo &info) {
        repo.saveAccount(info);
        accountDetailsUpdated(info);
    }));
    track(instance->ready.connect([this](const Discord::Ready &event) { ready(event); }));
    clients.emplace(accountId, std::move(owned));
    instance->start();
}
void Session::autoConnectAccounts()
{
    for (const auto &acc : repo.getAllAccounts()) if (acc.autoConnect) connectAccount(acc.id);
}
void Session::setAccountProxy(Snowflake accountId, const ProxyConfig &proxy)
{
    repo.updateProxy(accountId, proxy);
    imageManager->setAccountProxy(accountId, proxy);
}
void Session::disconnectAccount(Snowflake accountId)
{
    connectingAccounts.erase(accountId);
    auto it = clients.find(accountId);
    if (it == clients.end()) return;
    auto instance = std::move(it->second);
    clients.erase(it);
    instance->stop();
}
std::vector<ClientInstance *> Session::getClients() const
{
    std::vector<ClientInstance *> result;
    result.reserve(clients.size());
    for (const auto &[id, instance] : clients) result.push_back(instance.get());
    return result;
}
ClientInstance *Session::client(Snowflake accountId) const
{
    auto it = clients.find(accountId);
    return it == clients.end() ? nullptr : it->second.get();
}
AccountInfo Session::getAccountInfo(Snowflake accountId)
{
    if (auto *instance = client(accountId)) return instance->accountInfo();
    return repo.getAccount(accountId);
}
bool Session::hasActiveConnection() const
{
    for (const auto &[id, instance] : clients)
        if (instance->state() == ConnectionState::Connected || instance->state() == ConnectionState::Connecting) return true;
    return false;
}

} // namespace Styx::Core
