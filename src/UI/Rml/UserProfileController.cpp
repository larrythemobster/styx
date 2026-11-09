#include "UserProfileController.hpp"

#include "Shell.hpp"
#include "RmlView.hpp"
#include "RmlRuntime.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/MessageManager.hpp"
#include "Core/Presence/ActivityFormat.hpp"
#include "Core/Presence/PresenceBadge.hpp"
#include "Core/Presence/PresenceManager.hpp"
#include "Core/UserManager.hpp"
#include "Discord/CdnUrls.hpp"
#include "Discord/Client.hpp"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <algorithm>
#include <chrono>
#include <format>

namespace Styx::UI::Rml {
namespace {
std::string formatDate(const Platform::DateTime &date)
{
    return date.valid() ? date.format("%b %d, %Y") : "—";
}
}

UserProfileController::UserProfileController(Shell &shell) : Controller(shell, nullptr) {}
UserProfileController::~UserProfileController()
{
    activityTimer.stop();
    if (profileDocument) profileDocument->Close();
}
Core::ClientInstance *UserProfileController::clientInstance() const { return shell.currentInstance(); }
std::optional<Discord::User> UserProfileController::cachedUser() const
{
    auto *inst = clientInstance();
    return inst && inst->users() && currentUserId.isValid() ? inst->users()->getUser(currentUserId) : std::nullopt;
}
std::optional<Discord::Member> UserProfileController::cachedMember() const
{
    auto *inst = clientInstance();
    return inst && inst->users() && currentGuildId.isValid() && currentUserId.isValid()
        ? inst->users()->getMember(currentGuildId, currentUserId) : std::nullopt;
}
std::string UserProfileController::tagHandle() const
{
    if (auto u = cachedUser(); u && u->username.hasValue()) return "@" + u->username.get();
    if (hasProfile && profile.user.get().username.hasValue()) return "@" + profile.user.get().username.get();
    return "@unknown";
}
std::string UserProfileController::resolvedDisplayName() const
{
    if (auto *inst = clientInstance(); inst && inst->users() && currentGuildId.isValid()) {
        auto nick = inst->users()->getNickname(currentUserId, currentGuildId);
        if (!nick.empty()) return nick;
    }
    if (auto u = cachedUser()) return u->getDisplayName();
    return hasProfile ? profile.user.get().getDisplayName() : "Unknown User";
}
std::string UserProfileController::resolvedAvatarUrl(int size) const
{
    std::string memberHash, userHash;
    if (auto m = cachedMember()) memberHash = m->avatar.valueOr();
    if (auto u = cachedUser()) userHash = u->avatar.valueOr();
    if (userHash.empty() && hasProfile) userHash = profile.user.get().avatar.valueOr();
    return Discord::Cdn::effectiveAvatar(currentGuildId, currentUserId, memberHash, userHash, size);
}
int UserProfileController::resolvedAccentColor() const
{
    if (hasProfile && profile.guildMemberProfile.hasValue() && profile.guildMemberProfile.get().accentColor.hasValue()) return profile.guildMemberProfile.get().accentColor.get();
    if (hasProfile && profile.userProfile.hasValue() && profile.userProfile.get().accentColor.hasValue()) return profile.userProfile.get().accentColor.get();
    if (hasProfile && profile.user.get().accentColor.hasValue()) return profile.user.get().accentColor.get();
    if (auto u = cachedUser(); u && u->accentColor.hasValue()) return u->accentColor.get();
    return 0;
}
bool UserProfileController::isBot() const
{
    if (auto u = cachedUser(); u && u->bot.valueOr(false)) return true;
    return hasProfile && profile.user.get().bot.valueOr(false);
}
void UserProfileController::ensureDocument()
{
    if (profileDocument || !shell.view() || !shell.view()->context()) return;
    profileDocument = shell.view()->context()->LoadDocument(":/ui/user_profile.rml");
    if (!profileDocument) return;
    regionRoot = profileDocument;
    listen("click", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        if (!target) return;
        if (target->GetId() == "profile-backdrop" || target->GetId() == "user-profile-root") {
            event.StopPropagation(); hide(); return;
        }
        if (target->GetId() == "profile-message-btn" || closest(target, "profile-message-btn")) {
            event.StopPropagation();
            auto account = shell.location().accountId;
            auto user = currentUserId;
            hide(); shell.openDmWithUser(account, user);
        }
        if (closest(target, "data-action")) {
            event.StopPropagation();
            const auto user = currentUserId, guild = currentGuildId;
            hide(); shell.showContextMenuForUser(user, guild);
        }
    }, profileDocument);
    listen("keydown", [this](::Rml::Event &event) {
        if (event.GetParameter<int>("key_identifier", 0) == ::Rml::Input::KI_ESCAPE) {
            event.StopPropagation(); hide();
        }
    }, profileDocument);
    listen("change", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        if (target && target->GetId() == "profile-note-input") noteDirty = true;
    }, profileDocument);
}
void UserProfileController::positionCard(const Platform::Rect &anchor)
{
    if (!profileDocument) return;
    auto *card = profileDocument->GetElementById("profile-card");
    if (!card) return;
    constexpr float w = 320, h = 460;
    float winW = shell.view() ? static_cast<float>(shell.view()->width()) : 1280;
    float winH = shell.view() ? static_cast<float>(shell.view()->height()) : 720;
    float left = (winW - w) / 2, top = (winH - h) / 2;
    if (!anchor.empty()) {
        left = anchor.x + anchor.width / 2 > winW / 2 ? anchor.x - w - 8 : anchor.x + anchor.width + 8;
        top = anchor.y;
        left = std::max(10.0f, std::min(left, winW - w - 10));
        top = std::max(10.0f, std::min(top, winH - h - 10));
    }
    card->SetProperty("left", std::format("{}px", left));
    card->SetProperty("top", std::format("{}px", top));
}
void UserProfileController::updateProfileData()
{
    auto *inst = clientInstance();
    if (!profileDocument || !inst) return;
    if (auto *el = profileDocument->GetElementById("profile-display-name")) el->SetInnerRML(escape(resolvedDisplayName()));
    if (auto *el = profileDocument->GetElementById("profile-handle")) el->SetInnerRML(escape(tagHandle()));
    if (auto *el = profileDocument->GetElementById("profile-bot-badge")) el->SetClass("visible", isBot());
    std::string pronouns;
    if (hasProfile && profile.guildMemberProfile.hasValue()) pronouns = profile.guildMemberProfile.get().pronouns.valueOr();
    if (pronouns.empty() && hasProfile && profile.userProfile.hasValue()) pronouns = profile.userProfile.get().pronouns.valueOr();
    if (auto *el = profileDocument->GetElementById("profile-pronouns")) { el->SetInnerRML(escape(pronouns)); el->SetClass("visible", !pronouns.empty()); }
    if (auto *el = profileDocument->GetElementById("profile-avatar-img")) el->SetAttribute("src", shell.imageSource(resolvedAvatarUrl(128), {68, 68}));
    std::string banner;
    if (hasProfile && profile.guildMemberProfile.hasValue()) banner = profile.guildMemberProfile.get().banner.valueOr();
    if (banner.empty() && hasProfile) banner = profile.user.get().banner.valueOr();
    if (auto *el = profileDocument->GetElementById("profile-banner-img")) {
        if (!banner.empty()) el->SetAttribute("src", shell.imageSource(Discord::Cdn::userBanner(currentUserId, banner, 640), {320, 105}));
        el->SetClass("visible", !banner.empty());
    }
    if (auto *el = profileDocument->GetElementById("profile-banner")) {
        auto color = resolvedAccentColor();
        el->SetProperty("background-color", color ? Platform::Color::rgb(color).css() : "#5865f2");
    }
    std::string bio;
    if (hasProfile && profile.guildMemberProfile.hasValue()) bio = profile.guildMemberProfile.get().bio.valueOr();
    if (bio.empty() && hasProfile && profile.userProfile.hasValue()) bio = profile.userProfile.get().bio.valueOr();
    if (auto *section = profileDocument->GetElementById("profile-bio-section")) {
        section->SetProperty("display", bio.empty() ? "none" : "block");
        if (!bio.empty()) if (auto *el = profileDocument->GetElementById("profile-bio")) el->SetInnerRML(inst->messages()->profileBioHtml(bio));
    }
    if (!noteDirty) {
        if (auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(profileDocument->GetElementById("profile-note-input"))) {
            initialNote = inst->users()->getCachedNote(currentUserId).value_or(std::string());
            input->SetValue(initialNote);
        }
    }
    renderPresenceAndActivities(); renderRoles(); renderDates();
}
void UserProfileController::renderPresenceAndActivities()
{
    auto *inst = clientInstance();
    if (!profileDocument || !inst || !inst->presences()) return;
    auto *presences = inst->presences();
    auto badge = presences->badge(currentUserId, currentGuildId);
    if (auto *dot = profileDocument->GetElementById("profile-status-dot")) {
        dot->SetClass("online", badge.status == Discord::StatusType::ONLINE && !badge.streaming);
        dot->SetClass("idle", badge.status == Discord::StatusType::IDLE && !badge.streaming);
        dot->SetClass("dnd", badge.status == Discord::StatusType::DND && !badge.streaming);
        dot->SetClass("offline", badge.status == Discord::StatusType::OFFLINE && !badge.streaming);
        dot->SetClass("streaming", badge.streaming);
    }
    auto activities = presences->activities(currentUserId, currentGuildId);
    auto *custom = Core::ActivityFormat::custom(activities);
    if (auto *row = profileDocument->GetElementById("profile-custom-status")) {
        const bool show = custom && !custom->stateText().empty();
        row->SetClass("visible", show);
        if (show) {
            if (auto *el = profileDocument->GetElementById("profile-status-text")) el->SetInnerRML(escape(custom->stateText()));
            if (auto *el = profileDocument->GetElementById("profile-status-emoji")) {
                auto emoji = custom->emoji.hasValue() ? custom->emoji.get() : Discord::ActivityEmoji();
                std::string src;
                if (!emoji.isUnicode() && emoji.id.hasValue()) src = shell.imageSource(emoji.stillImageUrl(32), {18, 18});
                else if (!emoji.nameText().empty()) src = Runtime::emojiSource(emoji.nameText());
                if (!src.empty()) el->SetAttribute("src", src);
                el->SetProperty("display", src.empty() ? "none" : "inline-block");
            }
        }
    }
    if (auto *container = profileDocument->GetElementById("profile-activities")) {
        std::string rml;
        auto cards = Core::ActivityFormat::profileCardActivities(Core::ActivityFormat::sortAndFilter(activities));
        for (const auto &act : cards) {
            auto heading = Core::ActivityFormat::cardHeading(act);
            bool named = Core::ActivityFormat::headingHasName(act);
            auto title = named ? act.detailsText() : act.nameText();
            auto details = named ? std::string() : act.detailsText();
            auto state = Core::ActivityFormat::partyText(act);
            auto timer = Core::ActivityFormat::timerText(Core::ActivityFormat::timerFor(act), Platform::DateTime::now().unixMillis());
            std::string image;
            if (act.assets.hasValue() && act.assets->largeImage.hasValue() && act.applicationId.hasValue()) image = Discord::Cdn::activityAsset(act.applicationId.get(), act.assets->largeImage.get(), 96);
            if (image.empty() && act.applicationId.hasValue() && inst->discord()) image = Discord::Cdn::applicationIcon(act.applicationId.get(), inst->discord()->applicationIconHash(act.applicationId.get()), 96);
            rml += "<div class=\"activity-card\"><div class=\"activity-header\">" + escape(heading) + "</div><div class=\"activity-content\">";
            if (!image.empty()) rml += "<div class=\"activity-image-wrap\"><img class=\"activity-large-img\" src=\"" + escape(shell.imageSource(image, {48, 48})) + "\"/></div>";
            rml += "<div class=\"activity-text-col\">";
            if (!title.empty()) rml += "<div class=\"activity-name\">" + escape(title) + "</div>";
            if (!details.empty()) rml += "<div class=\"activity-details\">" + escape(details) + "</div>";
            if (!state.empty()) rml += "<div class=\"activity-state\">" + escape(state) + "</div>";
            if (!timer.empty()) rml += "<div class=\"activity-timer\">" + escape(timer) + "</div>";
            rml += "</div></div></div>";
        }
        container->SetInnerRML(rml);
        if (auto *section = profileDocument->GetElementById("profile-activities-section")) section->SetProperty("display", cards.empty() ? "none" : "block");
    }
}
void UserProfileController::renderRoles()
{
    if (!profileDocument) return;
    auto *inst = clientInstance();
    std::vector<Discord::Role> roles;
    if (inst && currentGuildId.isValid()) roles = inst->getMemberRolesSorted(currentGuildId, currentUserId);
    if (auto *container = profileDocument->GetElementById("profile-roles")) {
        std::string rml;
        for (const auto &role : roles) {
            auto color = role.hasColor() ? role.getColor() : Platform::Color::rgb(0x949ba4);
            rml += "<div class=\"role-pill\"><div class=\"role-dot\" style=\"background-color: " + color.css() + ";\"></div><span>" + escape(role.name.get()) + "</span></div>";
        }
        container->SetInnerRML(rml);
    }
    if (auto *section = profileDocument->GetElementById("profile-roles-section")) section->SetProperty("display", roles.empty() ? "none" : "block");
}
void UserProfileController::renderDates()
{
    if (!profileDocument) return;
    auto *container = profileDocument->GetElementById("profile-dates");
    if (!container) return;
    std::string rml = "<div class=\"dates-row\">Discord: <b>" + escape(formatDate(currentUserId.toDateTime())) + "</b></div>";
    if (currentGuildId.isValid()) {
        Platform::DateTime joined;
        if (auto m = cachedMember(); m && m->joinedAt.hasValue()) joined = m->joinedAt.get();
        else if (hasProfile && profile.guildMember.hasValue() && profile.guildMember.get().joinedAt.hasValue()) joined = profile.guildMember.get().joinedAt.get();
        if (joined.valid()) rml += "<div class=\"dates-row\">Server: <b>" + escape(formatDate(joined)) + "</b></div>";
    }
    container->SetInnerRML(rml);
}
void UserProfileController::saveNoteIfDirty()
{
    if (!noteDirty || !currentUserId.isValid() || isBot()) return;
    auto *inst = clientInstance();
    if (!inst || !inst->discord()) return;
    if (auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(profileDocument ? profileDocument->GetElementById("profile-note-input") : nullptr)) {
        auto note = input->GetValue();
        if (note != initialNote) { inst->discord()->setUserNote(currentUserId, note); initialNote = note; }
    }
    noteDirty = false;
}
void UserProfileController::showProfile(Core::Snowflake userId, Core::Snowflake guildId, const Platform::Rect &anchor)
{
    ensureDocument();
    if (!profileDocument) return;
    if (visible) saveNoteIfDirty();
    currentUserId = userId; currentGuildId = guildId; hasProfile = false; noteDirty = false;
    auto generation = ++requestGeneration;
    updateProfileData(); positionCard(anchor);
    profileDocument->Show(); profileDocument->PullToFront(); profileDocument->SetClass("open", true);
    visible = true;
    activityTimer.start(std::chrono::seconds(1), true, [this] { if (visible) { renderPresenceAndActivities(); requestRender(); } });
    requestRender();
    if (auto *inst = clientInstance(); inst && inst->discord()) {
        Platform::Guard<UserProfileController> self(this);
        inst->discord()->fetchUserProfile(userId, guildId, [self, generation](const Core::Result<Discord::UserProfile> &result) {
            if (!self || !self->visible || self->requestGeneration != generation) return;
            if (result.success()) { self->profile = result.value.value(); self->hasProfile = true; self->updateProfileData(); self->requestRender(); }
        });
    }
}
void UserProfileController::hide()
{
    if (!profileDocument || !visible) return;
    saveNoteIfDirty(); ++requestGeneration; activityTimer.stop();
    profileDocument->SetClass("open", false); profileDocument->Hide(); visible = false; requestRender();
}
} // namespace Styx::UI::Rml
