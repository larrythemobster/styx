#include "ProfileEditorController.hpp"

#include "Discord/CdnUrls.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/Theme/Manager.hpp"
#include "Discord/Client.hpp"
#include "ImageUpload.hpp"
#include "Platform/Desktop.hpp"
#include "Platform/Native.hpp"
#include "RmlRuntime.hpp"
#include "RmlView.hpp"
#include "Shell.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Styx::UI::Rml {

namespace {

void setControlDisabled(::Rml::Element *element, bool disabled)
{
    if (!element)
        return;
    if (disabled)
        element->SetAttribute("disabled", "");
    else
        element->RemoveAttribute("disabled");
    if (element->GetTagName() == "button")
        element->SetPseudoClass("disabled", disabled);
}

} // namespace

ProfileEditorController::ProfileEditorController(Shell &shell, ::Rml::Element &host)
    : Controller(shell, &host)
{
    const auto scope = std::to_string(reinterpret_cast<std::uintptr_t>(this));
    avatarFrameKey = "profile-editor-avatar-" + scope;
    bannerFrameKey = "profile-editor-banner-" + scope;
    track(this->shell.instanceChanged.connect([this](Core::ClientInstance *) {
        if (visible)
            close();
        else
            ++generation;
    }));
}

ProfileEditorController::~ProfileEditorController()
{
    visible = false;
    saving = false;
    loading = false;
    clearFrames();
    removeListeners();
}

void ProfileEditorController::open()
{
    ensureDocument();
    if (!regionRoot)
        return;

    ++generation;
    saving = false;
    loading = false;
    error.clear();
    savedMessage.clear();
    hasSavedData = false;
    savedUser = Discord::User{};
    savedProfile = Discord::UserProfileData{};

    clearDrafts();
    applyBaselineToInputs();

    auto *inst = shell.currentInstance();
    editingAccountId = inst ? inst->accountId() : Core::Snowflake::Invalid;

    visible = true;

    if (!editingAccountId.isValid() || inst->discord()->getMe().id.get() != editingAccountId) {
        error = editingAccountId.isValid() ? "Account is not ready. Retry after it connects." : "No active account.";
        render();
        regionRoot->Focus();
        return;
    }

    loading = true;
    render();
    regionRoot->Focus();

    const auto currentGen = generation;
    const auto accountId = editingAccountId;
    Platform::Guard<ProfileEditorController> self(this);

    inst->discord()->fetchUserProfile(accountId, Core::Snowflake::Invalid, [self, currentGen, accountId](const Core::Result<Discord::UserProfile> &result) {
        if (!self || !self->visible || self->generation != currentGen || self->editingAccountId != accountId)
            return;

        self->loading = false;
        if (result.success() && result.value->user->id.get() == accountId) {
            self->savedUser = result.value->user.get();
            self->savedProfile = result.value->userProfile.get();
            self->hasSavedData = true;
            self->applyBaselineToInputs();
            self->render();
            if (auto *nameEl = self->nameInput())
                nameEl->Focus();
            else
                self->regionRoot->Focus();
        } else {
            self->error = result.error.empty() ? "Failed to load profile." : result.error;
            self->render();
            self->regionRoot->Focus();
        }
    });
}

void ProfileEditorController::close()
{
    if (!visible)
        return;

    visible = false;
    saving = false;
    loading = false;
    ++generation;
    clearDrafts();
    requestRender();
}

void ProfileEditorController::ensureDocument()
{
    if (formInitialized)
        return;

    auto *view = shell.view();
    if (!view || !view->context())
        return;

    auto *form = view->context()->LoadDocument("ui/profile_editor.rml");
    if (!form)
        return;
    regionRoot->SetInnerRML(form->GetInnerRML());
    form->Close();
    formInitialized = true;
    setupEventListeners();
}

