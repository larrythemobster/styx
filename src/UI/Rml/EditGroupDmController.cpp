#include "EditGroupDmController.hpp"
#include "GroupDmDetailsForm.hpp"

#include "Core/ClientInstance.hpp"
#include "Discord/CdnUrls.hpp"
#include "ImageUpload.hpp"
#include "Platform/Desktop.hpp"
#include "RmlRuntime.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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

EditGroupDmController::EditGroupDmController(Shell &shell)
    : Controller(shell, nullptr)
    , form(std::make_unique<GroupDmDetailsForm>([this] { requestRender(); }))
{
    const auto scope = std::to_string(reinterpret_cast<std::uintptr_t>(this));
    uniqueFrameKey = "group-dm-icon-" + scope;

    track(this->shell.instanceChanged.connect([this](Core::ClientInstance *instance) {
        if (visible && (!instance || instance != originInstance.get()))
            close();
    }));
    track(this->shell.locationChanged.connect([this](const UI::Location &location) {
        if (visible && location != originLocation)
            close();
    }));
    track(form->pickIconRequested.connect([this] { chooseIcon(); }));
    track(form->removeIconRequested.connect([this] { handleRemoveIcon(); }));
    track(form->resetRequested.connect([this] { handleReset(); }));
    track(form->submitRequested.connect([this] { save(); }));
    track(form->closeRequested.connect([this] { close(); }));
}

EditGroupDmController::~EditGroupDmController()
{
    visible = false;
    pending = false;
    ++requestGeneration;
    ++pickerGeneration;
    instanceConnections.clear();
    Runtime::instance().clearFrame(uniqueFrameKey);
    form->setDocument(nullptr);
    if (document)
        document->Close();
}

void EditGroupDmController::open(Core::Snowflake channelId)
{
    auto *instance = shell.currentInstance();
    const auto location = shell.location();
    if (!instance || !validId(instance->accountId()) || !validId(channelId)
        || location.accountId != instance->accountId()
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
        form->focusName();
        return;
    }
    if (visible)
        close();

    auto snapshot = buildSnapshot(*channel);

    ++requestGeneration;
    ++pickerGeneration;
    pending = false;
    originInstance = Platform::Guard<Core::ClientInstance>(instance);
    originLocation = location;
    currentChannelId = channelId;

    auto remember = [this](Platform::Connection connection) {
        connection.setAutoDisconnect(true);
        instanceConnections.push_back(std::move(connection));
    };
    remember(instance->channelUpdated.connect([this](const Discord::ChannelUpdate &update) {
        if (update.channel->id.get() == currentChannelId)
            refreshBaseline();
    }));
    remember(instance->channelDeleted.connect([this](const Discord::ChannelDelete &ev) {
        if (ev.id.get() == currentChannelId)
            close();
    }));

    Runtime::instance().clearFrame(uniqueFrameKey);
    form->reset(std::move(snapshot));
    visible = true;
    document->Show(::Rml::ModalFlag::None);
    document->SetProperty("display", "flex");
    document->PullToFront();
    form->focusName();
    requestRender();
}

void EditGroupDmController::close()
{
    if (!visible)
        return;
    visible = false;
    pending = false;
    ++requestGeneration;
    ++pickerGeneration;
    instanceConnections.clear();
    originInstance.reset();
    originLocation = {};
    currentChannelId = Core::Snowflake::Invalid;
    Runtime::instance().clearFrame(uniqueFrameKey);
    form->reset({});
    if (document)
        document->Hide();
    requestRender();
}

void EditGroupDmController::ensureDocument()
{
    if (document || !shell.context())
        return;
    document = shell.context()->LoadDocument("ui/group_dm_details.rml");
    if (document) {
        regionRoot = document;
        form->setDocument(document);
    }
}

std::optional<Discord::Channel> EditGroupDmController::currentGroup()
{
    if (!visible)
        return std::nullopt;
    auto *origin = originInstance.get();
    if (!origin || shell.currentInstance() != origin || shell.location() != originLocation) {
        close();
        return std::nullopt;
    }
    auto channel = availableGroup(origin, currentChannelId);
    if (!channel)
        close();
    return channel;
}

void EditGroupDmController::refreshBaseline()
{
    if (const auto channel = currentGroup())
        form->updateBaseline(buildSnapshot(*channel));
}

