#pragma once

#include "Core/Snowflake.hpp"
#include "Platform/Trackable.hpp"
#include <vector>

namespace Styx::Core { class Session; }
namespace Styx::UI {
class ChannelTreeModel;
struct ChannelNode;

class ChannelFilterProxyModel : public Platform::Trackable {
public:
    explicit ChannelFilterProxyModel(Core::Session *session);
    void setSourceModel(ChannelTreeModel *sourceModel);
    ChannelTreeModel *sourceModel() const { return channelModel; }
    std::vector<ChannelNode *> children(ChannelNode *parent) const;
    bool accepts(ChannelNode *node) const;
    bool selected(ChannelNode *node) const;
    void setSelectedChannel(Core::Snowflake channelId, Core::Snowflake accountId);
    Core::Snowflake selectedChannel() const { return selectedChannelId; }
    void invalidateFilter();
    Platform::Signal<> changed;
private:
    bool hasChannelViewPermission(ChannelNode *node) const;
    static Core::Snowflake getUserIdForNode(ChannelNode *node);
    Core::Session *session;
    ChannelTreeModel *channelModel = nullptr;
    Core::Snowflake selectedChannelId;
    Core::Snowflake selectedAccountId;
};
}
