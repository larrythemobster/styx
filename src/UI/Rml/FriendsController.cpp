#include "FriendsController.hpp"

#include "RmlRuntime.hpp"
#include "Shell.hpp"

#include "Core/ClientInstance.hpp"
#include "Core/Presence/ActivityFormat.hpp"
#include "Core/Presence/PresenceBadge.hpp"
#include "Core/Presence/PresenceManager.hpp"
#include "Core/RelationshipManager.hpp"
#include "Core/UserManager.hpp"
#include "Discord/CdnUrls.hpp"
#include "Discord/Client.hpp"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

#include "Platform/Utf8.hpp"
#include <charconv>
#include <chrono>
#include <utility>
#include <algorithm>
#include <format>
#include <string_view>

namespace Styx {
namespace UI {
namespace Rml {

using Core::Snowflake;

namespace {
Snowflake parseId(const std::string &text)
{
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? Snowflake(value) : Snowflake::Invalid;
}
}

FriendsController::FriendsController(Shell &shell, ::Rml::Element *root)
    : Controller(shell, root)
{
    track(shell.locationChanged.connect([this](const Location &) {
        if (this->shell.isFriendsOpen())
            scheduleRebuild();
    }));
    track(shell.instanceChanged.connect([this](Core::ClientInstance *inst) {
        connectInstance(inst);
        if (this->shell.isFriendsOpen())
            scheduleRebuild();
    }));
    track(shell.accountReady.connect([this](Core::Snowflake) {
            scheduleRebuild();
    }));

    setupEventListeners();
    connectInstance(shell.currentInstance());

    if (shell.isFriendsOpen()) {
        render();
    }
}

FriendsController::~FriendsController() = default;

void FriendsController::connectInstance(Core::ClientInstance *instance)
{
    if (lastInstance.get() == instance)
        return;

    instanceConnections.clear();

    lastInstance = instance;
    ++requestGeneration;
    searchText.clear();
    addFriendInputText.clear();
    addFriendFeedback.clear();
    addFriendPending = false;
    addFriendSuccess = false;
    currentTab = Tab::Online;
    if (regionRoot)
        regionRoot->SetInnerRML("");
    if (!instance)
        return;

    auto remember = [this](Platform::Connection connection) {
        connection.setAutoDisconnect(true);
        instanceConnections.push_back(std::move(connection));
    };
    if (auto *rel = instance->relationships()) {
        remember(rel->relationshipChanged.connect([this](Snowflake) {
            if (this->shell.isFriendsOpen()) scheduleRebuild();
        }));
        remember(rel->relationshipsReset.connect([this]() {
            if (this->shell.isFriendsOpen()) scheduleRebuild();
        }));
    }
    if (auto *prs = instance->presences()) {
        remember(prs->presencesChanged.connect([this](const std::vector<Snowflake> &) {
            if (this->shell.isFriendsOpen()) scheduleRebuild();
        }));
    }
    remember(instance->membersUpdated.connect([this](Snowflake, const std::vector<Snowflake> &) {
        if (this->shell.isFriendsOpen()) scheduleRebuild();
    }));
}

void FriendsController::setupEventListeners()
{
    if (!regionRoot)
        return;

    listen("click", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();

        if (auto *tabEl = closest(target, "data-tab")) {
            std::string tabStr = attribute(tabEl, "data-tab");
            if (tabStr == std::string_view("online"))
                switchTab(Tab::Online);
            else if (tabStr == std::string_view("all"))
                switchTab(Tab::All);
            else if (tabStr == std::string_view("add"))
                switchTab(Tab::AddFriend);
            return;
        }

        if (auto *act = closest(target, "data-action")) {
            std::string action = attribute(act, "data-action");
            if (action == std::string_view("message")) {
                Snowflake uId = parseId(attribute(act, "data-user-id"));
                if (uId.isValid())
                    this->shell.openDmWithUser(this->shell.location().accountId, uId);
                return;
            }
            if (action == std::string_view("more")) {
                Snowflake uId = parseId(attribute(act, "data-user-id"));
                if (uId.isValid())
                    this->shell.showContextMenuForUser(uId, Snowflake::Invalid);
                return;
            }
            if (action == std::string_view("clear-friends-search")) {
                searchText.clear();
                if (auto *inp = dynamic_cast<::Rml::ElementFormControlInput *>(regionRoot->GetElementById("friends-search-input"))) {
                    inp->SetValue("");
                }
                rebuild();
                return;
            }
            if (action == std::string_view("send-friend-request")) {
                sendFriendRequest();
                return;
            }
        }

        if (auto *row = closest(target, "data-user-id")) {
            Snowflake uId = parseId(attribute(row, "data-user-id"));
            if (uId.isValid())
                this->shell.openDmWithUser(this->shell.location().accountId, uId);
            return;
        }
    });

