#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "DatabaseManager.hpp"
#include "Core/AccountInfo.hpp"
#include "Discord/ClientIdentity.hpp"
#include "Core/Result.hpp"
#include "Platform/Sqlite.hpp"

namespace Styx {
namespace Storage {

class AccountRepository
{
public:
    AccountRepository() = default;

    Core::Result<void> saveAccount(const Core::AccountInfo &acc);
    Core::AccountInfo getAccount(std::uint64_t id);
    std::vector<Core::AccountInfo> getAllAccounts();
    void removeAccount(std::uint64_t id);
    void updateDisplayOrder(std::uint64_t id, int order);
    void updateAutoConnect(std::uint64_t id, bool autoConnect);
    Core::Result<void> updateProxy(std::uint64_t id, const Core::ProxyConfig &proxy);
    static bool migrateLegacyProxies(Platform::Database &db);
    std::optional<Discord::HeartbeatSession> getHeartbeatSession(std::uint64_t id);
    void updateHeartbeatSession(std::uint64_t id, const Discord::HeartbeatSession &session);
    void clearHeartbeatSession(std::uint64_t id);
};

} // namespace Storage
} // namespace Styx
