#include "ManageGroupDmController.hpp"
#include "GroupDmMembersForm.hpp"

#include "Core/ClientInstance.hpp"
#include "Core/RelationshipManager.hpp"
#include "Core/UserManager.hpp"
#include "Discord/CdnUrls.hpp"
#include "Platform/Utf8.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace Styx::UI::Rml {
namespace {

bool validId(Core::Snowflake id)
{
    return id.isValid() && id != Core::Snowflake(0);
}

std::optional<Discord::Channel> availableGroup(Core::ClientInstance *instance, Core::Snowflake channelId)
{
    if (!instance || !validId(instance->accountId()) || !validId(channelId))
        return std::nullopt;
    auto channel = instance->getChannel(channelId);
    if (!channel || channel->type.get() != Discord::ChannelType::GROUP_DM
        || (channel->guildId.hasValue() && validId(channel->guildId.get())))
        return std::nullopt;
    return channel;
}

} // namespace

ManageGroupDmController::ManageGroupDmController(Shell &shell)
    : Controller(shell, nullptr)
    , form(std::make_unique<GroupDmMembersForm>([this] { requestRender(); }))
{
    track(this->shell.instanceChanged.connect([this](Core::ClientInstance *instance) {
        if (visible && (!instance || instance != originInstance.get()))
            close();
    }));
    track(this->shell.locationChanged.connect([this](const UI::Location &location) {
        if (visible && location != originLocation)
            close();
    }));
    track(form->addRequested.connect([this](Core::Snowflake userId) { submitRequest(userId, Request::Add); }));
    track(form->removeRequested.connect([this](Core::Snowflake userId) { submitRequest(userId, Request::Remove); }));
    track(form->closeRequested.connect([this] { close(); }));
}

ManageGroupDmController::~ManageGroupDmController()
{
    visible = false;
    instanceConnections.clear();
    form->setDocument(nullptr);
    if (document)
        document->Close();
}

bool ManageGroupDmController::canOpen(Core::ClientInstance *instance, Core::Snowflake channelId)
{
    return availableGroup(instance, channelId).has_value();
}

void ManageGroupDmController::open(Core::Snowflake channelId)
{
    auto *instance = shell.currentInstance();
    const auto location = shell.location();
    if (!instance || location.accountId != instance->accountId()
        || !location.isHome() || location.channelId != channelId)
        return;
    const auto channel = availableGroup(instance, channelId);
    if (!channel)
        return;

    ensureDocument();
    if (!document)
        return;
    if (visible && currentChannelId == channelId && originInstance.get() == instance && originLocation == location) {
        document->PullToFront();
        form->focusSearch();
        return;
    }
    if (visible)
        close();

    auto snapshot = buildSnapshot(*instance, *channel);

    ++generation;
    pending = false;
    originInstance = Platform::Guard<Core::ClientInstance>(instance);
    originLocation = location;
    currentChannelId = channelId;

    auto remember = [this](Platform::Connection connection) {
        connection.setAutoDisconnect(true);
        instanceConnections.push_back(std::move(connection));
    };
    remember(instance->relationships()->relationshipChanged.connect([this](Core::Snowflake) { refresh(); }));
    remember(instance->relationships()->relationshipsReset.connect([this] { refresh(); }));
    remember(instance->membersUpdated.connect([this](Core::Snowflake, const std::vector<Core::Snowflake> &) { refresh(); }));
    remember(instance->presences()->presencesChanged.connect([this](const std::vector<Core::Snowflake> &) { refresh(); }));
    remember(instance->channelUpdated.connect([this](const Discord::ChannelUpdate &update) {
        if (update.channel->id.get() == currentChannelId)
            refresh();
    }));
    remember(instance->channelDeleted.connect([this](const Discord::ChannelDelete &ev) {
        if (ev.id.get() == currentChannelId)
            close();
    }));

    form->reset(std::move(snapshot));
    visible = true;
    document->Show(::Rml::ModalFlag::None);
    document->SetProperty("display", "flex");
    document->PullToFront();
    form->focusSearch();
    requestRender();
}

