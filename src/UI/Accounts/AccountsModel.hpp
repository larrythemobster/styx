#pragma once

#include <vector>

#include "Core/AccountInfo.hpp"
#include "Core/Result.hpp"
#include "Platform/Trackable.hpp"
#include "UI/AvatarRequestTracker.hpp"

namespace Styx {
namespace Core { class Session; }
namespace UI {

class AccountsModel : public Platform::Trackable
{
public:
    explicit AccountsModel(Core::Session *session);

    [[nodiscard]] int rowCount() const { return static_cast<int>(accounts.size()); }
    [[nodiscard]] const std::vector<Core::AccountInfo> &allAccounts() const { return accounts; }
    [[nodiscard]] const Core::AccountInfo *accountAt(int row) const {
        return row >= 0 && row < rowCount() ? &accounts[row] : nullptr;
    }
    [[nodiscard]] const Core::AccountInfo *accountById(Core::Snowflake id) const {
        const int row = rowForId(id);
        return accountAt(row);
    }
    [[nodiscard]] int rowForId(Core::Snowflake id) const {
        for (int i = 0; i < rowCount(); ++i)
            if (accounts[i].id == id) return i;
        return -1;
    }
    [[nodiscard]] Platform::ImageBuffer avatar(int row) const;

    [[nodiscard]] Core::Result<void> addAccount(const Core::AccountInfo &account);
    void removeAccount(int row);
    void setConnectionState(int row, Core::ConnectionState state);
    void setAutoConnect(int row, bool enabled);
    Core::Result<void> setProxy(int row, const Core::ProxyConfig &proxy);
    bool moveAccount(int sourceRow, int targetRow);

    Platform::Signal<int> changed;
    Platform::Signal<> reset;
    Platform::Signal<int> avatarChanged;

private:
    Core::Session *session;
    std::vector<Core::AccountInfo> accounts;
    mutable AvatarRequestTracker<Core::Snowflake> avatarTracker;
};

} // namespace UI
} // namespace Styx