    listen("mousedown", [this](::Rml::Event &event) {
        if (event.GetParameter<int>("button", 0) != 1) // Right click
            return;

        auto *target = event.GetTargetElement();
        if (auto *row = closest(target, "data-user-id")) {
            Snowflake uId = parseId(attribute(row, "data-user-id"));
            if (uId.isValid())
                this->shell.showContextMenuForUser(uId, Snowflake::Invalid);
        }
    });

    listen("change", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        if (auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(target)) {
            if (input->GetId() == "friends-search-input") {
                std::string val = input->GetValue();
                if (val != searchText) {
                    searchText = val;
                    scheduleRebuild();
                }
            } else if (input->GetId() == "add-friend-input") {
                addFriendInputText = input->GetValue();
                updateAddFriendControls();
            }
        }
    });

    listen("keydown", [this](::Rml::Event &event) {
        if (auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(event.GetTargetElement())) {
            if (input->GetId() == "add-friend-input") {
                const int key = event.GetParameter<int>("key_identifier", 0);
                if (key == ::Rml::Input::KI_RETURN || key == ::Rml::Input::KI_NUMPADENTER) {
                    event.StopPropagation();
                    addFriendInputText = input->GetValue();
                    sendFriendRequest();
                }
            }
        }
    }, nullptr, true);
}

void FriendsController::switchTab(Tab newTab)
{
    if (currentTab == newTab)
        return;
    currentTab = newTab;
    addFriendFeedback.clear();
    rebuild();
}

void FriendsController::sendFriendRequest()
{
    if (addFriendPending)
        return;

    std::string tag(Platform::trim(addFriendInputText));
    if (tag.empty())
        return;

    auto *inst = shell.currentInstance();
    if (!inst || !inst->discord()) {
        addFriendFeedback = "Client instance not available.";
        addFriendSuccess = false;
        scheduleRebuild();
        return;
    }

    addFriendPending = true;
    const std::uint64_t generation = ++requestGeneration;
    const Platform::Guard<FriendsController> self(this);
    inst->discord()->sendFriendRequest(tag, [self, generation, tag](const Core::Result<void> &res) {
        if (!self || self->requestGeneration != generation)
            return;
        self->addFriendPending = false;
        self->addFriendSuccess = res.success();
        if (res.success()) {
            self->addFriendFeedback = std::format("Friend request sent to {0}.", tag);
            self->addFriendInputText.clear();
        } else {
            self->addFriendFeedback = res.error;
        }
        self->scheduleRebuild();
    });
    scheduleRebuild();
}

void FriendsController::updateAddFriendControls()
{
    if (!regionRoot)
        return;
    if (auto *placeholder = regionRoot->GetElementById("add-friend-placeholder"))
        placeholder->SetProperty("display", addFriendInputText.empty() ? "block" : "none");
    if (auto *button = regionRoot->GetElementById("add-friend-btn"))
        button->SetClass("disabled", addFriendPending || Platform::trim(addFriendInputText).empty());
    requestRender();
}

void FriendsController::scheduleRebuild()
{
    rebuildTimer.start(std::chrono::milliseconds(0), false, [this] { rebuild(); });
}

void FriendsController::rebuild()
{
    if (!shell.isFriendsOpen())
        return;
    render();
    requestRender(200);
}

void FriendsController::render()
{
    if (!regionRoot)
        return;

    auto *inst = shell.currentInstance();
    int onlineCount = 0;
    int allCount = 0;

    if (inst && inst->relationships() && inst->presences()) {
        const auto &allRels = inst->relationships()->all();
        for (const auto &[id, rel] : allRels) {
            if (!rel.type.hasValue() || rel.type.get() != Discord::RelationshipType::FRIEND)
                continue;
            ++allCount;
            auto badge = inst->presences()->badge(id);
            if (badge.status != Discord::StatusType::OFFLINE)
                ++onlineCount;
        }
    }

    auto *existingSearch = regionRoot->GetElementById("friends-search-input");
    auto *listContainer = regionRoot->GetElementById("friends-list-container");
    auto *activeNowList = regionRoot->GetElementById("active-now-list");
    auto *headerEl = regionRoot->GetElementById("friends-header");

    if (currentTab != Tab::AddFriend && existingSearch && listContainer && activeNowList && headerEl) {
        std::string headerRml;
        renderHeader(headerRml, onlineCount, allCount);
        headerEl->SetInnerRML(headerRml.c_str());

        if (auto *placeholder = regionRoot->GetElementById("friends-search-placeholder")) {
            placeholder->SetProperty("display", searchText.empty() ? "inline" : "none");
        }
        if (auto *clear = regionRoot->GetElementById("friends-search-clear"))
            clear->SetProperty("display", searchText.empty() ? "none" : "block");

        std::string listRml;
        renderFriendsList(listRml, onlineCount, allCount);
        listContainer->SetInnerRML(listRml.c_str());

        std::string activeRml;
        renderActiveNow(activeRml);
        if (auto *activeNowEl = regionRoot->GetElementById("friends-active-now")) {
            activeNowEl->SetInnerRML(activeRml.c_str());
        }
        return;
    }

    std::string rml;
    rml += "<div id=\"friends-header\">";
    renderHeader(rml, onlineCount, allCount);
    rml += "</div>";

    rml += "<div id=\"friends-body\">";
    rml += "<div id=\"friends-main\">";

    if (currentTab == Tab::AddFriend) {
        renderAddFriend(rml);
    } else {
        const std::string clearBtn = std::format(
            "<img id=\"friends-search-clear\" class=\"friends-search-clear\" src=\"icon:x\" data-action=\"clear-friends-search\" style=\"{0}\"/>", searchText.empty() ? "display: none;" : std::string());

        rml += std::format(
            "<div class=\"friends-search-box\">"
            "<input type=\"text\" id=\"friends-search-input\" class=\"friends-search-input\" value=\"{0}\"/>"
            "<span id=\"friends-search-placeholder\" class=\"friends-search-placeholder\" style=\"{1}\">Search</span>"
            "<img class=\"friends-search-icon\" src=\"icon:search\"/>"
            "{2}"
            "</div>", Rml::escape(searchText), searchText.empty() ? std::string() : "display: none;", clearBtn);

        rml += "<div id=\"friends-list-container\">";
        renderFriendsList(rml, onlineCount, allCount);
        rml += "</div>"; // #friends-list-container
    }

    rml += "</div>"; // #friends-main

    rml += "<div id=\"friends-active-now\">";
    renderActiveNow(rml);
    rml += "</div>"; // #friends-active-now

    rml += "</div>"; // #friends-body

    regionRoot->SetInnerRML(rml.c_str());
}

void FriendsController::renderHeader(std::string &rml, int onlineCount, int allCount)
{
    const std::string onlineClass = (currentTab == Tab::Online) ? " selected" : std::string();
    const std::string allClass = (currentTab == Tab::All) ? " selected" : std::string();
    const std::string addClass = (currentTab == Tab::AddFriend) ? " selected" : std::string();

    rml += std::format(
        "<div class=\"friends-header-title\">"
        "<img class=\"friends-header-icon\" src=\"icon:user-plus\"/>"
        "<span class=\"friends-header-text\">Friends</span>"
        "</div>"
        "<div class=\"friends-header-divider\"></div>"
        "<div class=\"friends-tab{0}\" data-tab=\"online\">Online{1}</div>"
        "<div class=\"friends-tab{2}\" data-tab=\"all\">All{3}</div>"
        "<div class=\"friends-tab-add{4}\" data-tab=\"add\">Add Friend</div>", onlineClass, onlineCount > 0 ? std::format(" ({0})", onlineCount) : std::string(), allClass, allCount > 0 ? std::format(" ({0})", allCount) : std::string(), addClass);
}

void FriendsController::renderFriendsList(std::string &rml, int onlineCount, int allCount)
{
    auto *inst = shell.currentInstance();
    if (!inst || !inst->relationships() || !inst->presences())
        return;
    const auto &allRels = inst->relationships()->all();

    struct FriendItem {
        Snowflake userId;
        std::string displayName;
        std::string username;
        std::string avatarSrc;
        std::string statusClass;
        std::string subtext;
        bool isOnline = false;
    };

    std::vector<FriendItem> items;
    items.reserve(allRels.size());

    const std::string searchFilter = Platform::unicodeLower(Platform::trim(searchText));

    for (const auto &[userId, rel] : allRels) {
        if (!rel.type.hasValue() || rel.type.get() != Discord::RelationshipType::FRIEND)
            continue;
        auto badge = inst->presences()->badge(userId);
        const bool isOnline = (badge.status != Discord::StatusType::OFFLINE);

        if (currentTab == Tab::Online && !isOnline)
            continue;

        auto userOpt = inst->users()->getUser(userId);
        Discord::User user;
        if (userOpt)
            user = *userOpt;
        else if (rel.user.hasValue())
            user = rel.user.get();

        std::string displayName = inst->users()->getDisplayName(userId);
        if (displayName.empty())
            displayName = user.globalName.valueOr(user.username.valueOr("Friend"));

        std::string username = user.username.valueOr();

        if (!searchFilter.empty()) {
            if (Platform::unicodeLower(displayName).find(searchFilter) == std::string::npos && Platform::unicodeLower(username).find(searchFilter) == std::string::npos)
                continue;
        }

        std::string avatarUrl = user.avatar.hasValue()
                             ? Discord::Cdn::userAvatar(userId, user.avatar.get(), 64)
                             : Discord::Cdn::defaultUserAvatar(userId);
        std::string avatarSrc = shell.imageSource(avatarUrl, Platform::Size{36, 36});

        std::string statusClass;
        switch (badge.status) {
        case Discord::StatusType::ONLINE: statusClass = "status-online"; break;
        case Discord::StatusType::IDLE: statusClass = "status-idle"; break;
        case Discord::StatusType::DND: statusClass = "status-dnd"; break;
        case Discord::StatusType::OFFLINE:
        default: statusClass = "status-offline"; break;
        }
        if (badge.streaming)
            statusClass = "status-streaming";

        auto acts = inst->presences()->activities(userId);
        auto sorted = Core::ActivityFormat::sortAndFilter(acts);
        const auto *custom = Core::ActivityFormat::custom(acts);
        const auto *prim = Core::ActivityFormat::primary(sorted);

        std::string subtext;
        if (custom && !custom->stateText().empty()) {
            subtext = custom->stateText();
        } else if (prim) {
            subtext = Core::ActivityFormat::cardHeading(*prim);
            std::string sec = Core::ActivityFormat::secondaryText(*prim);
            if (!sec.empty())
                subtext += ": " + sec;
        } else {
            switch (badge.status) {
            case Discord::StatusType::ONLINE: subtext = "Online"; break;
            case Discord::StatusType::IDLE: subtext = "Idle"; break;
            case Discord::StatusType::DND: subtext = "Do Not Disturb"; break;
            case Discord::StatusType::OFFLINE:
            default: subtext = "Offline"; break;
            }
        }

        items.push_back({ userId, displayName, username, avatarSrc, statusClass, subtext, isOnline });
    }

    std::sort(items.begin(), items.end(), [](const FriendItem &a, const FriendItem &b) {
        return Platform::unicodeLower(a.displayName) < Platform::unicodeLower(b.displayName);
    });

    const std::string sectionTitle = (currentTab == Tab::Online)
                                     ? std::format("Online — {0}", items.size())
                                     : std::format("All friends — {0}", items.size());

    rml += std::format("<div class=\"friends-section-title\">{0}</div>", sectionTitle);
    rml += "<div id=\"friends-list\">";

    if (items.empty()) {
        std::string emptyTitle;
        std::string emptySubtitle;
        if (!searchText.empty()) {
            emptyTitle = "No friends found";
            emptySubtitle = std::format("No friends found matching \"{0}\".", Rml::escape(searchText));
        } else if (currentTab == Tab::Online) {
            emptyTitle = "No one's around to play with Wumpus.";
            emptySubtitle = "None of your friends are currently online.";
        } else {
            emptyTitle = "You don't have any friends yet.";
            emptySubtitle = "Add friends using their Discord Tag or username!";
        }

        rml += std::format(
            "<div class=\"friends-empty-state\">"
            "<img class=\"empty-state-icon\" src=\"icon:user-plus\"/>"
            "<div class=\"empty-state-title\">{0}</div>"
            "<div class=\"empty-state-subtitle\">{1}</div>"
            "</div>", emptyTitle, emptySubtitle);
    } else {
        for (const auto &item : items) {
            std::string usernamePart;
            if (!item.username.empty() && item.username != item.displayName) {
                usernamePart = std::format("<span class=\"friend-username\">@{0}</span>", Rml::escape(item.username));
            }

            rml += std::format(
                "<div class=\"friend-row\" data-user-id=\"{0}\">"
                "<div class=\"friend-avatar-wrap\">"
                "<div class=\"avatar-clip\">"
                "<img class=\"friend-avatar\" src=\"{1}\"/>"
                "</div>"
                "<div class=\"status-dot {2}\"></div>"
                "</div>"
                "<div class=\"friend-info\">"
                "<div class=\"friend-name-line\">"
                "<span class=\"friend-display-name\">{3}</span>"
                "{4}"
                "</div>"
                "<div class=\"friend-subtext\">{5}</div>"
                "</div>"
                "<div class=\"friend-actions\">"
                "<div class=\"friend-action-btn\" data-action=\"message\" data-user-id=\"{0}\" title=\"Message\">"
                "<img class=\"icon\" src=\"icon:message-circle\"/>"
                "</div>"
                "<div class=\"friend-action-btn friend-action-more\" data-action=\"more\" data-user-id=\"{0}\" title=\"More\">"
                "<img class=\"icon\" src=\"icon:ellipsis\"/>"
                "</div>"
                "</div>"
                "</div>", item.userId.toString(), escape(item.avatarSrc), item.statusClass, escape(item.displayName), usernamePart, escape(item.subtext));
        }
    }

    rml += "</div>"; // #friends-list
}

void FriendsController::renderActiveNow(std::string &rml)
{
    auto *inst = shell.currentInstance();
    rml += "<div class=\"active-now-title\">Active Now</div>";
    rml += "<div id=\"active-now-list\">";

    struct CardData {
        Snowflake userId;
        std::string displayName;
        std::string avatarSrc;
        std::string statusClass;
        std::string typeIcon;
        std::string activityType;
        std::string heading;
        std::string details;
        std::string state;
        std::string artworkSrc;
        bool streaming;
    };

    std::vector<CardData> cards;

    if (inst && inst->relationships() && inst->presences()) {
        const auto &allRels = inst->relationships()->all();
        for (const auto &[userId, rel] : allRels) {
            if (!rel.type.hasValue() || rel.type.get() != Discord::RelationshipType::FRIEND)
                continue;
            auto acts = inst->presences()->activities(userId);
            auto sorted = Core::ActivityFormat::sortAndFilter(acts);
            auto profileCards = Core::ActivityFormat::profileCardActivities(sorted);
            if (profileCards.empty())
                continue;

            auto userOpt = inst->users()->getUser(userId);
            Discord::User user;
            if (userOpt)
                user = *userOpt;
            else if (rel.user.hasValue())
                user = rel.user.get();

            std::string displayName = inst->users()->getDisplayName(userId);
            if (displayName.empty())
                displayName = user.globalName.valueOr(user.username.valueOr("Friend"));

            std::string avatarUrl = user.avatar.hasValue()
                                 ? Discord::Cdn::userAvatar(userId, user.avatar.get(), 32)
                                 : Discord::Cdn::defaultUserAvatar(userId);
            std::string avatarSrc = shell.imageSource(avatarUrl, Platform::Size{32, 32});

            auto badge = inst->presences()->badge(userId);
            std::string statusClass;
            switch (badge.status) {
            case Discord::StatusType::ONLINE: statusClass = "status-online"; break;
            case Discord::StatusType::IDLE: statusClass = "status-idle"; break;
            case Discord::StatusType::DND: statusClass = "status-dnd"; break;
            case Discord::StatusType::OFFLINE:
            default: statusClass = "status-offline"; break;
            }
            if (badge.streaming)
                statusClass = "status-streaming";

            for (const auto &act : profileCards) {
                std::string typeIcon = "icon:gamepad-2";
                std::string typeText = "Playing";

                if (act.kind() == Discord::ActivityType::LISTENING) {
                    typeIcon = "icon:music";
                    typeText = std::format("Listening to {0}", act.name.valueOr("music"));
                } else if (act.kind() == Discord::ActivityType::STREAMING) {
                    typeIcon = "icon:monitor";
                    typeText = "Streaming";
                }

                std::string heading = Core::ActivityFormat::cardHeading(act);
                if (heading.empty())
                    heading = act.name.valueOr();

                std::string details = act.detailsText();
                std::string state = act.stateText();
                if (act.kind() == Discord::ActivityType::LISTENING && !details.empty()) {
                    heading = std::move(details);
                    details = std::move(state);
                    state = act.assets.hasValue() ? act.assets->largeText.valueOr() : std::string();
                    if (state == heading || state == details)
                        state.clear();
                }

                const bool streaming = act.kind() == Discord::ActivityType::STREAMING;
                const Platform::Size artSize = streaming ? Platform::Size{320, 180} : Platform::Size{64, 64};
                std::string artworkSrc;
                if (act.assets.hasValue() && act.assets->largeImage.hasValue()) {
                    const std::string artwork = Discord::Cdn::activityAsset(
                        act.applicationId.valueOr(Snowflake::Invalid), *act.assets->largeImage, streaming ? 512 : 64);
                    if (!artwork.empty())
                        artworkSrc = shell.imageSource(artwork, artSize);
                }
                cards.push_back({ userId, displayName, avatarSrc, statusClass, typeIcon, typeText,
                    heading, details, state, artworkSrc, streaming });
            }
        }
    }

    if (cards.empty()) {
        rml += 
            "<div class=\"active-now-empty\">"
            "<div class=\"active-now-empty-heading\">It's quiet for now...</div>"
            "<div class=\"active-now-empty-text\">When friends are playing games, hanging out in voice, or listening to music, they'll show up here.</div>"
            "</div>";
    } else {
        for (const auto &card : cards) {
            std::string detailsPart;
            if (!card.details.empty()) {
                detailsPart = std::format("<div class=\"active-card-details\">{0}</div>", Rml::escape(card.details));
            }
            std::string statePart;
            if (!card.state.empty()) {
                statePart = std::format("<div class=\"active-card-state\">{0}</div>", Rml::escape(card.state));
            }
            const std::string artwork = card.artworkSrc.empty() ? std::string() : std::format(
                "<div class=\"active-card-art\"><img src=\"{0}\"/></div>", escape(card.artworkSrc));

            rml += std::format("<div class=\"active-card{0}\" data-user-id=\"{1}\">", card.streaming ? " streaming" : std::string(), card.userId.toString());
            rml += std::format(
                "<div class=\"active-card-top\">"
                "<div class=\"active-card-avatar-wrap\">"
                "<div class=\"active-card-avatar-clip\">"
                "<img class=\"active-card-avatar\" src=\"{0}\"/>"
                "</div>"
                "<div class=\"status-dot {1}\"></div>"
                "</div>"
                "<div class=\"active-card-user-info\">"
                "<span class=\"active-card-user\">{2}</span>"
                "<span class=\"active-card-activity-type\">{3}</span>"
                "</div>"
                "<img class=\"active-card-type-icon\" src=\"{4}\"/>"
                "</div>"
                "<div class=\"active-card-body\">"
                "{8}"
                "<div class=\"active-card-text\">"
                "<div class=\"active-card-heading\">{5}</div>"
                "{6}"
                "{7}"
                "</div>"
                "</div>"
                "</div>", escape(card.avatarSrc), card.statusClass, escape(card.displayName), escape(card.activityType), card.typeIcon, escape(card.heading), detailsPart, statePart, artwork);
        }
    }

    rml += "</div>"; // #active-now-list
}

void FriendsController::renderAddFriend(std::string &rml)
{
    const bool canSend = !Platform::trim(addFriendInputText).empty() && !addFriendPending;
    const std::string btnClass = canSend ? std::string() : " disabled";

    std::string feedbackHtml;
    if (!addFriendFeedback.empty()) {
        const std::string feedbackClass = addFriendSuccess ? "feedback-success" : "feedback-error";
        feedbackHtml = std::format("<div class=\"add-friend-feedback {0}\">{1}</div>", feedbackClass, Rml::escape(addFriendFeedback));
    }

    rml += std::format(
        "<div class=\"add-friend-container\">"
        "<div class=\"add-friend-header\">ADD FRIEND</div>"
        "<div class=\"add-friend-subtitle\">You can add friends with their Discord Tag or username.</div>"
        "<div class=\"add-friend-input-wrap\">"
        "<input type=\"text\" id=\"add-friend-input\" class=\"add-friend-input\" value=\"{0}\"{5}/>"
        "<span id=\"add-friend-placeholder\" class=\"add-friend-placeholder\" style=\"{1}\">You can add friends with their Discord Tag or username</span>"
        "<button id=\"add-friend-btn\" class=\"add-friend-btn{2}\" data-action=\"send-friend-request\">{3}</button>"
        "</div>"
        "{4}"
        "</div>", Rml::escape(addFriendInputText), addFriendInputText.empty() ? std::string() : "display: none;", btnClass, addFriendPending ? "Sending..." : "Send Friend Request", feedbackHtml, addFriendPending ? " disabled=\"disabled\"" : std::string());
}

} // namespace Rml
} // namespace UI
} // namespace Styx
