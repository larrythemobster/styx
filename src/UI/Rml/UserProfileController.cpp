#include "UserProfileController.hpp"

#include "Shell.hpp"
#include "RmlView.hpp"
#include "RmlRuntime.hpp"

#include "Core/ClientInstance.hpp"
#include "Core/ImageManager.hpp"
#include "Core/MessageManager.hpp"
#include "Core/Presence/ActivityFormat.hpp"
#include "Core/Presence/PresenceBadge.hpp"
#include "Core/Presence/PresenceManager.hpp"
#include "Core/UserManager.hpp"
#include "Discord/CdnUrls.hpp"
#include "Discord/Client.hpp"

#include <QColor>
#include <QDateTime>
#include <QLocale>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

namespace Styx {
namespace UI {
namespace Rml {

namespace {

QString formatDate(const QDateTime &dt)
{
    if (!dt.isValid())
        return QStringLiteral("—");
    return QLocale::system().toString(dt.toLocalTime().date(), QStringLiteral("MMM d, yyyy"));
}

} // namespace

UserProfileController::UserProfileController(Shell &shell, QObject *parent)
    : Controller(shell, nullptr, parent)
{
    activityTimer.setInterval(1000);
    connect(&activityTimer, &QTimer::timeout, this, [this]() {
        if (visible)
            renderPresenceAndActivities();
    });
}

UserProfileController::~UserProfileController()
{
    if (profileDocument) {
        profileDocument->Close();
        profileDocument = nullptr;
    }
}

Core::ClientInstance *UserProfileController::clientInstance() const
{
    return shell.currentInstance();
}

std::optional<Discord::User> UserProfileController::cachedUser() const
{
    auto *inst = clientInstance();
    if (!inst || !currentUserId.isValid())
        return std::nullopt;
    return inst->users()->getUser(currentUserId);
}

std::optional<Discord::Member> UserProfileController::cachedMember() const
{
    auto *inst = clientInstance();
    if (!inst || !currentGuildId.isValid() || !currentUserId.isValid())
        return std::nullopt;
    return inst->users()->getMember(currentGuildId, currentUserId);
}

QString UserProfileController::tagHandle() const
{
    if (auto u = cachedUser(); u && u->username.hasValue())
        return QStringLiteral("@") + u->username.get();
    if (hasProfile && profile.user.get().username.hasValue())
        return QStringLiteral("@") + profile.user.get().username.get();
    return QStringLiteral("@unknown");
}

QString UserProfileController::resolvedDisplayName() const
{
    auto *inst = clientInstance();
    if (inst && currentGuildId.isValid()) {
        QString nick = inst->users()->getNickname(currentUserId, currentGuildId);
        if (!nick.isEmpty())
            return nick;
    }
    if (auto u = cachedUser())
        return u->getDisplayName();
    if (hasProfile)
        return profile.user.get().getDisplayName();
    return QStringLiteral("Unknown User");
}

QUrl UserProfileController::resolvedAvatarUrl(int size) const
{
    QString memberHash;
    if (auto m = cachedMember())
        memberHash = m->avatar.valueOr();

    QString userHash;
    if (auto u = cachedUser())
        userHash = u->avatar.valueOr();
    if (userHash.isEmpty() && hasProfile)
        userHash = profile.user.get().avatar.valueOr();

    return Discord::Cdn::effectiveAvatar(currentGuildId, currentUserId, memberHash, userHash, size);
}

int UserProfileController::resolvedAccentColor() const
{
    if (hasProfile && profile.guildMemberProfile.hasValue() && profile.guildMemberProfile.get().accentColor.hasValue())
        return profile.guildMemberProfile.get().accentColor.get();
    if (hasProfile && profile.userProfile.hasValue() && profile.userProfile.get().accentColor.hasValue())
        return profile.userProfile.get().accentColor.get();
    if (hasProfile && profile.user.get().accentColor.hasValue())
        return profile.user.get().accentColor.get();
    if (auto u = cachedUser(); u && u->accentColor.hasValue())
        return u->accentColor.get();
    return 0;
}

bool UserProfileController::isBot() const
{
    if (auto u = cachedUser(); u && u->bot.hasValue() && u->bot.get())
        return true;
    return hasProfile && profile.user.get().bot.hasValue() && profile.user.get().bot.get();
}

void UserProfileController::ensureDocument()
{
    if (profileDocument || !shell.view() || !shell.view()->context())
        return;

    profileDocument = shell.view()->context()->LoadDocument(":/ui/user_profile.rml");
    if (!profileDocument)
        return;

    regionRoot = profileDocument;

    listen("click", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        if (!target)
            return;

        if (target->GetId() == "profile-backdrop" || target->GetId() == "user-profile-root") {
            event.StopPropagation();
            hide();
            return;
        }

        if (target->GetId() == "profile-message-btn" || closest(target, "profile-message-btn")) {
            event.StopPropagation();
            Core::Snowflake accId = shell.location().accountId;
            Core::Snowflake targetUser = currentUserId;
            hide();
            shell.openDmWithUser(accId, targetUser);
            return;
        }
    }, profileDocument);