void ProfileEditorController::setupEventListeners()
{
    listen("change", [this](::Rml::Event &event) {
        savedMessage.clear();
        updatePreview(event.GetTargetElement());
        setControlDisabled(saveButton(), loading || saving || !hasSavedData || !hasPendingChanges());
        if (auto *state = regionRoot->GetElementById("profile-edit-state"); state && !loading && !saving)
            state->SetProperty("display", "none");
    });
    listen("click", [this](::Rml::Event &event) {
        auto *actionEl = closest(event.GetTargetElement(), "data-action");
        if (!actionEl)
            return;

        const std::string action = attribute(actionEl, "data-action");

        if (action == "reset") {
            reset();
            return;
        }

        if (loading || saving || !editingAccountId.isValid())
            return;

        if (action == "choose-avatar") {
            chooseImage(ImageSlot::Avatar);
        } else if (action == "remove-avatar") {
            removeImage(ImageSlot::Avatar);
        } else if (action == "choose-banner") {
            chooseImage(ImageSlot::Banner);
        } else if (action == "remove-banner") {
            removeImage(ImageSlot::Banner);
        } else if (action == "save") {
            save();
        }
    });


    auto submitOnEnter = [this](::Rml::Event &event) {
        const int key = event.GetParameter<int>("key_identifier", 0);
        if (key != ::Rml::Input::KI_RETURN && key != ::Rml::Input::KI_NUMPADENTER)
            return;
        event.StopPropagation();
        if (!saving && !loading && editingAccountId.isValid() && hasSavedData)
            save();
    };

    if (auto *inputEl = nameInput())
        listen("keydown", submitOnEnter, inputEl);
    if (auto *inputEl = pronounsInput())
        listen("keydown", submitOnEnter, inputEl);
}

void ProfileEditorController::render()
{
    if (!regionRoot)
        return;

    const std::string empty;
    std::string avatarSrc;
    if (avatarDraft.removed) {
        avatarSrc = shell.imageSource(Discord::Cdn::defaultUserAvatar(editingAccountId, 128), {80, 80});
    } else if (!avatarDraft.uri.empty()) {
        if (!avatarDraft.previewEmpty)
            avatarSrc = "frame:" + frameKey(ImageSlot::Avatar);
    } else if (hasSavedData) {
        const auto &avatarHash = savedUser.avatar.hasValue() ? savedUser.avatar.get() : empty;
        if (!avatarHash.empty())
            avatarSrc = shell.imageSource(Discord::Cdn::userAvatar(editingAccountId, avatarHash, 128), {80, 80});
        else
            avatarSrc = shell.imageSource(Discord::Cdn::defaultUserAvatar(editingAccountId, 128), {80, 80});
    } else {
        avatarSrc = shell.imageSource(Discord::Cdn::defaultUserAvatar(editingAccountId, 128), {80, 80});
    }

    std::string bannerSrc;
    if (bannerDraft.removed) {
        bannerSrc.clear();
    } else if (!bannerDraft.uri.empty()) {
        if (!bannerDraft.previewEmpty)
            bannerSrc = "frame:" + frameKey(ImageSlot::Banner);
    } else if (hasSavedData) {
        const auto &banner = savedProfile.banner.isUndefined() ? savedUser.banner : savedProfile.banner;
        const auto &bannerHash = banner.hasValue() ? banner.get() : empty;
        if (!bannerHash.empty())
            bannerSrc = shell.imageSource(Discord::Cdn::userBanner(editingAccountId, bannerHash, 1024), {600, 240});
    }

    if (auto *avatarImg = avatarPreview()) {
        if (!avatarDraft.uri.empty() && avatarDraft.previewEmpty) {
            avatarImg->SetProperty("display", "none");
            if (auto *unavail = avatarUnavailable())
                unavail->SetProperty("display", "flex");
        } else {
            if (auto *unavail = avatarUnavailable())
                unavail->SetProperty("display", "none");
            avatarImg->SetProperty("display", "block");
            avatarImg->SetAttribute("src", avatarSrc);
        }
    }

    if (auto *bannerImg = bannerPreview()) {
        if (!bannerDraft.uri.empty() && bannerDraft.previewEmpty) {
            bannerImg->SetProperty("display", "none");
            if (auto *unavail = bannerUnavailable())
                unavail->SetProperty("display", "flex");
        } else {
            if (auto *unavail = bannerUnavailable())
                unavail->SetProperty("display", "none");
            if (!bannerSrc.empty()) {
                bannerImg->SetProperty("display", "block");
                bannerImg->SetAttribute("src", bannerSrc);
            } else {
                bannerImg->SetProperty("display", "none");
                bannerImg->RemoveAttribute("src");
            }
        }
    }

    if (auto *errorEl = errorElement()) {
        errorEl->SetInnerRML(Platform::htmlEscape(error));
        errorEl->SetProperty("display", error.empty() ? "none" : "block");
    }

    const bool busy = loading || saving || !editingAccountId.isValid() || !hasSavedData;
    setControlDisabled(nameInput(), busy);
    setControlDisabled(pronounsInput(), busy);
    setControlDisabled(bioTextarea(), busy);
    setControlDisabled(chooseAvatarButton(), busy);
    setControlDisabled(removeAvatarButton(), busy || avatarDraft.removed ||
        (avatarDraft.uri.empty() && (!savedUser.avatar.hasValue() || savedUser.avatar->empty())));
    setControlDisabled(chooseBannerButton(), busy);
    const auto &currentBanner = savedProfile.banner.isUndefined() ? savedUser.banner : savedProfile.banner;
    setControlDisabled(removeBannerButton(), busy || bannerDraft.removed ||
        (bannerDraft.uri.empty() && (!currentBanner.hasValue() || currentBanner->empty())));
    setControlDisabled(saveButton(), busy || !hasPendingChanges());
    setControlDisabled(resetButton(), loading || saving || !editingAccountId.isValid());
    if (auto *state = regionRoot->GetElementById("profile-edit-state")) {
        const std::string text = loading ? "Loading profile..." : saving ? "Saving changes..." : savedMessage;
        state->SetInnerRML(text);
        state->SetProperty("display", text.empty() ? "none" : "block");
    }
    updatePreview();
    requestRender();
}

