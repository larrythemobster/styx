#pragma once

#include "Controller.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"
#include "Discord/UserEdits.hpp"

#include <string>

namespace Rml {
class ElementFormControlInput;
class ElementFormControlTextArea;
} // namespace Rml

namespace Styx {
namespace UI {
namespace Rml {

class ProfileEditorController final : public Controller
{
public:
    ProfileEditorController(Shell &shell, ::Rml::Element &host);
    ~ProfileEditorController() override;

    void open(Core::Snowflake guildId = Core::Snowflake::Invalid, Core::Snowflake account = Core::Snowflake::Invalid);
    void close();
    void reset();
    [[nodiscard]] bool isSaving() const { return saving; }
    [[nodiscard]] bool hasPendingChanges() const;


private:
    enum class ImageSlot { Avatar, Banner };
    struct ImageDraft {
        std::string uri;
        std::string originalMd5;
        bool removed = false;
        bool previewEmpty = false;
    };

    void ensureDocument();
    void setupEventListeners();
    void render();
    void updatePreview(::Rml::Element *changed = nullptr);
    void save();
    void finishSave();
    void saveAccount(const Discord::CurrentUserEdit &accountEdit, Discord::UserProfileEdit profileEdit);
    void saveMember(const Discord::CurrentMemberEdit &edit);
    bool applyProfileSnapshot(const Discord::UserProfile &profile);
    [[nodiscard]] const std::string &baselineName() const;
    [[nodiscard]] const std::string &baselineAvatar() const;
    void saveMetadata(const Discord::UserProfileEdit &profileEdit, bool accountAlreadySaved);
    void chooseImage(ImageSlot slot);
    void removeImage(ImageSlot slot);
    void clearFrames();
    void clearDrafts();
    ImageDraft &imageDraft(ImageSlot slot);
    void applyBaselineToInputs();

    [[nodiscard]] const std::string &frameKey(ImageSlot slot) const;

    [[nodiscard]] ::Rml::ElementFormControlInput *nameInput() const;
    [[nodiscard]] ::Rml::ElementFormControlInput *pronounsInput() const;
    [[nodiscard]] ::Rml::ElementFormControlTextArea *bioTextarea() const;
    [[nodiscard]] ::Rml::Element *avatarPreview() const;
    [[nodiscard]] ::Rml::Element *bannerPreview() const;
    [[nodiscard]] ::Rml::Element *avatarUnavailable() const;
    [[nodiscard]] ::Rml::Element *bannerUnavailable() const;
    [[nodiscard]] ::Rml::Element *chooseAvatarButton() const;
    [[nodiscard]] ::Rml::Element *removeAvatarButton() const;
    [[nodiscard]] ::Rml::Element *chooseBannerButton() const;
    [[nodiscard]] ::Rml::Element *removeBannerButton() const;
    [[nodiscard]] ::Rml::Element *saveButton() const;
    [[nodiscard]] ::Rml::Element *resetButton() const;
    [[nodiscard]] ::Rml::Element *errorElement() const;

    bool formInitialized = false;
    std::string savedMessage;

    bool visible = false;
    bool loading = false;
    bool saving = false;
    bool hasSavedData = false;
    int generation = 0;
    Core::Snowflake editingAccountId;
    std::string error;
    Core::Snowflake editingGuildId;
    bool nicknameEditable = true;
    // The editing account has full Nitro (or its plan isn't known): banners and server identities.
    bool nitroFeatures = true;

    Discord::User savedUser;
    Discord::UserProfileData savedProfile;
    Discord::UserProfileData savedMainProfile;
    Discord::Member savedMember;

    ImageDraft avatarDraft;
    ImageDraft bannerDraft;
    std::string avatarFrameKey;
    std::string bannerFrameKey;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
