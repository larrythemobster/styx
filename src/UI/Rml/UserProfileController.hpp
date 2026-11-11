#pragma once

#include "Controller.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"

#include "Platform/Native.hpp"
#include "Platform/Timer.hpp"
#include <RmlUi/Core/ObserverPtr.h>
#include <string>
#include <optional>

namespace Rml {
class ElementDocument;
}

namespace Styx {
namespace Core {
class ClientInstance;
}
namespace UI {
class Shell;
namespace Rml {

class UserProfileController final : public Controller
{
public:
    explicit UserProfileController(Shell &shell);
    ~UserProfileController() override;

    void showProfile(Core::Snowflake userId, Core::Snowflake guildId, const Platform::Rect &anchorRect = {}, bool full = false);
    void hide();
    [[nodiscard]] bool isOpen() const { return visible; }

private:
    void ensureDocument();
    void updateProfileData();
    void renderPresenceAndActivities();
    void renderRoles();
    void renderDates();
    void renderFullContent();
    void selectFullTab(const std::string &tab);
    void positionCard(const Platform::Rect &anchorRect);
    void saveNoteIfDirty();

    [[nodiscard]] Core::ClientInstance *clientInstance() const;
    [[nodiscard]] std::optional<Discord::User> cachedUser() const;
    [[nodiscard]] std::optional<Discord::Member> cachedMember() const;
    [[nodiscard]] std::string resolvedDisplayName() const;
    [[nodiscard]] std::string tagHandle() const;
    [[nodiscard]] std::string resolvedAvatarUrl(int size) const;
    [[nodiscard]] int resolvedAccentColor() const;
    [[nodiscard]] bool isBot() const;

    ::Rml::ElementDocument *profileDocument = nullptr;
    ::Rml::ObserverPtr<::Rml::Element> previousFocus;
    Core::Snowflake currentUserId;
    Core::Snowflake currentGuildId;
    Discord::UserProfile profile;
    bool hasProfile = false;
    bool visible = false;
    bool noteDirty = false;
    bool fullMode = false;
    bool profileFailed = false;
    std::string fullTab = "activity";
    std::string initialNote;
    std::uint64_t requestGeneration = 0;

    Platform::Timer activityTimer;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
