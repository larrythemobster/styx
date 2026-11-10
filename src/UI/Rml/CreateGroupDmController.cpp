#include "CreateGroupDmController.hpp"

#include "GroupDmCreateForm.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/Presence/PresenceManager.hpp"
#include "Core/RelationshipManager.hpp"
#include "Core/UserManager.hpp"
#include "Platform/Utf8.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>
#include <string>
#include <utility>

namespace Styx::UI::Rml {
namespace {

bool validId(Core::Snowflake id)
{
    return id.isValid() && id != Core::Snowflake(0);
}

} // namespace

CreateGroupDmController::CreateGroupDmController(Shell &shell)
    : Controller(shell, nullptr)
    , form(std::make_unique<GroupDmCreateForm>([this] { requestRender(); }))
{
    track(this->shell.instanceChanged.connect([this](Core::ClientInstance *instance) {
        if (visible && (!instance || instance != originInstance.get()))
            close();
    }));
    track(this->shell.locationChanged.connect([this](const UI::Location &location) {
        if (visible && location != originLocation)
            close();
    }));
    track(form->submitRequested.connect([this] { submit(); }));
    track(form->cancelRequested.connect([this] { close(); }));
}

CreateGroupDmController::~CreateGroupDmController()
{
    visible = false;
    instanceConnections.clear();
    form->setDocument(nullptr);
    if (document)
        document->Close();
}

bool CreateGroupDmController::canOpen(Core::ClientInstance *instance, Core::Snowflake source)
{
    return instance && validId(instance->accountId())
        && (!source.isValid() || resolveSourceRecipient(*instance, source).has_value());
}

void CreateGroupDmController::open(Core::Snowflake source)
{
    auto *instance = shell.currentInstance();
    const auto location = shell.location();
    if (!instance || !validId(instance->accountId()) || location.accountId != instance->accountId())
        return;
    std::optional<Core::Snowflake> other;
    if (source.isValid()) {
        other = resolveSourceRecipient(*instance, source);
        if (!other || !location.isHome() || location.channelId != source)
            return;
    }

    ensureDocument();
    if (!document)
        return;
    if (visible && sourceChannelId == source && originInstance.get() == instance && originLocation == location) {
        document->PullToFront();
        form->focusSearch();
        return;
    }
    if (visible)
        close();

    ++generation;
    pending = false;
    originInstance = Platform::Guard<Core::ClientInstance>(instance);
    originLocation = location;
    sourceChannelId = source;
    sourceOtherRecipient = other.value_or(Core::Snowflake::Invalid);
    auto remember = [this](Platform::Connection connection) {
        connection.setAutoDisconnect(true);
        instanceConnections.push_back(std::move(connection));
    };
    remember(instance->relationships()->relationshipChanged.connect([this](Core::Snowflake) { refreshCandidates(); }));
    remember(instance->relationships()->relationshipsReset.connect([this] { refreshCandidates(); }));
    remember(instance->presences()->presencesChanged.connect([this](const std::vector<Core::Snowflake> &) { refreshCandidates(); }));
    remember(instance->membersUpdated.connect([this](Core::Snowflake, const std::vector<Core::Snowflake> &) { refreshCandidates(); }));
    form->reset(buildCandidates(*instance), source.isValid() ? 8 : 9, !source.isValid());
    visible = true;
    document->Show(::Rml::ModalFlag::None);
    document->SetProperty("display", "flex");
    document->PullToFront();
    form->focusSearch();
    requestRender();
}

void CreateGroupDmController::close()
{
    if (!visible)
        return;
    visible = false;
    pending = false;
    ++generation;
    instanceConnections.clear();
    originInstance.reset();
    originLocation = {};
    sourceChannelId = Core::Snowflake::Invalid;
    sourceOtherRecipient = Core::Snowflake::Invalid;
    form->setPending(false);
    if (document)
        document->Hide();
    requestRender();
}

void CreateGroupDmController::ensureDocument()
{
    if (document || !shell.context())
        return;
    document = shell.context()->LoadDocument("ui/create_group_dm.rml");
    if (document) {
        regionRoot = document;
        form->setDocument(document);
    }
}

void CreateGroupDmController::submit()
{
    if (!visible || pending)
        return;
    auto *origin = originInstance.get();
    if (!origin || shell.currentInstance() != origin || shell.location() != originLocation) {
        close();
        return;
    }
    const bool conversion = sourceChannelId.isValid();
    if (conversion) {
        const auto other = resolveSourceRecipient(*origin, sourceChannelId);
        if (!other || *other != sourceOtherRecipient) {
            form->setError("The original conversation is no longer available.");
            return;
        }
    }
    const auto &selected = form->selected();
    if (selected.size() > (conversion ? 8U : 9U) || (conversion && selected.empty())) {
        form->setError("Select friends within the group limit.");
        return;
    }
    for (auto id : selected) {
        if (id == origin->accountId() || id == sourceOtherRecipient || !origin->relationships()->isFriend(id)) {
            refreshCandidates();
            form->setError("One or more selected users are no longer available.");
            return;
        }
    }

    pending = true;
    const auto requestGeneration = ++generation;
    form->setPending(true);
    form->setError({});
    auto finish = [self = Platform::Guard(this), originGuard = originInstance,
                   location = originLocation, requestGeneration](Core::Snowflake id, std::string_view error) {
        if (self)
            self->finishRequest(requestGeneration, originGuard, location, id, error);
    };
    if (conversion && selected.size() == 1) {
        origin->addDmRecipient(sourceChannelId, selected.front(),
            [finish = std::move(finish)](const Core::Result<std::optional<Discord::Channel>> &result) {
                if (result.success() && result.value->has_value())
                    finish((**result.value).id.get(), {});
                else
                    finish(Core::Snowflake::Invalid, result.error);
            });
        return;
    }
    auto created = [finish = std::move(finish)](const Core::Result<Discord::Channel> &result) {
        if (result.success())
            finish(result.value->id.get(), {});
        else
            finish(Core::Snowflake::Invalid, result.error);
    };
    if (conversion) {
        std::vector<Core::Snowflake> recipients;
        recipients.reserve(selected.size() + 1);
        recipients.push_back(sourceOtherRecipient);
        recipients.insert(recipients.end(), selected.begin(), selected.end());
        origin->createGroupDm(recipients, std::move(created));
    } else {
        origin->createGroupDm(selected, std::move(created));
    }
}

void CreateGroupDmController::finishRequest(std::uint64_t requestGeneration,
    const Platform::Guard<Core::ClientInstance> &origin, const UI::Location &location,
    Core::Snowflake createdId, std::string_view error)
{
    if (!visible || !pending || generation != requestGeneration || !origin
        || originInstance.get() != origin.get() || originLocation != location
        || shell.currentInstance() != origin.get() || shell.location() != location)
        return;
    if (validId(createdId)) {
        const auto cached = origin->getChannel(createdId);
        if (cached && cached->type.get() == Discord::ChannelType::GROUP_DM) {
            close();
            shell.openChannel(location.accountId, Core::Snowflake::Invalid, createdId);
            return;
        }
        error = "The created group is no longer available.";
    }
    pending = false;
    form->setPending(false);
    refreshCandidates();
    form->setError(error.empty() ? "Failed to create group DM. Try again." : error);
    form->focusSearch();
}

void CreateGroupDmController::refreshCandidates()
{
    if (!visible || pending || !originInstance)
        return;
    if (shell.currentInstance() != originInstance.get() || shell.location() != originLocation) {
        close();
        return;
    }
    form->setCandidates(buildCandidates(*originInstance.get()));
}

std::vector<GroupDmFriend> CreateGroupDmController::buildCandidates(Core::ClientInstance &instance)
{
    struct Candidate
    {
        GroupDmFriend person;
        std::string nameKey;
        std::string usernameKey;
    };
    std::vector<Candidate> sorted;
    const auto &relationships = instance.relationships()->all();
    sorted.reserve(relationships.size());
    for (const auto &[id, relationship] : relationships) {
        if (!validId(id) || id == instance.accountId() || id == sourceOtherRecipient
            || relationship.type.get() != Discord::RelationshipType::FRIEND)
            continue;
        auto user = instance.users()->getUser(id);
        if (!user && relationship.user->id.get() == id)
            user = relationship.user.get();
        if (!user)
            continue;
        auto name = instance.users()->getNickname(id, Core::Snowflake::Invalid);
        if (name.empty())
            name = user->getDisplayName();
        GroupDmFriend person{id, std::move(name), user->username.get(),
            shell.imageSource(instance.users()->getAvatarUrl(*user, Core::Snowflake::Invalid, 64), Platform::Size{32, 32})};
        auto nameKey = Platform::caseFold(person.displayName);
        auto usernameKey = Platform::caseFold(person.username);
        sorted.push_back({std::move(person), std::move(nameKey), std::move(usernameKey)});
    }
    std::sort(sorted.begin(), sorted.end(), [](const Candidate &a, const Candidate &b) {
        if (a.nameKey != b.nameKey)
            return a.nameKey < b.nameKey;
        if (a.usernameKey != b.usernameKey)
            return a.usernameKey < b.usernameKey;
        return a.person.userId < b.person.userId;
    });
    std::vector<GroupDmFriend> candidates;
    candidates.reserve(sorted.size());
    for (auto &candidate : sorted)
        candidates.push_back(std::move(candidate.person));
    return candidates;
}

std::optional<Core::Snowflake> CreateGroupDmController::resolveSourceRecipient(Core::ClientInstance &instance,
    Core::Snowflake channelId)
{
    if (!validId(channelId))
        return std::nullopt;
    const auto channel = instance.getChannel(channelId);
    if (!channel || channel->type.get() != Discord::ChannelType::DM
        || (channel->guildId.hasValue() && validId(channel->guildId.get())))
        return std::nullopt;
    std::optional<Core::Snowflake> other;
    const auto take = [&](Core::Snowflake id) {
        if (!validId(id))
            return false;
        if (id != instance.accountId()) {
            if (other)
                return false;
            other = id;
        }
        return true;
    };
    if (channel->recipientIds.hasValue()) {
        for (auto id : channel->recipientIds.get())
            if (!take(id))
                return std::nullopt;
    } else if (channel->recipients.hasValue()) {
        for (const auto &user : channel->recipients.get())
            if (!take(user.id.get()))
                return std::nullopt;
    }
    return other;
}

} // namespace Styx::UI::Rml