void ManageGroupDmController::close()
{
    if (!visible)
        return;
    visible = false;
    pending = false;
    ++generation;
    instanceConnections.clear();
    originInstance.reset();
    originLocation = {};
    currentChannelId = Core::Snowflake::Invalid;
    form->setPending(false);
    if (document)
        document->Hide();
    requestRender();
}

void ManageGroupDmController::ensureDocument()
{
    if (document || !shell.context())
        return;
    document = shell.context()->LoadDocument("ui/group_dm_members.rml");
    if (document) {
        regionRoot = document;
        form->setDocument(document);
    }
}

void ManageGroupDmController::submitRequest(Core::Snowflake userId, Request request)
{
    if (!visible || pending)
        return;
    auto *origin = originInstance.get();
    if (!origin || shell.currentInstance() != origin || shell.location() != originLocation) {
        close();
        return;
    }
    if (!validId(userId) || userId == origin->accountId())
        return;
    const auto channel = availableGroup(origin, currentChannelId);
    if (!channel) {
        close();
        return;
    }
    auto snapshot = buildSnapshot(*origin, *channel);
    const bool isMember = std::any_of(snapshot.members.begin(), snapshot.members.end(),
        [userId](const GroupDmMemberPerson &member) { return member.userId == userId; });
    std::string_view error;
    if (request == Request::Add) {
        if (snapshot.members.size() >= 10)
            error = "This group has reached the 10 member limit.";
        else if (isMember)
            error = "This user is already in the group.";
        else if (!origin->relationships()->isFriend(userId))
            error = "Only friends can be added.";
    } else if (!snapshot.canRemove)
        error = "Only the group owner can remove members.";
    else if (!isMember)
        error = "This user is no longer in the group.";
    if (!error.empty()) {
        form->update(std::move(snapshot));
        form->setError(error);
        return;
    }

    pending = true;
    const auto requestGeneration = ++generation;
    form->setPending(true);
    form->setError({});
    auto completed = [self = Platform::Guard(this), originGuard = originInstance,
                      location = originLocation, requestGeneration](const auto &result) {
        if (self)
            self->finishRequest(requestGeneration, originGuard, location, result.success(), result.error);
    };
    if (request == Request::Add)
        origin->addDmRecipient(currentChannelId, userId, std::move(completed));
    else
        origin->removeGroupDmRecipient(currentChannelId, userId, std::move(completed));
}

void ManageGroupDmController::finishRequest(std::uint64_t requestGeneration,
    const Platform::Guard<Core::ClientInstance> &origin, const UI::Location &location,
    bool success, std::string_view error)
{
    if (!visible || !pending || generation != requestGeneration || !origin
        || originInstance.get() != origin.get() || originLocation != location
        || shell.currentInstance() != origin.get() || shell.location() != location)
        return;
    pending = false;
    form->setPending(false);
    refresh();
    if (visible) {
        form->setError(success ? std::string_view{} : error.empty() ? std::string_view("Could not update group members. Try again.") : error);
        if (!success)
            form->focusSearch();
    }
}

void ManageGroupDmController::refresh()
{
    if (!visible || !originInstance)
        return;
    auto *origin = originInstance.get();
    if (shell.currentInstance() != origin || shell.location() != originLocation) {
        close();
        return;
    }
    const auto channel = availableGroup(origin, currentChannelId);
    if (!channel) {
        close();
        return;
    }
    form->update(buildSnapshot(*origin, *channel));
}