    listen("keydown", [this](::Rml::Event &event) {
        const auto key = static_cast<::Rml::Input::KeyIdentifier>(
            event.GetParameter<int>("key_identifier", 0));
        if (key == ::Rml::Input::KI_ESCAPE) {
            event.StopPropagation();
            hide();
        }
    }, profileDocument);

    listen("change", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        if (target && target->GetId() == "profile-note-input") {
            noteDirty = true;
        }
    }, profileDocument);
}

void UserProfileController::positionCard(const QRectF &anchorRect)
{
    if (!profileDocument)
        return;

    auto *card = profileDocument->GetElementById("profile-card");
    if (!card)
        return;

    const float popoutW = 320.0f;
    const float popoutH = 460.0f;
    float winW = shell.view() ? static_cast<float>(shell.view()->width()) : 1280.0f;
    float winH = shell.view() ? static_cast<float>(shell.view()->height()) : 720.0f;

    float left = (winW - popoutW) / 2.0f;
    float top = (winH - popoutH) / 2.0f;

    if (anchorRect.isValid() && anchorRect.width() > 0 && anchorRect.height() > 0) {
        // Place adjacent to anchor
        if (anchorRect.center().x() > winW / 2.0f) {
            // Anchor on the right -> popout to the left
            left = static_cast<float>(anchorRect.left()) - popoutW - 8.0f;
        } else {
            // Anchor on the left -> popout to the right
            left = static_cast<float>(anchorRect.right()) + 8.0f;
        }

        top = static_cast<float>(anchorRect.top());
        // Clamp bounds
        if (left < 10.0f)
            left = 10.0f;
        if (left + popoutW > winW - 10.0f)
            left = winW - popoutW - 10.0f;
        if (top < 10.0f)
            top = 10.0f;
        if (top + popoutH > winH - 10.0f)
            top = winH - popoutH - 10.0f;
    }

    card->SetProperty("left", QStringLiteral("%1px").arg(left).toStdString());
    card->SetProperty("top", QStringLiteral("%1px").arg(top).toStdString());
}

void UserProfileController::updateProfileData()
{
    if (!profileDocument)
        return;

    auto *inst = clientInstance();
    if (!inst)
        return;

    // Names
    if (auto *nameEl = profileDocument->GetElementById("profile-display-name"))
        nameEl->SetInnerRML(resolvedDisplayName().toHtmlEscaped().toStdString());

    if (auto *handleEl = profileDocument->GetElementById("profile-handle"))
        handleEl->SetInnerRML(tagHandle().toHtmlEscaped().toStdString());

    if (auto *botBadge = profileDocument->GetElementById("profile-bot-badge"))
        botBadge->SetClass("visible", isBot());

    // Pronouns
    QString pronouns;
    if (hasProfile && profile.guildMemberProfile.hasValue() && !profile.guildMemberProfile.get().pronouns.get().isEmpty())
        pronouns = profile.guildMemberProfile.get().pronouns.get();
    else if (hasProfile && !profile.userProfile.get().pronouns.get().isEmpty())
        pronouns = profile.userProfile.get().pronouns.get();

    if (auto *pronounsEl = profileDocument->GetElementById("profile-pronouns")) {
        pronounsEl->SetInnerRML(pronouns.toHtmlEscaped().toStdString());
        pronounsEl->SetClass("visible", !pronouns.isEmpty());
    }

    // Avatar
    if (auto *avatarImg = profileDocument->GetElementById("profile-avatar-img")) {
        QUrl avatarUrl = resolvedAvatarUrl(128);
        QString src = shell.imageSource(avatarUrl, QSize(68, 68));
        avatarImg->SetAttribute("src", src.toStdString());
    }

    // Banner
    QString bannerHash;
    if (hasProfile && profile.guildMemberProfile.hasValue() && profile.guildMemberProfile.get().banner.hasValue())
        bannerHash = profile.guildMemberProfile.get().banner.get();
    if (bannerHash.isEmpty() && hasProfile && profile.user.get().banner.hasValue())
        bannerHash = profile.user.get().banner.get();

    if (auto *bannerImg = profileDocument->GetElementById("profile-banner-img")) {
        if (!bannerHash.isEmpty()) {
            QUrl bannerUrl = Discord::Cdn::userBanner(currentUserId, bannerHash, 640);
            QString src = shell.imageSource(bannerUrl, QSize(320, 105));
            bannerImg->SetAttribute("src", src.toStdString());
            bannerImg->SetClass("visible", true);
        } else {
            bannerImg->SetClass("visible", false);
        }
    }

    if (auto *bannerEl = profileDocument->GetElementById("profile-banner")) {
        int colorInt = resolvedAccentColor();
        if (colorInt != 0) {
            QColor color = QColor::fromRgb(colorInt);
            bannerEl->SetProperty("background-color", color.name().toStdString());
        } else {
            bannerEl->SetProperty("background-color", "#5865f2");
        }
    }

    // Bio / About Me
    QString bio;
    if (hasProfile && profile.guildMemberProfile.hasValue() && profile.guildMemberProfile.get().bio.hasValue())
        bio = profile.guildMemberProfile.get().bio.get();
    if (bio.isEmpty() && hasProfile && profile.userProfile.hasValue() && profile.userProfile.get().bio.hasValue())
        bio = profile.userProfile.get().bio.get();

    if (auto *bioSec = profileDocument->GetElementById("profile-bio-section")) {
        if (!bio.isEmpty()) {
            bioSec->SetProperty("display", "block");
            if (auto *bioEl = profileDocument->GetElementById("profile-bio")) {
                QString bioHtml = inst->messages()->profileBioHtml(bio);
                bioEl->SetInnerRML(bioHtml.toStdString());
            }
        } else {
            bioSec->SetProperty("display", "none");
        }
    }

    // Note
    if (auto *noteInput = profileDocument->GetElementById("profile-note-input")) {
        if (auto *formInput = dynamic_cast<::Rml::ElementFormControlInput *>(noteInput)) {
            QString note = inst->users()->getCachedNote(currentUserId).value_or(QString());
            initialNote = note;
            formInput->SetValue(note.toStdString());
            noteDirty = false;
        }
    }

    renderPresenceAndActivities();
    renderRoles();
    renderDates();
}

void UserProfileController::renderPresenceAndActivities()
{
    if (!profileDocument)
        return;

    auto *inst = clientInstance();
    if (!inst)
        return;

    auto *presences = inst->presences();
    Core::PresenceBadge badge = presences->badge(currentUserId, currentGuildId);

    // Status dot
    if (auto *dotEl = profileDocument->GetElementById("profile-status-dot")) {
        dotEl->SetClass("online", badge.status == Discord::StatusType::ONLINE && !badge.streaming);
        dotEl->SetClass("idle", badge.status == Discord::StatusType::IDLE && !badge.streaming);
        dotEl->SetClass("dnd", badge.status == Discord::StatusType::DND && !badge.streaming);
        dotEl->SetClass("offline", badge.status == Discord::StatusType::OFFLINE && !badge.streaming);
        dotEl->SetClass("streaming", badge.streaming);
    }

    const QList<Discord::Activity> allActivities = presences->activities(currentUserId, currentGuildId);

    // Custom status
    const Discord::Activity *custom = Core::ActivityFormat::custom(allActivities);
    if (auto *customRow = profileDocument->GetElementById("profile-custom-status")) {
        if (custom && !custom->stateText().isEmpty()) {
            customRow->SetClass("visible", true);
            if (auto *textEl = profileDocument->GetElementById("profile-status-text"))
                textEl->SetInnerRML(custom->stateText().toHtmlEscaped().toStdString());
            if (auto *emojiEl = profileDocument->GetElementById("profile-status-emoji")) {
                const Discord::ActivityEmoji emoji = custom->emoji.hasValue() ? custom->emoji.get() : Discord::ActivityEmoji();
                if (!emoji.isUnicode() && emoji.id.hasValue()) {
                    QUrl emojiUrl = emoji.stillImageUrl(32);
                    emojiEl->SetAttribute("src", shell.imageSource(emojiUrl, QSize(18, 18)).toStdString());
                    emojiEl->SetProperty("display", "inline-block");
                } else if (!emoji.nameText().isEmpty()) {
                    emojiEl->SetAttribute("src", Runtime::emojiSource(emoji.nameText()).toStdString());
                    emojiEl->SetProperty("display", "inline-block");
                } else {
                    emojiEl->SetProperty("display", "none");
                }
            }
        } else {
            customRow->SetClass("visible", false);
        }
    }

    // Activities
    if (auto *activitiesContainer = profileDocument->GetElementById("profile-activities")) {
        QString activitiesRml;
        auto profileActivities = Core::ActivityFormat::profileCardActivities(allActivities);
        for (const Discord::Activity &act : profileActivities) {
            QString heading = Core::ActivityFormat::cardHeading(act);
            bool named = Core::ActivityFormat::headingHasName(act);
            QString title = named ? act.detailsText() : act.nameText();
            QString details = named ? QString() : act.detailsText();
            QString state = Core::ActivityFormat::partyText(act);
            auto timer = Core::ActivityFormat::timerFor(act);
            QString timerText = Core::ActivityFormat::timerText(timer, QDateTime::currentMSecsSinceEpoch());

            QUrl largeImgUrl;
            if (act.assets.hasValue() && act.assets.get().largeImage.hasValue()) {
                QString asset = act.assets.get().largeImage.get();
                if (act.applicationId.hasValue())
                    largeImgUrl = Discord::Cdn::activityAsset(act.applicationId.get(), asset, 96);
            }
            if (!largeImgUrl.isValid() && act.applicationId.hasValue()) {
                largeImgUrl = Discord::Cdn::applicationIcon(act.applicationId.get(),
                    inst->discord()->applicationIconHash(act.applicationId.get()), 96);
            }

            activitiesRml += QStringLiteral("<div class=\"activity-card\">");
            activitiesRml += QStringLiteral("<div class=\"activity-header\">%1</div>").arg(heading.toHtmlEscaped());
            activitiesRml += QStringLiteral("<div class=\"activity-content\">");
            if (largeImgUrl.isValid()) {
                QString imgSource = shell.imageSource(largeImgUrl, QSize(48, 48));
                activitiesRml += QStringLiteral("<div class=\"activity-image-wrap\"><img class=\"activity-large-img\" src=\"%1\"/></div>").arg(imgSource);
            }
            activitiesRml += QStringLiteral("<div class=\"activity-text-col\">");
            if (!title.isEmpty())
                activitiesRml += QStringLiteral("<div class=\"activity-name\">%1</div>").arg(title.toHtmlEscaped());
            if (!details.isEmpty())
                activitiesRml += QStringLiteral("<div class=\"activity-details\">%1</div>").arg(details.toHtmlEscaped());
            if (!state.isEmpty())
                activitiesRml += QStringLiteral("<div class=\"activity-state\">%1</div>").arg(state.toHtmlEscaped());
            if (!timerText.isEmpty())
                activitiesRml += QStringLiteral("<div class=\"activity-timer\">%1</div>").arg(timerText.toHtmlEscaped());
            activitiesRml += QStringLiteral("</div></div></div>");
        }

        activitiesContainer->SetInnerRML(activitiesRml.toStdString());
        if (auto *actSec = profileDocument->GetElementById("profile-activities-section"))
            actSec->SetProperty("display", profileActivities.isEmpty() ? "none" : "block");
    }
}

void UserProfileController::renderRoles()
{
    if (!profileDocument)
        return;

    auto *inst = clientInstance();
    if (!inst || !currentGuildId.isValid()) {
        if (auto *rolesSec = profileDocument->GetElementById("profile-roles-section"))
            rolesSec->SetProperty("display", "none");
        return;
    }

    QList<Discord::Role> memberRoles = inst->getMemberRolesSorted(currentGuildId, currentUserId);
    if (auto *rolesContainer = profileDocument->GetElementById("profile-roles")) {
        QString rolesRml;
        for (const auto &role : memberRoles) {
            QColor color = role.hasColor() ? role.getColor() : QColor(148, 155, 164);
            rolesRml += QStringLiteral("<div class=\"role-pill\">"
                                       "<div class=\"role-dot\" style=\"background-color: %1;\"></div>"
                                       "<span>%2</span>"
                                       "</div>")
                            .arg(color.name(), role.name.get().toHtmlEscaped());
        }
        rolesContainer->SetInnerRML(rolesRml.toStdString());
    }

    if (auto *rolesSec = profileDocument->GetElementById("profile-roles-section"))
        rolesSec->SetProperty("display", memberRoles.isEmpty() ? "none" : "block");
}

void UserProfileController::renderDates()
{
    if (!profileDocument)
        return;

    auto *datesContainer = profileDocument->GetElementById("profile-dates");
    if (!datesContainer)
        return;

    QString datesRml;
    datesRml += QStringLiteral("<div class=\"dates-row\">Discord: <b>%1</b></div>").arg(formatDate(currentUserId.toDateTime()));

    if (currentGuildId.isValid()) {
        QDateTime joinedAt;
        if (auto m = cachedMember(); m && m->joinedAt.hasValue())
            joinedAt = m->joinedAt.get();
        else if (hasProfile && profile.guildMember.hasValue() && profile.guildMember.get().joinedAt.hasValue())
            joinedAt = profile.guildMember.get().joinedAt.get();

        if (joinedAt.isValid())
            datesRml += QStringLiteral("<div class=\"dates-row\">Server: <b>%1</b></div>").arg(formatDate(joinedAt));
    }

    datesContainer->SetInnerRML(datesRml.toStdString());
}

void UserProfileController::saveNoteIfDirty()
{
    if (!noteDirty || !currentUserId.isValid() || isBot())
        return;

    auto *inst = clientInstance();
    if (!inst)
        return;

    if (auto *noteInput = profileDocument ? profileDocument->GetElementById("profile-note-input") : nullptr) {
        if (auto *formInput = dynamic_cast<::Rml::ElementFormControlInput *>(noteInput)) {
            QString newNote = QString::fromStdString(formInput->GetValue());
            if (newNote != initialNote) {
                inst->discord()->setUserNote(currentUserId, newNote);
                initialNote = newNote;
            }
        }
    }
    noteDirty = false;
}

void UserProfileController::showProfile(Core::Snowflake userId, Core::Snowflake guildId, const QRectF &anchorRect)
{
    ensureDocument();
    if (!profileDocument)
        return;

    currentUserId = userId;
    currentGuildId = guildId;
    hasProfile = false;

    updateProfileData();
    positionCard(anchorRect);

    profileDocument->Show();
    profileDocument->PullToFront();
    profileDocument->SetClass("open", true);
    visible = true;
    activityTimer.start();
    requestRender();

    // Fetch full profile asynchronously
    if (auto *inst = clientInstance()) {
        QPointer<UserProfileController> self(this);
        inst->discord()->fetchUserProfile(userId, guildId, [self, userId](const Core::Result<Discord::UserProfile> &result) {
            if (!self || !self->visible || self->currentUserId != userId)
                return;
            if (result.success()) {
                self->profile = result.value.value();
                self->hasProfile = true;
                self->updateProfileData();
                self->requestRender();
            }
        });
    }
}

void UserProfileController::hide()
{
    if (!profileDocument || !visible)
        return;

    saveNoteIfDirty();
    activityTimer.stop();
    profileDocument->SetClass("open", false);
    profileDocument->Hide();
    visible = false;
    requestRender();
}

} // namespace Rml
} // namespace UI
} // namespace Styx
