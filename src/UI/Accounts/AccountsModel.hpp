#pragma once

#include <QAbstractListModel>
#include <QVector>
#include "Core/AccountInfo.hpp"
#include "Core/Result.hpp"
#include "UI/AvatarRequestTracker.hpp"

namespace Styx {

namespace Core {
class Session;
}

namespace UI {

class AccountsModel : public QAbstractListModel
{
    Q_OBJECT
public:
    explicit AccountsModel(Core::Session *session, QObject *parent = nullptr);

    enum Roles {
        AccountObjectRole = Qt::UserRole + 1,
        ConnectionStateRole,
        AutoConnectRole,
    };

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    Qt::DropActions supportedDropActions() const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                      const QModelIndex &parent) override;

    [[nodiscard]] Core::Result<void> addAccount(const Core::AccountInfo &account);
    void removeAccount(int row);

    void setConnectionState(int row, Core::ConnectionState state);
    void setAutoConnect(int row, bool enabled);
    void setProxy(int row, const Core::ProxyConfig &proxy);
    [[nodiscard]] const QVector<Core::AccountInfo> &allAccounts() const { return accounts; }
    [[nodiscard]] const Core::AccountInfo *accountAt(int row) const {
        if (row >= 0 && row < accounts.size()) return &accounts[row];
        return nullptr;
    }
    [[nodiscard]] const Core::AccountInfo *accountById(Core::Snowflake id) const {
        for (const auto &acc : accounts) {
            if (acc.id == id) return &acc;
        }
        return nullptr;
    }
    int rowForId(Core::Snowflake id) const {
        for (int i = 0; i < accounts.size(); ++i) {
            if (accounts[i].id == id) return i;
        }
        return -1;
    }

private:
    Core::Session *session;

    QVector<Core::AccountInfo> accounts;
    mutable AvatarRequestTracker<QPersistentModelIndex> avatarTracker;
};

} // namespace UI
} // namespace Styx