void ProfileEditorController::updatePreview(::Rml::Element *changed)
{
    const std::string_view id = changed ? changed->GetId() : std::string_view{};
    auto text = [this](const char *target, const std::string &value) {
        if (auto *element = regionRoot->GetElementById(target))
            element->SetInnerRML(Platform::htmlEscape(value));
    };
    if (!changed || id == "profile-edit-name") {
        const auto *input = nameInput();
        const auto name = input ? input->GetValue() : "";
        text("profile-live-name", name.empty() ? savedUser.username.get() : name);
    }
    if (!changed) text("profile-live-username", savedUser.username.get());
    if (!changed || id == "profile-edit-pronouns") {
        const auto *input = pronounsInput();
        text("profile-live-pronouns", input ? input->GetValue() : "");
    }
    if (!changed || id == "profile-edit-bio") {
        const auto *input = bioTextarea();
        text("profile-live-bio", input ? input->GetValue() : "");
    }
    if (changed) return;
    for (const auto &[original, preview] : {std::pair{avatarPreview(), "profile-live-avatar"},
                                          std::pair{bannerPreview(), "profile-live-banner-image"}}) {
        if (auto *image = regionRoot->GetElementById(preview); image && original) {
            const auto source = original->GetAttribute<std::string>("src", "");
            image->SetAttribute("src", source);
            image->SetProperty("display", source.empty() ? "none" : "block");
        }
    }
    if (auto *banner = regionRoot->GetElementById("profile-live-banner"))
        banner->SetProperty("background-color", savedProfile.accentColor.hasValue()
            ? Platform::Color::rgb(uint32_t(savedProfile.accentColor.get())).css()
            : Core::Theme::Manager::instance().color(Core::Theme::Token::Highlight).css());
}

void ProfileEditorController::reset()
{
    if (loading || saving)
        return;

    if (!hasSavedData) {
        open();
        return;
    }
    ++generation;

    error.clear();
    savedMessage.clear();
    clearDrafts();
    applyBaselineToInputs();
    render();
}

