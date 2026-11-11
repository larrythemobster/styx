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
        if (target->GetId() == "profile-close") { event.StopPropagation(); hide(); return; }
        if (auto *tab = closest(target, "data-profile-tab")) {
            event.StopPropagation(); selectFullTab(attribute(tab, "data-profile-tab")); return;
        }
        if (auto *row = closest(target, "data-profile-user")) {
            event.StopPropagation();
            const auto userId = Core::JsonUtils::detail::snowflake(attribute(row, "data-profile-user"));
            if (!userId.isValid()) return;
            if (auto *inst = clientInstance(); inst && inst->users() && !inst->users()->getUser(userId)
                && profile.mutualFriends.hasValue()) {
                for (const auto &user : profile.mutualFriends.get())
                    if (user.id.get() == userId) { inst->users()->saveUser(user); break; }
            }
            showProfile(userId, Core::Snowflake::Invalid, {}, true);
            return;
        }
        if (auto *row = closest(target, "data-profile-guild")) {
            event.StopPropagation();
            const auto guildId = Core::JsonUtils::detail::snowflake(attribute(row, "data-profile-guild"));
            if (!guildId.isValid()) return;
            const auto accountId = shell.location().accountId;
            hide();
            shell.openGuild(accountId, guildId);
            return;
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
        const int key = event.GetParameter<int>("key_identifier", 0);
        if (key == ::Rml::Input::KI_ESCAPE) {
            event.StopPropagation(); hide(); return;
        }
        auto *target = event.GetTargetElement();
        if (key == ::Rml::Input::KI_RETURN || key == ::Rml::Input::KI_NUMPADENTER || key == ::Rml::Input::KI_SPACE) {
            if (target && target->GetTagName() == "button") {
                event.StopPropagation(); target->DispatchEvent("click", {}); return;
            }
        }
        if (!fullMode || key != ::Rml::Input::KI_TAB) return;
        ::Rml::ElementList controls;
        profileDocument->QuerySelectorAll(controls, "button, input");
        std::erase_if(controls, [](auto *el) { return !el->IsVisible() || el->HasAttribute("disabled"); });
        if (controls.empty()) return;
        const auto focused = profileDocument->GetContext()->GetFocusElement();
        auto position = std::find(controls.begin(), controls.end(), focused);
        int index = position == controls.end() ? -1 : static_cast<int>(position - controls.begin());
        const bool backwards = event.GetParameter<bool>("shift_key", false);
        const int count = static_cast<int>(controls.size());
        if (index < 0) index = backwards ? 0 : count - 1;
        index = (index + (backwards ? -1 : 1) + count) % count;
        event.StopPropagation();
        controls[index]->Focus();
        controls[index]->ScrollIntoView();
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
    if (fullMode) {
        card->SetProperty("width", "100%");
        card->SetProperty("height", "100%");
        card->SetProperty("left", "auto");
        card->SetProperty("top", "auto");
        return;
    }
    auto *context = profileDocument->GetContext();
    const auto dimensions = context->GetDimensions();
    const float scale = context->GetDensityIndependentPixelRatio();
    const float winW = static_cast<float>(dimensions.x), winH = static_cast<float>(dimensions.y);
    float w = 320.0f * scale;
    float h = 460.0f * scale;
    card->SetProperty("width", std::format("{}px", w));
    card->SetProperty("height", "auto");
    float left = (winW - w) / 2, top = (winH - h) / 2;
    if (!fullMode && !anchor.empty()) {
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
    const int avatarSize = fullMode ? 100 : 68;
    if (auto *el = profileDocument->GetElementById("profile-avatar-img")) el->SetAttribute("src", shell.imageSource(resolvedAvatarUrl(fullMode ? 256 : 128), {avatarSize, avatarSize}));
    std::string banner;
    if (hasProfile && profile.guildMemberProfile.hasValue()) banner = profile.guildMemberProfile.get().banner.valueOr();
    if (banner.empty() && hasProfile) banner = profile.user.get().banner.valueOr();
    if (auto *el = profileDocument->GetElementById("profile-banner-img")) {
        if (!banner.empty()) el->SetAttribute("src", shell.imageSource(Discord::Cdn::userBanner(currentUserId, banner, 640), {320, 105}));
        el->SetClass("visible", !banner.empty());
    }
    if (auto *el = profileDocument->GetElementById("profile-banner")) {
        auto color = resolvedAccentColor();
        el->SetProperty("background-color", color ? Platform::Color::rgb(color).css() : "#303136");
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
    if (auto *el = profileDocument->GetElementById("profile-message-btn")) el->SetProperty("display", currentUserId == inst->accountId() ? "none" : "block");
    if (auto *el = profileDocument->GetElementById("profile-note-section")) el->SetProperty("display", isBot() || currentUserId == inst->accountId() ? "none" : "block");
    renderPresenceAndActivities(); renderRoles(); renderDates();
}
void UserProfileController::renderPresenceAndActivities()
{
    auto *inst = clientInstance();
    if (!profileDocument || !inst) return;
    if (!inst->presences()) {
        if (fullMode && fullTab == "activity")
            if (auto *results = profileDocument->GetElementById("profile-full-results"))
                results->SetInnerRML("<div class=\"profile-empty\">Activity is unavailable.</div>");
        return;
    }
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
        if (auto *section = profileDocument->GetElementById("profile-activities-section")) section->SetProperty("display", fullMode || cards.empty() ? "none" : "block");
        if (fullMode && fullTab == "activity") {
            if (auto *results = profileDocument->GetElementById("profile-full-results"))
                results->SetInnerRML(rml.empty() ? "<div class=\"profile-empty\">No activity to show.</div>" : rml);
        }
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
void UserProfileController::selectFullTab(const std::string &tab)
{
    if (!fullMode || !profileDocument) return;
    fullTab = tab;
    for (const auto *name : {"activity", "friends", "guilds"}) {
        if (auto *el = profileDocument->GetElementById(std::string("profile-tab-") + name))
            el->SetClass("selected", fullTab == name);
    }
    renderFullContent();
    requestRender();
}

void UserProfileController::renderFullContent()
{
    if (!fullMode || !profileDocument) return;
    auto *results = profileDocument->GetElementById("profile-full-results");
    if (!results) return;
    if (fullTab == "activity") { renderPresenceAndActivities(); return; }
    if (!hasProfile) {
        results->SetInnerRML(profileFailed ? "<div class=\"profile-empty\">Could not load profile.</div>" : "<div class=\"profile-empty\">Loading profile...</div>");
        return;
    }
    std::string rml;
    if (fullTab == "friends") {
        if (!profile.mutualFriends.hasValue()) rml = "<div class=\"profile-empty\">Mutual friends are unavailable.</div>";
        else {
            for (const auto &user : profile.mutualFriends.get()) {
                const auto userId = user.id.get();
                if (!userId.isValid()) continue;
                const auto name = user.getDisplayName();
                const auto avatar = user.avatar.hasValue() && !user.avatar->empty()
                    ? Discord::Cdn::userAvatar(userId, user.avatar.get(), 64)
                    : Discord::Cdn::defaultUserAvatar(userId, 64);
                rml += "<button class=\"profile-mutual-row\" data-profile-user=\"" + userId.toString()
                    + "\" title=\"Open profile\" aria-label=\"Open profile for " + escape(name)
                    + "\"><span class=\"profile-mutual-image\"><img src=\"" + escape(shell.imageSource(avatar, {32, 32}))
                    + "\"/></span><span class=\"profile-mutual-name\">" + escape(name) + "</span></button>";
            }
            if (rml.empty()) rml = "<div class=\"profile-empty\">No mutual friends.</div>";
        }
    } else {
        if (!profile.mutualGuilds.hasValue()) rml = "<div class=\"profile-empty\">Mutual servers are unavailable.</div>";
        else {
            auto *inst = clientInstance();
            for (const auto &entry : profile.mutualGuilds.get()) {
                const auto guildId = entry.id.get();
                if (!guildId.isValid()) continue;
                const auto guild = inst ? inst->getGuild(guildId) : std::nullopt;
                const auto name = guild ? guild->name.valueOr(guildId.toString()) : guildId.toString();
                const auto icon = guild && guild->icon.hasValue() && !guild->icon->empty()
                    ? shell.imageSource(Discord::Cdn::guildIcon(guildId, guild->icon.get(), 64), {32, 32})
                    : std::string("icon:users");
                rml += "<button class=\"profile-mutual-row\" data-profile-guild=\"" + guildId.toString()
                    + "\" title=\"Open server\" aria-label=\"Open server " + escape(name)
                    + "\"><span class=\"profile-mutual-image profile-mutual-server\"><img src=\"" + escape(icon)
                    + "\"/></span><span class=\"profile-mutual-name\">" + escape(name) + "</span></button>";
            }
            if (rml.empty()) rml = "<div class=\"profile-empty\">No mutual servers.</div>";
        }
    }
    results->SetInnerRML(rml);
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
void UserProfileController::showProfile(Core::Snowflake userId, Core::Snowflake guildId, const Platform::Rect &anchor, bool full)
{
    ensureDocument();
    if (!profileDocument) return;
    if (visible) saveNoteIfDirty();
    else if (full) {
        if (auto *focused = profileDocument->GetContext()->GetFocusElement()) previousFocus = focused->GetObserverPtr();
        else previousFocus.reset();
    }
    currentUserId = userId; currentGuildId = guildId; hasProfile = false; noteDirty = false;
    fullMode = full; profileFailed = false; fullTab = "activity";
    profileDocument->SetClass("full-profile", fullMode);
    auto generation = ++requestGeneration;
    updateProfileData(); positionCard(anchor); selectFullTab(fullTab);
    profileDocument->Show(fullMode ? ::Rml::ModalFlag::Modal : ::Rml::ModalFlag::None);
    profileDocument->PullToFront(); profileDocument->SetClass("open", true);
    if (fullMode) if (auto *close = profileDocument->GetElementById("profile-close")) close->Focus();
    visible = true;
    activityTimer.start(std::chrono::seconds(1), true, [this] { if (visible) { renderPresenceAndActivities(); requestRender(); } });
    requestRender();
    if (auto *inst = clientInstance(); inst && inst->discord()) {
        Platform::Guard<UserProfileController> self(this);
        inst->discord()->fetchUserProfile(userId, guildId, [self, generation](const Core::Result<Discord::UserProfile> &result) {
            if (!self || !self->visible || self->requestGeneration != generation) return;
            if (result.success()) { self->profile = result.value.value(); self->hasProfile = true; self->updateProfileData(); }
            else self->profileFailed = true;
            self->renderFullContent(); self->requestRender();
        });
    }
}
void UserProfileController::hide()
{
    if (!profileDocument || !visible) return;
    saveNoteIfDirty(); ++requestGeneration; activityTimer.stop();
    profileDocument->SetClass("open", false); profileDocument->Hide(); visible = false;
    if (auto *focus = previousFocus.get()) focus->Focus();
    previousFocus.reset();
    requestRender();
}
} // namespace Styx::UI::Rml
