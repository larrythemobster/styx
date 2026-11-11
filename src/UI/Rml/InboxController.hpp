#pragma once

#include "Controller.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"
#include <RmlUi/Core/ObserverPtr.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Rml { class ElementDocument; }
namespace Styx::Storage { class MessageRepository; }
namespace Styx::UI {
struct ChannelNode;
namespace Rml {

class InboxController : public Controller
{
public:
    explicit InboxController(Shell &shell);
    ~InboxController() override;
    void toggle();
    void hide();
    bool isOpen() const { return visible; }

private:
    void ensureDocument();
    void render();
    void activate(::Rml::Element *element);
    void loadMentions();
    void loadPreview(Core::Snowflake channel);
    bool permitted(const ChannelNode *node) const;
    std::string messageRml(const Discord::Message &message, Core::Snowflake guild) const;
    void accountChanged();

    ::Rml::ElementDocument *document = nullptr;
    ::Rml::ObserverPtr<::Rml::Element> trigger;
    std::unique_ptr<Storage::MessageRepository> messageRepo;
    bool visible = false;
    bool mentionsTab = false;
    bool mentionsLoaded = false;
    bool mentionsLoading = false;
    bool mentionsMore = true;
    std::string mentionsError;
    std::vector<Discord::Message> mentions;
    Core::Snowflake mentionCursor;
    Core::Snowflake account;
    unsigned generation = 0;
    struct Preview {
        bool loading = false;
        bool loaded = false;
        std::string error;
        std::vector<Discord::Message> messages;
        Core::Snowflake lastMessage;
    };
    std::unordered_map<Core::Snowflake, Preview> previews;
};

} // namespace Rml
} // namespace Styx::UI