void ProfileEditorController::save()
{
    if (loading || saving || !editingAccountId.isValid() || !hasSavedData)
        return;

    auto *inst = shell.currentInstance();
    if (!inst || !inst->discord())
        return;

    ++generation;
    Discord::CurrentUserEdit accountEdit;
    Discord::UserProfileEdit profileEdit;
    const std::string empty;

    const auto *nameEl = nameInput();
    const std::string currentName = nameEl ? nameEl->GetValue() : "";
    const auto &baselineName = savedUser.globalName.hasValue() ? savedUser.globalName.get() : empty;
    if (currentName != baselineName) {
        if (currentName.empty())
            accountEdit.globalName = nullptr;
        else
            accountEdit.globalName = currentName;
    }

    if (avatarDraft.removed && savedUser.avatar.hasValue() && !savedUser.avatar->empty()) {
        accountEdit.avatar = nullptr;
    } else if (!avatarDraft.uri.empty()) {
        accountEdit.avatar = avatarDraft.uri;
    }

    const auto *pronounsEl = pronounsInput();
    const std::string currentPronouns = pronounsEl ? pronounsEl->GetValue() : "";
    const auto &baselinePronouns = savedProfile.pronouns.get();
    if (currentPronouns != baselinePronouns)
        profileEdit.pronouns = currentPronouns;

    const auto *bioEl = bioTextarea();
    const std::string currentBio = bioEl ? bioEl->GetValue() : "";
    const auto &baselineBio = savedProfile.bio.hasValue() ? savedProfile.bio.get() : empty;
    if (currentBio != baselineBio)
        profileEdit.bio = currentBio;

    const auto &baselineBanner = savedProfile.banner.isUndefined() ? savedUser.banner : savedProfile.banner;
    if (bannerDraft.removed && baselineBanner.hasValue() && !baselineBanner->empty()) {
        profileEdit.banner = nullptr;
    } else if (!bannerDraft.uri.empty()) {
        profileEdit.banner = bannerDraft.uri;
    }

    if (accountEdit.empty() && profileEdit.empty()) {
        render();
        return;
    }

    if (!accountEdit.empty())
        saveAccount(accountEdit, std::move(profileEdit));
    else
        saveMetadata(profileEdit, false);
}

void ProfileEditorController::saveAccount(const Discord::CurrentUserEdit &accountEdit, Discord::UserProfileEdit profileEdit)
{
    auto *inst = shell.currentInstance();
    if (!inst || !inst->discord())
        return;

    saving = true;
    error.clear();
    render();

    std::map<std::string, std::string> avatarMd5s;
    if (!avatarDraft.originalMd5.empty() && !avatarDraft.uri.empty())
        avatarMd5s["avatar"] = avatarDraft.originalMd5;

    const auto currentGen = generation;
    const auto accountId = editingAccountId;
    Platform::Guard<ProfileEditorController> self(this);

    inst->discord()->modifyCurrentUser(accountEdit, avatarMd5s, [self, currentGen, accountId, profileEdit = std::move(profileEdit)](const Core::Result<Discord::User> &result) {
        if (!self || !self->visible || self->generation != currentGen || self->editingAccountId != accountId)
            return;

        if (!result.success()) {
            self->saving = false;
            self->error = result.error.empty() ? "Failed to save account changes." : result.error;
            self->render();
            return;
        }

        self->savedUser = result.value.value();
        if (auto *input = self->nameInput())
            input->SetValue(self->savedUser.globalName.valueOr(""));
        Runtime::instance().clearFrame(self->frameKey(ImageSlot::Avatar));
        self->avatarDraft = {};

        if (!profileEdit.empty()) {
            self->saveMetadata(profileEdit, true);
        } else {
            self->finishSave();
        }
    });
}

