#pragma once

#include "Controller.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"

#include "Platform/Trackable.hpp"
#include "Platform/Signal.hpp"
#include <string>
#include <vector>

namespace Rml {
class ElementDocument;
}

namespace Styx {
namespace UI {
namespace Rml {

class ThreadBrowserController final : public Controller
{
public:
    explicit ThreadBrowserController(Shell &shell);
    ~ThreadBrowserController() override;

    void open(Core::ClientInstance *instance, Core::Snowflake channelId, const std::string &channelName);
    void close();
    [[nodiscard]] bool isOpen() const { return visible; }

    Platform::Signal<Core::Snowflake> threadActivated;

private:
    enum class Tab {
        Active,
        Archived
    };

    void ensureDocument();
    void setupEventListeners();
    void render();
    void loadActive();
    void loadArchived();

    ::Rml::ElementDocument *threadsDocument = nullptr;
    bool visible = false;

    Platform::Guard<Core::ClientInstance> clientInstance;
    Core::Snowflake currentChannelId;
    std::string currentChannelName;

    Tab currentTab = Tab::Active;
    int loadGeneration = 0;

    std::vector<Discord::Channel> activeThreads;
    bool activeLoading = false;
    bool activeLoaded = false;
    std::string activeError;

    std::vector<Discord::Channel> archivedThreads;
    bool archivedLoading = false;
    bool archivedLoaded = false;
    bool archivedHasMore = false;
    int archivedOffset = 0;
    std::string archivedError;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
