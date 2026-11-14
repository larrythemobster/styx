#pragma once

#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "Platform/Trackable.hpp"
#include "ClientInstance.hpp"
#include "Storage/AccountRepository.hpp"
#include "Core/Enums.hpp"
#include "Core/AnimatedImageCache.hpp"
#include "Core/ImageManager.hpp"

namespace Styx::Core {

class Session : public Platform::Trackable {
public:
    Session();
    ~Session() override;
    void start();
    void shutdown();
    void connectAccount(Snowflake accountId);
    void disconnectAccount(Snowflake accountId);
    void autoConnectAccounts();
    Result<void> setAccountProxy(Snowflake accountId, const ProxyConfig &proxy);
    [[nodiscard]] std::vector<ClientInstance *> getClients() const;
    [[nodiscard]] ClientInstance *client(Snowflake accountId) const;
    [[nodiscard]] AccountInfo getAccountInfo(Snowflake accountId);
    [[nodiscard]] ImageManager *getImageManager() { return imageManager.get(); }
    [[nodiscard]] AnimatedImageCache *getAnimatedImageCache() { return animatedImageCache.get(); }
    [[nodiscard]] bool hasActiveConnection() const;
    void setCaptchaResolver(Discord::CaptchaResolver *resolver) { captchaResolver = resolver; }
    [[nodiscard]] Discord::CaptchaResolver *getCaptchaResolver() const { return captchaResolver; }

    Platform::Signal<Snowflake, Core::ConnectionState> connectionStateChanged;
    Platform::Signal<Snowflake, const std::string &> connectFailed;
    Platform::Signal<const Core::AccountInfo &> accountDetailsUpdated;
    Platform::Signal<const Discord::Ready &> ready;
private:
    void startInstance(const AccountInfo &acc);
    std::unique_ptr<ImageManager> imageManager;
    std::unique_ptr<AnimatedImageCache> animatedImageCache;
    Storage::AccountRepository repo;
    std::map<Snowflake, std::unique_ptr<ClientInstance>> clients;
    std::unordered_set<Snowflake> connectingAccounts;
    Discord::CaptchaResolver *captchaResolver = nullptr;
};

} // namespace Styx::Core