void ProfileEditorController::saveMetadata(const Discord::UserProfileEdit &profileEdit, bool accountAlreadySaved)
{
    auto *inst = shell.currentInstance();
    if (!inst || !inst->discord())
        return;

    error.clear();
    saving = true;
    render();

    std::map<std::string, std::string> bannerMd5s;
    if (!bannerDraft.originalMd5.empty() && !bannerDraft.uri.empty())
        bannerMd5s["banner"] = bannerDraft.originalMd5;

    const auto currentGen = generation;
    const auto accountId = editingAccountId;
    Platform::Guard<ProfileEditorController> self(this);

    inst->discord()->modifyCurrentUserProfile(profileEdit, bannerMd5s, [self, currentGen, accountId, accountAlreadySaved](const Core::Result<Discord::UserProfileData> &result) {
        if (!self || !self->visible || self->generation != currentGen || self->editingAccountId != accountId)
            return;

        self->saving = false;
        if (!result.success()) {
            std::string msg = result.error.empty() ? "Failed to save profile changes." : result.error;
            if (accountAlreadySaved)
                msg = "Account changes were saved, but profile changes failed: " + msg;
            self->error = std::move(msg);
            self->render();
            return;
        }

        self->savedProfile = result.value.value();
        self->finishSave();
    });
}

void ProfileEditorController::finishSave()
{
    saving = false;
    clearDrafts();
    applyBaselineToInputs();
    savedMessage = "Changes saved.";
    render();
}

bool ProfileEditorController::hasPendingChanges() const
{
    if (!visible || loading || !hasSavedData)
        return false;
    const auto *name = nameInput();
    const auto *pronouns = pronounsInput();
    const auto *bio = bioTextarea();
    const auto &banner = savedProfile.banner.isUndefined() ? savedUser.banner : savedProfile.banner;
    return (name && name->GetValue() != savedUser.globalName.valueOr("")) ||
        (pronouns && pronouns->GetValue() != savedProfile.pronouns.get()) ||
        (bio && bio->GetValue() != savedProfile.bio.valueOr("")) ||
        !avatarDraft.uri.empty() || !bannerDraft.uri.empty() ||
        (avatarDraft.removed && savedUser.avatar.hasValue() && !savedUser.avatar->empty()) ||
        (bannerDraft.removed && banner.hasValue() && !banner->empty());
}

void ProfileEditorController::chooseImage(ImageSlot slot)
{
    if (loading || saving || !editingAccountId.isValid() || !hasSavedData)
        return;
    ++generation;

    const char *title = slot == ImageSlot::Avatar ? "Change Avatar" : "Change Banner";
    const Platform::Size bounds = slot == ImageSlot::Avatar ? Platform::Size{1024, 1024} : Platform::Size{1200, 480};
    Platform::Guard<ProfileEditorController> self(this);
    const auto accountId = editingAccountId;
    const auto currentGen = generation;

    Platform::Desktop::openFiles(title, {{"Images", "*.png;*.jpg;*.jpeg;*.gif;*.webp;*.avif;*.bmp;*.svg"}},
        [self, accountId, currentGen, slot, bounds](std::vector<std::string> files) {
            if (!self || files.empty() || !self->visible || self->generation != currentGen || self->editingAccountId != accountId)
                return;

            auto prepared = ImageUpload::prepareCropped(files.front(), bounds.width, bounds.height);
            if (prepared.dataUri.empty()) {
                self->error = prepared.error.empty() ? "Failed to prepare image." : prepared.error;
                self->render();
                return;
            }

            self->error.clear();
            auto &draft = self->imageDraft(slot);
            draft.uri = std::move(prepared.dataUri);
            draft.originalMd5 = std::move(prepared.originalMd5);
            draft.removed = false;
            draft.previewEmpty = prepared.preview.empty();
            if (!draft.previewEmpty)
                Runtime::instance().setFrame(self->frameKey(slot), prepared.preview);
            else
                Runtime::instance().clearFrame(self->frameKey(slot));
            self->render();
        }, false);
}

void ProfileEditorController::removeImage(ImageSlot slot)
{
    if (loading || saving || !editingAccountId.isValid() || !hasSavedData)
        return;
    ++generation;

    auto &draft = imageDraft(slot);
    draft = {};
    draft.removed = true;
    Runtime::instance().clearFrame(frameKey(slot));

    error.clear();
    render();
}

