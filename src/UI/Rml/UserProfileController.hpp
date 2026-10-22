#pragma once

#include "Controller.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"

#include <QPointer>
#include <QTimer>
#include <QUrl>
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
    Q_OBJECT
public:
    explicit UserProfileController(Shell &shell, QObject *parent = nullptr);
    ~UserProfileController() override;

    void showProfile(Core::Snowflake userId, Core::Snowflake guildId, const QRectF &anchorRect = QRectF());
    void hide();
    [[nodiscard]] bool isOpen() const { return visible; }

private:
    void ensureDocument();
    void updateProfileData();
    void renderPresenceAndActivities();
    void renderRoles();
    void renderDates();
    void positionCard(const QRectF &anchorRect);
    void saveNoteIfDirty();

    [[nodiscard]] Core::ClientInstance *clientInstance() const;
    [[nodiscard]] std::optional<Discord::User> cachedUser() const;
    [[nodiscard]] std::optional<Discord::Member> cachedMember() const;
    [[nodiscard]] QString resolvedDisplayName() const;
    [[nodiscard]] QString tagHandle() const;
    [[nodiscard]] QUrl resolvedAvatarUrl(int size) const;
    [[nodiscard]] int resolvedAccentColor() const;
    [[nodiscard]] bool isBot() const;

    ::Rml::ElementDocument *profileDocument = nullptr;
    Core::Snowflake currentUserId;
    Core::Snowflake currentGuildId;
    Discord::UserProfile profile;
    bool hasProfile = false;
    bool visible = false;
    bool noteDirty = false;
    QString initialNote;

    QTimer activityTimer;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