GroupDmMembersSnapshot ManageGroupDmController::buildSnapshot(Core::ClientInstance &instance, const Discord::Channel &channel)
{
    const auto selfId = instance.accountId();
    const auto ownerId = channel.ownerId.valueOr(Core::Snowflake::Invalid);
    GroupDmMembersSnapshot snapshot;
    snapshot.canRemove = ownerId == selfId;
    const auto recipientCount = channel.recipientIds.hasValue() ? channel.recipientIds->size()
        : channel.recipients.hasValue() ? channel.recipients->size() : 0;
    snapshot.members.reserve(recipientCount + 1);

    auto makePerson = [&](Core::Snowflake id, const Discord::User *user) {
        std::string name;
        std::string username;
        std::string avatar;
        if (user) {
            name = instance.users()->getNickname(id, Core::Snowflake::Invalid);
            if (name.empty())
                name = user->getDisplayName();
            username = user->username.get();
            avatar = instance.users()->getAvatarUrl(*user, Core::Snowflake::Invalid, 64);
        } else if (id == selfId) {
            const auto &account = instance.accountInfo();
            name = account.displayName.empty() ? account.username : account.displayName;
            username = account.username;
            if (!account.avatar.empty())
                avatar = Discord::Cdn::userAvatar(id, account.avatar, 64);
        }
        if (name.empty())
            name = id.toString();
        if (avatar.empty())
            avatar = Discord::Cdn::defaultUserAvatar(id, 64);
        return GroupDmMemberPerson{id, std::move(name), std::move(username),
            shell.imageSource(avatar, Platform::Size{32, 32}), id == selfId, id == ownerId};
    };
    auto findUser = [&](Core::Snowflake id) -> std::optional<Discord::User> {
        auto user = instance.users()->getUser(id);
        if (user)
            return user;
        if (channel.recipients.hasValue()) {
            for (const auto &recipient : channel.recipients.get())
                if (recipient.id.get() == id)
                    return recipient;
        }
        if (auto relationship = instance.relationships()->getRelationship(id))
            if (relationship->user->id.get() == id)
                return relationship->user.get();
        return std::nullopt;
    };
    auto containsMember = [&](Core::Snowflake id) {
        return std::any_of(snapshot.members.begin(), snapshot.members.end(),
            [id](const GroupDmMemberPerson &member) { return member.userId == id; });
    };
    auto appendMember = [&](Core::Snowflake id) {
        if (!validId(id) || containsMember(id))
            return;
        auto user = findUser(id);
        snapshot.members.push_back(makePerson(id, user ? &*user : nullptr));
    };
    appendMember(selfId);
    if (channel.recipientIds.hasValue()) {
        for (auto id : channel.recipientIds.get())
            appendMember(id);
    } else if (channel.recipients.hasValue()) {
        for (const auto &user : channel.recipients.get())
            appendMember(user.id.get());
    }

    struct FriendCandidate
    {
        GroupDmMemberPerson person;
        std::string nameKey;
        std::string usernameKey;
    };
    std::vector<FriendCandidate> sortedFriends;
    const auto &relationships = instance.relationships()->all();
    sortedFriends.reserve(relationships.size());
    for (const auto &[id, relationship] : relationships) {
        if (!validId(id) || containsMember(id) || relationship.type.get() != Discord::RelationshipType::FRIEND)
            continue;
        auto user = instance.users()->getUser(id);
        if (!user && relationship.user->id.get() == id)
            user = relationship.user.get();
        if (!user)
            continue;
        auto person = makePerson(id, &*user);
        auto nameKey = Platform::caseFold(person.displayName);
        auto usernameKey = Platform::caseFold(person.username);
        sortedFriends.push_back({std::move(person), std::move(nameKey), std::move(usernameKey)});
    }
    std::sort(sortedFriends.begin(), sortedFriends.end(), [](const FriendCandidate &a, const FriendCandidate &b) {
        if (a.nameKey != b.nameKey)
            return a.nameKey < b.nameKey;
        if (a.usernameKey != b.usernameKey)
            return a.usernameKey < b.usernameKey;
        return a.person.userId < b.person.userId;
    });
    snapshot.friends.reserve(sortedFriends.size());
    for (auto &candidate : sortedFriends)
        snapshot.friends.push_back(std::move(candidate.person));
    return snapshot;
}

} // namespace Styx::UI::Rml
