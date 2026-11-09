#pragma once

#include "Controller.hpp"
#include "Core/Snowflake.hpp"
#include "Discord/Entities.hpp"
#include "Discord/UserEdits.hpp"
#include "Platform/Signal.hpp"

#include <string>

namespace Rml {
class ElementDocument;
class ElementFormControlInput;
class ElementFormControlTextArea;
} // namespace Rml

namespace Styx {
namespace UI {
namespace Rml {

class ProfileEditorController final : public Controller
{
public:
    explicit ProfileEditorController(Shell &shell);
    ~ProfileEditorController() override;

    void open();
    void close();

    Platform::Signal<> closed;

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
    void reset();
    void save();
    void saveAccount(const Discord::CurrentUserEdit &accountEdit, Discord::UserProfileEdit profileEdit);
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

    ::Rml::ElementDocument *profileEditorDocument = nullptr;

    bool visible = false;
    bool loading = false;
    bool saving = false;
    bool hasSavedData = false;
    int generation = 0;
    Core::Snowflake editingAccountId;
    std::string error;

    Discord::User savedUser;
    Discord::UserProfileData savedProfile;

    ImageDraft avatarDraft;
    ImageDraft bannerDraft;
    std::string avatarFrameKey;
    std::string bannerFrameKey;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