void EditGroupDmController::chooseIcon()
{
    if (!visible || pending)
        return;
    if (!currentGroup())
        return;

    const auto currentPickerGen = ++pickerGeneration;
    Platform::Guard<EditGroupDmController> self(this);
    const auto originGuard = originInstance;
    const auto location = originLocation;
    const auto targetChannelId = currentChannelId;

    Platform::Desktop::openFiles("Change Icon", {{"Images", "*.png;*.jpg;*.jpeg;*.gif;*.webp;*.avif;*.bmp;*.svg"}},
        [self, originGuard, location, targetChannelId, currentPickerGen](std::vector<std::string> files) {
            if (!self || files.empty() || !self->visible || self->pending
                || self->pickerGeneration != currentPickerGen
                || self->originInstance.get() != originGuard.get()
                || self->originLocation != location
                || self->currentChannelId != targetChannelId
                || self->shell.currentInstance() != originGuard.get()
                || self->shell.location() != location)
                return;

            if (!self->currentGroup())
                return;

            auto prepared = ImageUpload::prepareCropped(files.front(), 1024, 1024);
            if (prepared.dataUri.empty()) {
                self->form->setError(prepared.error.empty() ? std::string_view("Unable to process image") : std::string_view(prepared.error));
                return;
            }

            self->form->setError({});
            if (!prepared.preview.empty()) {
                Runtime::instance().setFrame(self->uniqueFrameKey, prepared.preview);
                self->form->setIcon(std::move(prepared.dataUri), "frame:" + self->uniqueFrameKey,
                                    prepared.preview.width, prepared.preview.height);
            } else {
                Runtime::instance().clearFrame(self->uniqueFrameKey);
                self->form->setIcon(std::move(prepared.dataUri), {}, 0, 0);
            }
        }, false);
}

void EditGroupDmController::handleRemoveIcon()
{
    if (!visible || pending)
        return;
    ++pickerGeneration;
    if (!currentGroup())
        return;
    Runtime::instance().clearFrame(uniqueFrameKey);
    form->removeIcon();
}

void EditGroupDmController::handleReset()
{
    if (!visible || pending)
        return;
    ++pickerGeneration;
    const auto channel = currentGroup();
    if (!channel)
        return;
    Runtime::instance().clearFrame(uniqueFrameKey);
    form->reset(buildSnapshot(*channel));
}

void EditGroupDmController::save()
{
    if (!visible || pending)
        return;
    ++pickerGeneration;
    if (!currentGroup())
        return;
    if (!form->isDirty())
        return;
    auto editResult = form->edit();
    if (!editResult.success()) {
        form->setError(editResult.error);
        return;
    }

    pending = true;
    const auto requestGen = ++requestGeneration;
    form->setPending(true);
    form->setError({});

    auto completed = [self = Platform::Guard(this), originGuard = originInstance,
                      location = originLocation, targetChannelId = currentChannelId,
                      requestGen](const Core::Result<Discord::Channel> &result) {
        if (self)
            self->finishSave(requestGen, originGuard, location, targetChannelId, result);
    };
    originInstance.get()->modifyGroupDm(currentChannelId, *editResult.value, std::move(completed));
}

void EditGroupDmController::finishSave(std::uint64_t requestGen,
    const Platform::Guard<Core::ClientInstance> &origin, const UI::Location &location,
    Core::Snowflake channelId, const Core::Result<Discord::Channel> &result)
{
    if (!visible || !pending || requestGeneration != requestGen || !origin
        || originInstance.get() != origin.get() || originLocation != location
        || currentChannelId != channelId
        || shell.currentInstance() != origin.get() || shell.location() != location)
        return;

    auto *instance = origin.get();
    const auto channel = availableGroup(instance, channelId);
    if (!channel) {
        close();
        return;
    }

    pending = false;
    form->setPending(false);

    if (result.success()) {
        Runtime::instance().clearFrame(uniqueFrameKey);
        form->reset(buildSnapshot(*result.value));
    } else {
        form->updateBaseline(buildSnapshot(*channel));
        form->setError(result.error.empty() ? std::string_view("Could not update group details. Try again.") : std::string_view(result.error));
        form->focusName();
    }
}

GroupDmDetailsSnapshot EditGroupDmController::buildSnapshot(const Discord::Channel &channel) const
{
    GroupDmDetailsSnapshot snapshot;
    if (channel.name.hasValue())
        snapshot.name = channel.name.get();
    if (channel.icon.hasValue() && !channel.icon.get().empty()) {
        snapshot.iconHash = channel.icon.get();
        snapshot.iconSrc = shell.imageSource(Discord::Cdn::channelIcon(channel.id.get(), snapshot.iconHash, 128), Platform::Size(96, 96));
    }
    return snapshot;
}

} // namespace Styx::UI::Rml