void ProfileEditorController::clearFrames()
{
    Runtime::instance().clearFrame(frameKey(ImageSlot::Avatar));
    Runtime::instance().clearFrame(frameKey(ImageSlot::Banner));
}

void ProfileEditorController::clearDrafts()
{
    avatarDraft = {};
    bannerDraft = {};
    clearFrames();
}

ProfileEditorController::ImageDraft &ProfileEditorController::imageDraft(ImageSlot slot)
{
    return slot == ImageSlot::Avatar ? avatarDraft : bannerDraft;
}

void ProfileEditorController::applyBaselineToInputs()
{
    const std::string empty;
    if (auto *el = nameInput())
        el->SetValue(savedUser.globalName.hasValue() ? savedUser.globalName.get() : empty);
    if (auto *el = pronounsInput())
        el->SetValue(savedProfile.pronouns.get());
    if (auto *el = bioTextarea())
        el->SetValue(savedProfile.bio.hasValue() ? savedProfile.bio.get() : empty);
}

const std::string &ProfileEditorController::frameKey(ImageSlot slot) const
{
    return slot == ImageSlot::Avatar ? avatarFrameKey : bannerFrameKey;
}

::Rml::ElementFormControlInput *ProfileEditorController::nameInput() const
{
    return regionRoot ? static_cast<::Rml::ElementFormControlInput *>(regionRoot->GetElementById("profile-edit-name")) : nullptr;
}

::Rml::ElementFormControlInput *ProfileEditorController::pronounsInput() const
{
    return regionRoot ? static_cast<::Rml::ElementFormControlInput *>(regionRoot->GetElementById("profile-edit-pronouns")) : nullptr;
}

::Rml::ElementFormControlTextArea *ProfileEditorController::bioTextarea() const
{
    return regionRoot ? static_cast<::Rml::ElementFormControlTextArea *>(regionRoot->GetElementById("profile-edit-bio")) : nullptr;
}

::Rml::Element *ProfileEditorController::avatarPreview() const
{
    return regionRoot ? regionRoot->GetElementById("profile-edit-avatar-preview") : nullptr;
}

::Rml::Element *ProfileEditorController::bannerPreview() const
{
    return regionRoot ? regionRoot->GetElementById("profile-edit-banner-preview") : nullptr;
}

::Rml::Element *ProfileEditorController::avatarUnavailable() const
{
    return regionRoot ? regionRoot->GetElementById("profile-edit-avatar-unavailable") : nullptr;
}

::Rml::Element *ProfileEditorController::bannerUnavailable() const
{
    return regionRoot ? regionRoot->GetElementById("profile-edit-banner-unavailable") : nullptr;
}

::Rml::Element *ProfileEditorController::chooseAvatarButton() const
{
    return regionRoot ? regionRoot->GetElementById("profile-edit-choose-avatar") : nullptr;
}

::Rml::Element *ProfileEditorController::removeAvatarButton() const
{
    return regionRoot ? regionRoot->GetElementById("profile-edit-remove-avatar") : nullptr;
}

::Rml::Element *ProfileEditorController::chooseBannerButton() const
{
    return regionRoot ? regionRoot->GetElementById("profile-edit-choose-banner") : nullptr;
}

::Rml::Element *ProfileEditorController::removeBannerButton() const
{
    return regionRoot ? regionRoot->GetElementById("profile-edit-remove-banner") : nullptr;
}

::Rml::Element *ProfileEditorController::saveButton() const
{
    return regionRoot ? regionRoot->GetElementById("profile-edit-save") : nullptr;
}

::Rml::Element *ProfileEditorController::resetButton() const
{
    return regionRoot ? regionRoot->GetElementById("profile-edit-reset") : nullptr;
}

::Rml::Element *ProfileEditorController::errorElement() const
{
    return regionRoot ? regionRoot->GetElementById("profile-edit-error") : nullptr;
}

} // namespace Styx::UI::Rml
