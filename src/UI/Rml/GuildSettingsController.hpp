#pragma once

#include "Controller.hpp"
#include "GuildSettingsSection.hpp"
#include "AuditLogFormatter.hpp"
#include "MemberModeration.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"
#include "Discord/GuildRequests.hpp"

namespace Rml {
class ElementDocument;
}

namespace Styx {
namespace Core {
class ClientInstance;
}
namespace UI {
namespace Rml {

class GuildSettingsController final : public Controller
{
public:
    explicit GuildSettingsController(Shell &shell);
    ~GuildSettingsController() override;

    void open(Core::Snowflake accountId, Core::Snowflake guildId, GuildSettingsSection section = GuildSettingsSection::Profile);
    void close();
    [[nodiscard]] bool isOpen() const { return visible; }

    void setSection(GuildSettingsSection section);
    [[nodiscard]] GuildSettingsSection section() const { return currentSection; }

private:
    void ensureDocument();
    void render();
    void renderSidebar();
    void renderContent();

    // Render individual pages
    void renderProfile(std::string &rml);
    void renderEngagement(std::string &rml);
    void renderBoostPerks(std::string &rml);
    void renderEmoji(std::string &rml);
    void renderStickers(std::string &rml);
    void renderMembers(std::string &rml);
    void renderRoles(std::string &rml);
    void renderInvites(std::string &rml);
    void renderAuditLog(std::string &rml);
    void renderBans(std::string &rml);
    void renderDeleteServer(std::string &rml);

    // Data loaders
    void loadCurrentSectionData();
    void loadProfile();
    void loadEngagement();
    void loadBoostPerks();
    void loadEmoji();
    void loadStickers();
    void loadMembers();
    void loadRoles();
    void loadInvites();
    void loadAuditLog();
    void loadBans();

    void saveChanges();
    void reportError(const std::string &message);
    [[nodiscard]] bool canEditCurrentSection() const;
    [[nodiscard]] bool isCurrentRequest(Core::Snowflake guildId, std::uint64_t generation) const;
    // Unsaved changes bar
    void updateUnsavedChangesBar();
    [[nodiscard]] bool hasUnsavedChanges() const;
    void discardChanges();

    // Event handling
    void setupEventListeners();
    void handleClick(::Rml::Element *target, ::Rml::Event &event);
    void handleInput(::Rml::Element *target, ::Rml::Event &event);
    void handleChange(::Rml::Element *target, ::Rml::Event &event);
    void handleKeyDown(::Rml::Event &event);

    void resetGuildState();
    // Helpers
    [[nodiscard]] Core::ClientInstance *instance() const;
    [[nodiscard]] std::optional<Discord::Guild> guild() const;
    [[nodiscard]] std::string getGuildIconUrl() const;

    ::Rml::ElementDocument *document = nullptr;
    bool visible = false;
    Core::Snowflake currentAccountId;
    Core::Snowflake currentGuildId;
    std::uint64_t requestGeneration = 0;
    std::string errorMessage;
    GuildSettingsSection currentSection = GuildSettingsSection::Profile;
    // --- Page State & Drafts ---

    // 1. Profile
    struct ProfileDraft {
        std::string name;
        std::string description;
        std::string iconDataUri;
        bool iconRemoved = false;
        std::string brandColor;
        std::string traits[6];
        bool isPrivate = false;
    };
    ProfileDraft savedProfile;
    ProfileDraft draftProfile;
    Discord::GuildProfileEdit savedGuildProfile;
    bool profileLoaded = false;

    // 2. Engagement
    struct EngagementDraft {
        Core::Snowflake systemChannelId;
        Discord::SystemChannelFlags systemChannelFlags = Discord::SystemChannelFlags{};
        bool suppressJoin = false;
        bool suppressBoost = false;
        bool suppressTips = false;
        bool suppressPrompt = false;
        Discord::MessageNotificationLevel defaultNotifications = Discord::MessageNotificationLevel::ALL_MESSAGES;
        Core::Snowflake afkChannelId;
        int afkTimeout = 300;
    };
    EngagementDraft savedEngagement;
    EngagementDraft draftEngagement;
    bool engagementLoaded = false;

    // 3. Boost Perks
    struct BoostDraft {
        bool progressBar = false;
        std::string bannerUri;
        bool bannerRemoved = false;
        std::string splashUri;
        bool splashRemoved = false;
    };
    BoostDraft savedBoost;
    BoostDraft draftBoost;
    bool boostLoaded = false;

    // 4. Emoji
    std::vector<Discord::Emoji> emojis;
    bool emojisLoaded = false;

    // 5. Stickers
    std::vector<Discord::Sticker> stickers;
    bool stickersLoaded = false;

    // 6. Members
    std::vector<Discord::Member> members;
    std::string memberSearchQuery;
    bool membersLoaded = false;

    // 7. Roles
    enum class RoleTab { Display, Permissions, Members };
    RoleTab currentRoleTab = RoleTab::Display;
    std::vector<Discord::Role> roles;
    Core::Snowflake selectedRoleId;
    struct RoleDraft {
        std::string name;
        int color = 0;
        bool hoist = false;
        bool mentionable = false;
        Discord::Permissions permissions;
    };
    std::unordered_map<Core::Snowflake, RoleDraft> savedRoleDrafts;
    std::unordered_map<Core::Snowflake, RoleDraft> dirtyRoleDrafts;
    std::string permissionSearchQuery;
    bool rolesLoaded = false;

    // 8. Invites
    std::vector<Discord::Invite> invites;
    bool invitesLoaded = false;

    // 9. Audit Log
    std::unique_ptr<AuditLogFormatter> auditFormatter;
    std::vector<AuditLogFormatter::Row> auditRows;
    std::unordered_set<int> expandedAuditRows;
    Core::Snowflake auditFilterUserId;
    int auditFilterAction = -1;
    bool auditLogLoaded = false;

    // 10. Bans
    std::vector<Discord::Ban> bans;
    std::string banSearchQuery;
    bool bansLoaded = false;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
