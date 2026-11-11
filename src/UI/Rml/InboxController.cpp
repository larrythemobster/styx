#include "InboxController.hpp"
#include "Shell.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/JsonUtils.hpp"
#include "Core/NotificationText.hpp"
#include "Core/UserManager.hpp"
#include "UI/ChannelList/ChannelTreeModel.hpp"
#include "Storage/MessageRepository.hpp"
#include "Platform/Native.hpp"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <algorithm>
#include <functional>

namespace Styx::UI::Rml {
namespace {
using Core::Snowflake;
constexpr int PageSize = 25;
std::string esc(const std::string &s) { return Platform::htmlEscape(s); }
Snowflake guildOf(const ChannelNode *node)
{
    for (auto *p = node; p; p = p->parent)
        if (p->type == ChannelNode::Type::Server) return p->id;
    return Snowflake::Invalid;
}
}

InboxController::InboxController(Shell &shell) : Controller(shell, nullptr)
{
    track(shell.instanceChanged.connect([this](auto *) { accountChanged(); }));
    if (auto *tree = shell.channelTree()) {
        track(tree->changed.connect([this](auto *) { if (visible) render(); }));
        track(tree->reset.connect([this] { if (visible) render(); }));
    }
}

InboxController::~InboxController()
{
    removeListeners();
    if (document) document->Close();
}

void InboxController::accountChanged()
{
    hide();
    ++generation;
    account = Snowflake::Invalid;
    mentions.clear();
    mentionCursor = Snowflake::Invalid;
    mentionsLoaded = false;
    mentionsLoading = false;
    mentionsMore = true;
    mentionsError.clear();
    messageRepo.reset();
    previews.clear();
}

void InboxController::ensureDocument()
{
    if (document || !shell.view() || !shell.view()->context()) return;
    document = shell.view()->context()->LoadDocument("ui/inbox.rml");
    if (!document) return;
    regionRoot = document;
    listen("click", [this](::Rml::Event &event) {
        if (auto *el = closest(event.GetTargetElement(), "data-action")) activate(el);
    });
    listen("keydown", [this](::Rml::Event &event) {
        const int key = event.GetParameter<int>("key_identifier", 0);
        if (key == ::Rml::Input::KI_ESCAPE) {
            event.StopPropagation();
            hide();
            return;
        }
        if (key == ::Rml::Input::KI_RETURN || key == ::Rml::Input::KI_NUMPADENTER || key == ::Rml::Input::KI_SPACE) {
            auto *target = event.GetTargetElement();
            if (target && target->GetTagName() == "button" && target->HasAttribute("data-action")) {
                event.StopPropagation();
                target->DispatchEvent("click", {});
            }
            return;
        }
        if (key == ::Rml::Input::KI_TAB) {
            ::Rml::ElementList controls;
            document->QuerySelectorAll(controls, "button");
            std::erase_if(controls, [](auto *el) { return !el->IsVisible() || el->HasAttribute("disabled"); });
            if (controls.empty()) return;
            auto *focused = document->GetContext()->GetFocusElement();
            auto position = std::find(controls.begin(), controls.end(), focused);
            int index = position == controls.end() ? -1 : static_cast<int>(position - controls.begin());
            const bool backwards = event.GetParameter<bool>("shift_key", false);
            const int count = static_cast<int>(controls.size());
            if (index < 0) index = backwards ? 0 : count - 1;
            index = (index + (backwards ? -1 : 1) + count) % count;
            event.StopPropagation();
            controls[index]->Focus();
            controls[index]->ScrollIntoView();
        }
    });
}

void InboxController::toggle()
{
    if (visible) { hide(); return; }
    auto *instance = shell.currentInstance();
    if (!instance) return;
    if (account != instance->accountId()) {
        accountChanged();
        account = instance->accountId();
        messageRepo = std::make_unique<Storage::MessageRepository>(account);
    }
    ensureDocument();
    if (!document) return;
    if (auto *root = document->GetContext()->GetRootElement()) {
        if (auto *button = root->QuerySelector("[data-toolbar-action=\"inbox\"]"))
            trigger = button->GetObserverPtr();
    }
    visible = true;
    document->Show(::Rml::ModalFlag::None);
    render();
    if (auto *tab = document->GetElementById(mentionsTab ? "inbox-mentions" : "inbox-unreads")) tab->Focus();
    if (mentionsTab && !mentionsLoaded && !mentionsLoading) loadMentions();
    requestRender();
}

void InboxController::hide()
{
    if (!visible) return;
    visible = false;
    ++generation;
    mentionsLoading = false;
    for (auto &[id, preview] : previews) preview.loading = false;
    if (document) document->Hide();
    if (trigger) trigger->Focus();
    trigger.reset();
    requestRender();
}

bool InboxController::permitted(const ChannelNode *node) const
{
    if (!node || (node->type != ChannelNode::Type::Channel && node->type != ChannelNode::Type::DMChannel && node->type != ChannelNode::Type::Thread)) return false;
    auto *instance = shell.currentInstance();
    if (!instance || instance->accountId() != account) return false;
    const auto guild = guildOf(node);
    if (!guild.isValid()) return node->type == ChannelNode::Type::DMChannel;
    for (auto *p = node; p; p = p->parent) if (p->unavailable) return false;
    const auto channel = node->type == ChannelNode::Type::Thread ? node->parentId : node->id;
    return channel.isValid() && instance->permissions() && instance->permissions()->hasChannelPermission(account, channel, Discord::Permission::VIEW_CHANNEL);
}

void InboxController::activate(::Rml::Element *el)
{
    const std::string action = attribute(el, "data-action");
    if (action == "close") { hide(); return; }
    if (action == "unreads" || action == "mentions") {
        mentionsTab = action == "mentions";
        render();
        if (mentionsTab && !mentionsLoaded && !mentionsLoading) loadMentions();
        if (auto *tab = document->GetElementById(mentionsTab ? "inbox-mentions" : "inbox-unreads")) tab->Focus();
        return;
    }
    if (action == "more") { loadMentions(); return; }
    const auto channel = Core::JsonUtils::detail::snowflake(attribute(el, "data-channel"));
    auto *tree = shell.channelTree();
    auto *node = tree && channel.isValid() ? tree->findChannelTreeNode(channel, account) : nullptr;
    if (!permitted(node)) return;
    if (action == "preview") { loadPreview(channel); return; }
    if (action == "read") { shell.markChannelRead(channel); return; }
    if (action == "open") {
        const auto guild = guildOf(node);
        const auto message = Core::JsonUtils::detail::snowflake(attribute(el, "data-message"));
        hide();
        shell.openChannel(account, guild, channel);
        if (message.isValid()) shell.jumpToMessage(channel, message);
    }
}

void InboxController::loadMentions()
{
    auto *instance = shell.currentInstance();
    if (!visible || mentionsLoading || !mentionsMore || !instance || instance->accountId() != account) return;
    mentionsLoading = true;
    mentionsError.clear();
    render();
    const unsigned request = generation;
    const auto cursor = mentionCursor;
    instance->discord()->fetchRecentMentions(cursor, PageSize, [this, guard = lifetime(), request, cursor](const Core::Result<std::vector<Discord::Message>> &result) {
        if (guard.expired() || request != generation || !visible || cursor != mentionCursor) return;
        mentionsLoading = false;
        if (!result.success()) mentionsError = result.error.empty() ? "Could not load mentions" : result.error;
        else {
            mentionsLoaded = true;
            mentionsMore = result.value->size() == PageSize;
            if (!result.value->empty()) {
                const auto next = result.value->back().id.get();
                if (next == cursor) mentionsMore = false;
                else mentionCursor = next;
                for (const auto &message : *result.value) {
                    bool duplicate = false;
                    for (const auto &existing : mentions) if (existing.id.get() == message.id.get()) { duplicate = true; break; }
                    if (!duplicate) mentions.push_back(message);
                }
            }
        }
        render();
    });
}

void InboxController::loadPreview(Snowflake channel)
{
    auto *instance = shell.currentInstance();
    auto *tree = shell.channelTree();
    auto *node = tree ? tree->findChannelTreeNode(channel, account) : nullptr;
    if (!instance || !permitted(node)) return;
    auto &preview = previews[channel];
    if (preview.loading) return;
    preview.loading = true;
    preview.error.clear();
    preview.lastMessage = node->lastMessageId;
    const auto last = preview.lastMessage;
    const unsigned request = generation;
    render();
    instance->discord()->fetchLatestMessages(channel, 3, [this, guard = lifetime(), request, channel, last](const Core::Result<std::vector<Discord::Message>> &result) {
        if (guard.expired() || request != generation || !visible) return;
        auto it = previews.find(channel);
        if (it == previews.end() || it->second.lastMessage != last) return;
        it->second.loading = false;
        if (result.success()) {
            it->second.messages = *result.value;
            std::reverse(it->second.messages.begin(), it->second.messages.end());
            it->second.loaded = true;
        }
        else it->second.error = result.error.empty() ? "Could not load preview" : result.error;
        render();
    });
}

std::string InboxController::messageRml(const Discord::Message &message, Snowflake guild) const
{
    auto *instance = shell.currentInstance();
    if (!instance) return {};
    std::string author = "Unknown user", avatar;
    if (message.author.hasValue() && message.author->id.get().isValid()) {
        author = instance->users()->getAuthorDisplayName(message.author.get(), guild);
        avatar = shell.imageSource(instance->users()->getAvatarUrl(message.author.get(), guild, 64), {32, 32});
    }
    const auto text = Core::notificationText(message.content.valueOr(), message.attachments.hasValue() ? static_cast<int>(message.attachments->size()) : 0, message.stickerItems.hasValue() ? static_cast<int>(message.stickerItems->size()) : 0, instance->mentionNames(guild));
    const std::string time = message.timestamp.hasValue() && message.timestamp->valid() ? message.timestamp->format("%m/%d/%Y %I:%M %p") : "";
    return "<button class=\"inbox-message\" data-action=\"open\" data-channel=\"" + message.channelId.get().toString() + "\" data-message=\"" + message.id.get().toString() + "\"><span class=\"inbox-avatar\"><img src=\"" + esc(avatar) + "\"/></span><span class=\"inbox-message-body\"><span class=\"inbox-author\">" + esc(author) + "</span><span class=\"inbox-time\"> " + esc(time) + "</span><span class=\"inbox-text\">" + esc(text) + "</span></span></button>";
}

void InboxController::render()
{
    if (!document || !visible) return;
    std::string focusAction, focusChannel, focusMessage;
    if (auto *focused = document->GetContext()->GetFocusElement()) {
        if (auto *list = document->GetElementById("inbox-content")) {
            for (auto *parent = focused; parent; parent = parent->GetParentNode()) {
                if (parent == list) {
                    focusAction = attribute(focused, "data-action");
                    focusChannel = attribute(focused, "data-channel");
                    focusMessage = attribute(focused, "data-message");
                    break;
                }
            }
        }
    }
    auto *tree = shell.channelTree();
    std::string content;
    int count = 0;
    if (tree && tree->rootNode()) {
        std::function<void(ChannelNode *)> visit = [&](ChannelNode *node) {
            if (node->type == ChannelNode::Type::Account && node->id != account) return;
            if (permitted(node) && node->self.isUnread && (!node->self.isMuted || node->self.mentionCount > 0)) {
                ++count;
                if (!mentionsTab) {
                    auto *server = ChannelTreeModel::findGuildNode(node);
                    const std::string context = server ? server->name + " / #" + node->name : node->name;
                    const auto id = node->id.toString();
                    content += "<div class=\"inbox-row\"><div class=\"inbox-row-head\"><button class=\"inbox-channel\" data-action=\"open\" data-channel=\"" + id + "\">" + esc(context) + "</button><button class=\"inbox-read\" data-action=\"read\" data-channel=\"" + id + "\">Mark read</button></div>";
                    auto [it, inserted] = previews.try_emplace(node->id);
                    auto &preview = it->second;
                    if (inserted || preview.lastMessage != node->lastMessageId) {
                        preview = {};
                        preview.lastMessage = node->lastMessageId;
                        if (messageRepo && node->lastMessageId.isValid()) {
                            auto cached = messageRepo->getMessage(node->lastMessageId);
                            if (cached && cached->channelId.get() == node->id)
                                preview.messages.push_back(std::move(*cached));
                        }
                    }
                    for (const auto &message : preview.messages) content += messageRml(message, guildOf(node));
                    if (preview.loading) content += "<div class=\"inbox-status\">Loading preview...</div>";
                    else if (!preview.error.empty()) content += "<div class=\"inbox-status\">" + esc(preview.error) + "</div><button class=\"inbox-link\" data-action=\"preview\" data-channel=\"" + id + "\">Retry</button>";
                    else if (!preview.loaded) content += "<button class=\"inbox-link\" data-action=\"preview\" data-channel=\"" + id + "\">Show recent messages</button>";
                    else if (preview.messages.empty()) content += "<div class=\"inbox-status\">No messages to preview.</div>";
                    content += "</div>";
                }
            }
            for (auto &child : node->children) visit(child.get());
        };
        visit(tree->rootNode());
    }
    if (mentionsTab) {
        for (const auto &message : mentions) {
            auto *node = tree ? tree->findChannelTreeNode(message.channelId.get(), account) : nullptr;
            if (!permitted(node)) continue;
            auto *server = ChannelTreeModel::findGuildNode(node);
            const std::string context = server ? server->name + " / #" + node->name : node->name;
            content += "<div class=\"inbox-row\"><div class=\"inbox-context\">" + esc(context) + "</div>" + messageRml(message, guildOf(node)) + "</div>";
        }
        if (mentionsLoading) content += "<div class=\"inbox-status\">Loading mentions...</div>";
        else if (!mentionsError.empty()) content += "<div class=\"inbox-status\">" + esc(mentionsError) + "</div><button class=\"inbox-link\" data-action=\"more\">Retry</button>";
        else if (mentionsLoaded && mentions.empty()) content += "<div class=\"inbox-empty\">No recent mentions.</div>";
        else if (mentionsLoaded && content.empty()) content += "<div class=\"inbox-empty\">No mentions in available channels.</div>";
        if (mentionsLoaded && mentionsMore && !mentionsLoading && mentionsError.empty()) content += "<button class=\"inbox-more\" data-action=\"more\">Load more</button>";
    } else if (!count) content = "<div class=\"inbox-empty\">You're all caught up.</div>";
    if (auto *unreads = document->GetElementById("inbox-unreads")) {
        unreads->SetInnerRML(count ? "Unreads (" + std::to_string(count) + ")" : "Unreads");
        unreads->SetClass("selected", !mentionsTab);
    }
    if (auto *tab = document->GetElementById("inbox-mentions")) tab->SetClass("selected", mentionsTab);
    if (auto *list = document->GetElementById("inbox-content")) {
        list->SetInnerRML(content);
        if (!focusAction.empty()) {
            ::Rml::ElementList controls;
            list->QuerySelectorAll(controls, "button");
            auto it = std::find_if(controls.begin(), controls.end(), [&](auto *el) {
                return attribute(el, "data-action") == focusAction &&
                       attribute(el, "data-channel") == focusChannel &&
                       attribute(el, "data-message") == focusMessage;
            });
            if (it != controls.end()) (*it)->Focus();
            else if (auto *tab = document->GetElementById(mentionsTab ? "inbox-mentions" : "inbox-unreads")) tab->Focus();
        }
    }
    requestRender();
}

} // namespace Styx::UI::Rml
