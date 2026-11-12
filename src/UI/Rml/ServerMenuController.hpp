#pragma once

#include "Controller.hpp"
#include "ContextMenu.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/UserEdits.hpp"
#include "Proto/PreloadedSettings.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace Rml { class ElementDocument; }
namespace Styx::Core { class ClientInstance; }

namespace Styx::UI::Rml {

class ProfileEditorController;

class ServerMenuController final : public Controller
{
public:
    explicit ServerMenuController(Shell &shell);
    ~ServerMenuController() override;
    void show(Core::Snowflake account, Core::Snowflake guild, bool developerMode);

private:
    enum class Page { Invite, Privacy, Profile };
    void ensureDocument();
    void openPage(Core::Snowflake account, Core::Snowflake guild, Page page);
    void close();
    void hide();
    void renderPrivacy();
    void renderInvite();
    void togglePrivacy(const std::string &key);
    void createInvite();
    void setError(const std::string &message);
    void setMute(Core::Snowflake account, Core::Snowflake guild, int seconds);
    void applyGuildEdit(Core::Snowflake account, Core::Snowflake guild, const Discord::UserGuildSettingsEdit &edit);
    void applyProtoEdit(Core::Snowflake account, Proto::PreloadedSettingsEdit edit);
    void leave(Core::Snowflake account, Core::Snowflake guild, const std::string &name);
    std::vector<MenuItem> notificationItems(Core::Snowflake account, Core::Snowflake guild);
    std::vector<MenuItem> moveItems(Core::Snowflake account, Core::Snowflake guild);

    ::Rml::ElementDocument *document = nullptr;
    std::unique_ptr<ProfileEditorController> profileEditor;
    Platform::Guard<Core::ClientInstance> origin;
    Platform::Connection originDestroyed;
    Core::Snowflake currentGuild;
    Page page = Page::Privacy;
    bool visible = false;
    bool saving = false;
    std::uint64_t generation = 0;
    std::string inviteUrl;
};

} // namespace Styx::UI::Rml
