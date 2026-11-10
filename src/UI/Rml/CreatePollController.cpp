#include "CreatePollController.hpp"

#include "ExpressionPickerController.hpp"
#include "PollCreateForm.hpp"
#include "RmlView.hpp"
#include "Shell.hpp"

#include "Core/ClientInstance.hpp"
#include "Core/Emoji/PickerEmoji.hpp"
#include "Core/MessageManager.hpp"
#include "Core/PermissionManager.hpp"
#include "Discord/Client.hpp"
#include "Discord/Enums.hpp"
#include "Discord/PollCreate.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <string_view>

namespace Styx {
namespace UI {
namespace Rml {

CreatePollController::CreatePollController(Shell &shell)
    : Controller(shell, nullptr)
    , form(std::make_unique<PollCreateForm>(
          [this](const std::string &url, Platform::Size size) { return this->shell.imageSource(url, size); },
          [this]() { requestRender(); }))
{
    track(this->shell.instanceChanged.connect([this](Core::ClientInstance *instance) {
        if (visible && (!instance || instance != originInstance.get() || instance->accountId() != originAccountId))
            close();
    }));
    track(this->shell.locationChanged.connect([this](const UI::Location &loc) {
        if (visible && (loc.accountId != originAccountId || loc.channelId != originChannelId))
            close();
    }));

    track(form->changed.connect([this]() {
        nonce.clear();
        pickerTargetIndex = std::nullopt;
        if (picker && picker->isOpen()) {
            suppressFocusRestoration = true;
            picker->close();
            suppressFocusRestoration = false;
        }
    }));
    track(form->submitRequested.connect([this]() {
        submit();
    }));
    track(form->cancelRequested.connect([this]() {
        close();
    }));
    track(form->emojiRequested.connect([this](std::size_t index) {
        requestEmoji(index);
    }));
}

CreatePollController::~CreatePollController()
{
    visible = false;
    suppressFocusRestoration = true;
    pickerTargetIndex = std::nullopt;
    if (picker)
        picker->close();
    form->setDocument(nullptr);
    if (document)
        document->Close();
}

void CreatePollController::open()
{
    auto *instance = shell.currentInstance();
    const auto loc = shell.location();
    if (!canCreate(instance, loc.channelId))
        return;

    ensureDocument();
    if (!document)
        return;

    if (visible) {
        document->Show(::Rml::ModalFlag::None);
        document->SetProperty("display", "flex");
        document->PullToFront();
        form->focusQuestion();
        return;
    }

    ++generation;
    pending = false;
    nonce.clear();
    pickerTargetIndex = std::nullopt;
    originInstance = Platform::Guard<Core::ClientInstance>(instance);
    originAccountId = loc.accountId;
    originChannelId = loc.channelId;

    form->reset();

    visible = true;
    document->Show(::Rml::ModalFlag::None);
    document->SetProperty("display", "flex");
    document->PullToFront();
    form->focusQuestion();
    requestRender();
}

void CreatePollController::close()
{
    if (!visible)
        return;

    visible = false;
    pending = false;
    ++generation;

    suppressFocusRestoration = true;
    pickerTargetIndex = std::nullopt;
    if (picker && picker->isOpen())
        picker->close();
    suppressFocusRestoration = false;

    nonce.clear();
    originInstance.reset();
    originAccountId = Core::Snowflake::Invalid;
    originChannelId = Core::Snowflake::Invalid;

    form->setPending(false);

    if (document)
        document->Hide();

    requestRender();
}

bool CreatePollController::canCreate(Core::ClientInstance *instance, Core::Snowflake channelId)
{
    if (!instance || !instance->discord())
        return false;

    const auto selfId = instance->discord()->getMe().id.get();
    if (!selfId.isValid() || !channelId.isValid())
        return false;

    const auto channel = instance->getChannel(channelId);
    if (!channel)
        return false;

    const auto type = channel->type.get();
    switch (type) {
    case Discord::ChannelType::DM:
    case Discord::ChannelType::GROUP_DM:
        return true;
    case Discord::ChannelType::GUILD_TEXT:
    case Discord::ChannelType::GUILD_NEWS:
    case Discord::ChannelType::GUILD_VOICE:
    case Discord::ChannelType::GUILD_STAGE_VOICE:
    case Discord::ChannelType::NEWS_THREAD:
    case Discord::ChannelType::PUBLIC_THREAD:
    case Discord::ChannelType::PRIVATE_THREAD:
        break;
    default:
        return false;
    }

    if (channel->isArchived())
        return false;

    Core::Snowflake permChannel = channelId;
    Discord::Permissions required = Discord::Permission::VIEW_CHANNEL | Discord::Permission::SEND_POLLS;
    if (channel->isThread()) {
        if (!channel->parentId.hasValue() || !channel->parentId->isValid())
            return false;
        permChannel = channel->parentId.get();
        required |= Discord::Permission::SEND_MESSAGES_IN_THREADS;
    } else {
        required |= Discord::Permission::SEND_MESSAGES;
    }

    const auto permissions = instance->permissions()->getChannelPermissions(selfId, permChannel);
    return (permissions & required) == required;
}

void CreatePollController::ensureDocument()
{
    if (document)
        return;

    auto *view = shell.view();
    if (!view || !view->context())
        return;

    document = view->context()->LoadDocument("ui/create_poll.rml");
    if (!document)
        return;

    regionRoot = document;
    form->setDocument(document);
}

void CreatePollController::submit()
{
    if (!visible || pending)
        return;

    auto *currentInst = shell.currentInstance();
    const auto loc = shell.location();
    if (!currentInst || currentInst != originInstance.get() ||
        loc.accountId != originAccountId || loc.channelId != originChannelId) {
        close();
        return;
    }

    if (!canCreate(originInstance.get(), originChannelId)) {
        form->setError("You do not have permission to post polls in this channel.");
        return;
    }

    if (nonce.empty())
        nonce = Core::Snowflake::generateNonce().toString();

    pending = true;
    form->setPending(true);
    form->setError({});

    const auto originChannel = originChannelId;
    const auto originAccount = originAccountId;
    const auto currentGen = generation;
    Platform::Guard<CreatePollController> self(this);
    Platform::Guard<Core::ClientInstance> origin(originInstance);

    origin->discord()->createPoll(
        originChannel, form->draft(), nonce,
        [self, origin, originAccount, originChannel, currentGen](const Core::Result<Discord::Message> &result) {
            if (result.success() && origin)
                origin->messages()->onMessageCreated(*result.value);

            if (!self || !self->visible || self->generation != currentGen ||
                self->originAccountId != originAccount || self->originChannelId != originChannel ||
                self->originInstance.get() != origin.get())
                return;
            const auto &location = self->shell.location();
            if (self->shell.currentInstance() != origin.get()
                || location.accountId != originAccount || location.channelId != originChannel)
                return;

            if (result.success()) {
                self->close();
            } else {
                self->pending = false;
                self->form->setPending(false);
                const std::string_view err = !result.error.empty() ? std::string_view(result.error) : "Failed to create poll. Try again.";
                self->form->setError(err);
            }
        });
}

void CreatePollController::requestEmoji(std::size_t index)
{
    if (!visible || pending)
        return;

    auto *instance = originInstance.get();
    if (!instance || !instance->discord() || instance->accountId() != originAccountId)
        return;

    if (index >= form->draft().answers.size())
        return;

    pickerTargetIndex = index;
    pickerGeneration = generation;

    if (!picker) {
        picker = std::make_unique<ExpressionPickerController>(shell);
        track(picker->emojiPicked.connect([this](const Core::PickerEmoji &emoji, bool) {
            onEmojiPicked(emoji);
        }));
        track(picker->closed.connect([this]() {
            onPickerClosed();
        }));
    }

    const Platform::Rect anchor = form->emojiAnchor(index);
    picker->openForPoll(instance->emojis(), originAccountId, originChannelId, anchor);
}

void CreatePollController::onEmojiPicked(const Core::PickerEmoji &emoji)
{
    if (!visible || pending || generation != pickerGeneration || !pickerTargetIndex.has_value())
        return;

    const std::size_t index = *pickerTargetIndex;
    if (index >= form->draft().answers.size())
        return;

    form->setEmoji(index, emoji.toReactionEmoji());
    suppressFocusRestoration = true;
    picker->close();
    suppressFocusRestoration = false;
    pickerTargetIndex = std::nullopt;
    form->focusEmoji(index);
}

void CreatePollController::onPickerClosed()
{
    if (!visible || suppressFocusRestoration) {
        pickerTargetIndex = std::nullopt;
        return;
    }

    if (pickerTargetIndex.has_value()) {
        const std::size_t index = *pickerTargetIndex;
        pickerTargetIndex = std::nullopt;
        if (index < form->draft().answers.size())
            form->focusEmoji(index);
    }
}

} // namespace Rml
} // namespace UI
} // namespace Styx
