#include "SwitcherController.hpp"

#include "Shell.hpp"
#include "ShortcutController.hpp"
#include "RmlView.hpp"
#include "Core/ClientInstance.hpp"
#include "Core/Emoji/EmojiManager.hpp"
#include "Core/RelationshipManager.hpp"
#include "Discord/CdnUrls.hpp"
#include "Discord/Entities.hpp"
#include "Discord/Enums.hpp"
#include "Platform/Native.hpp"
#include "UI/ChannelList/ChannelNode.hpp"
#include "UI/ChannelList/ChannelTreeModel.hpp"

#include <charconv>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

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

std::string avatarImageSource(const Shell &shell, Core::Snowflake userId, const std::string &avatarHash)
{
    const std::string url = !avatarHash.empty()
        ? Discord::Cdn::userAvatar(userId, avatarHash, 64)
        : Discord::Cdn::defaultUserAvatar(userId);
    return shell.imageSource(url, Platform::Size{20, 20});
}

} // namespace

SwitcherController::SwitcherController(Shell &shell)
    : Controller(shell, nullptr)
{
}

SwitcherController::~SwitcherController()
{
    if (switcherDocument) {
        switcherDocument->Close();
        switcherDocument = nullptr;
    }
}

void SwitcherController::open()
{
    if (!shell.currentInstance())
        return;

    ensureDocument();
    if (!switcherDocument)
        return;

    collectRows();
    if (auto *inputEl = input())
        inputEl->SetValue("");

    visible = true;
    switcherDocument->Show(::Rml::ModalFlag::None);

    if (auto *inputEl = input())
        inputEl->Focus();

    updateResults();
    requestRender();
}

void SwitcherController::close()
{
    if (!visible)
        return;

    visible = false;
    if (switcherDocument)
        switcherDocument->Hide();
    requestRender();
}

void SwitcherController::ensureDocument()
{
    if (switcherDocument)
        return;

    auto *view = shell.view();
    if (!view || !view->context())
        return;

    switcherDocument = view->context()->LoadDocument("ui/switcher.rml");
    if (!switcherDocument)
        return;

    regionRoot = switcherDocument;
    setupEventListeners();
}

void SwitcherController::setupEventListeners()
{
    if (!switcherDocument)
        return;

    listen("click", [this](::Rml::Event &event) {
        auto *target = event.GetTargetElement();
        if (!target)
            return;

        auto *actionEl = closest(target, "data-action");
        if (!actionEl)
            return;

        const std::string action = attribute(actionEl, "data-action");
        if (action == "close") {
            close();
        } else if (action == "open-row") {
            const std::string idxStr = attribute(actionEl, "data-index");
            std::size_t idx = 0;
            const auto [ptr, ec] = std::from_chars(idxStr.data(), idxStr.data() + idxStr.size(), idx);
            if (ec == std::errc{} && ptr == idxStr.data() + idxStr.size())
                activate(idx);
        }
    });

    if (auto *inputEl = input()) {
        listen("change", [this](::Rml::Event &) {
            updateResults();
        }, inputEl);

        listen("keydown", [this](::Rml::Event &event) {
            // This document's keys never reach the main window's shortcut listener.
            if (ShortcutController::actionFor(event) == ShortcutAction::QuickSwitcher) {
                event.StopPropagation();
                close();
                return;
            }

            const int key = event.GetParameter<int>("key_identifier", 0);
            if (key == ::Rml::Input::KI_DOWN) {
                event.StopPropagation();
                moveSelection(1);
            } else if (key == ::Rml::Input::KI_UP) {
                event.StopPropagation();
                moveSelection(-1);
            } else if (key == ::Rml::Input::KI_RETURN || key == ::Rml::Input::KI_NUMPADENTER) {
                event.StopPropagation();
                if (selected >= 0)
                    activate(static_cast<std::size_t>(selected));
            } else if (key == ::Rml::Input::KI_ESCAPE) {
                event.StopPropagation();
                close();
            }
        }, inputEl);
    }
}

void SwitcherController::collectRows()
{
    rows.clear();

    auto *client = shell.currentInstance();
    if (!client)
        return;

    const Core::Snowflake account = client->accountId();
    std::unordered_set<Core::Snowflake> dmRecipients;

    // Every row from the tree starts the same way; only the kind, server and image differ.
    auto addRow = [this](const ChannelNode &node, Core::SwitcherItem::Kind kind, const ChannelNode *server, std::string image) -> Row & {
        Row &row = rows.emplace_back();
        row.item.kind = kind;
        row.item.id = node.id;
        row.item.guildId = server ? server->id : Core::Snowflake::Invalid;
        row.item.name = node.name;
        if (server && server != &node)
            row.detail = server->name;
        row.image = std::move(image);
        return row;
    };

    std::function<void(const ChannelNode &, const ChannelNode *)> walk = [&](const ChannelNode &node, const ChannelNode *server) {
        using Kind = Core::SwitcherItem::Kind;
        switch (node.type) {
        case ChannelNode::Type::Server:
            server = &node;
            addRow(node, Kind::Server, server, node.TEMP_iconHash.empty()
                ? std::string("icon:messages-square")
                : shell.imageSource(Discord::Cdn::guildIcon(node.id, node.TEMP_iconHash, 64), Platform::Size{20, 20}));
            break;
        case ChannelNode::Type::Channel:
        case ChannelNode::Type::Forum:
        case ChannelNode::Type::Thread:
            addRow(node, Kind::TextChannel, server, "icon:hash");
            break;
        // Its children are the people in the call.
        case ChannelNode::Type::VoiceChannel:
            addRow(node, Kind::VoiceChannel, server, "icon:volume-2");
            return;
        case ChannelNode::Type::DMChannel: {
            Row &row = addRow(node, Kind::Dm, nullptr, node.dmRecipientId.isValid()
                ? avatarImageSource(shell, node.dmRecipientId, node.dmAvatarHash)
                : std::string("icon:messages-square"));
            row.detail = node.recipientIds.size() <= 1 ? "Direct Message" : "Group";
            dmRecipients.insert(node.dmRecipientId);
            return;
        }
        case ChannelNode::Type::VoiceParticipant:
            return;
        default:
            break;
        }
        for (const auto &child : node.children)
            walk(*child, server);
    };

    if (auto *tree = shell.channelTree()) {
        for (const auto &accountNode : tree->rootNode()->children)
            if (accountNode->type == ChannelNode::Type::Account && accountNode->id == account)
                walk(*accountNode, nullptr);
    }

    if (auto *relMgr = client->relationships()) {
        for (const auto &[userId, rel] : relMgr->all()) {
            if (!rel.type.hasValue() || rel.type.get() != Discord::RelationshipType::FRIEND)
                continue;
            if (dmRecipients.contains(userId))
                continue;

            const Discord::User &user = rel.user.hasValue() ? rel.user.get() : Discord::User{};
            std::string name;
            if (rel.nickname.hasValue() && !rel.nickname.get().empty())
                name = rel.nickname.get();
            else if (user.globalName.hasValue() && !user.globalName.get().empty())
                name = user.globalName.get();
            else
                name = user.username.valueOr();

            const std::string avatarHash = user.avatar.valueOr();

            Row row;
            row.item.kind = Core::SwitcherItem::Kind::Friend;
            row.item.id = userId;
            row.item.guildId = Core::Snowflake::Invalid;
            row.item.name = std::move(name);
            row.item.frecency = 0;
            row.detail = "Friend";
            row.image = avatarImageSource(shell, userId, avatarHash);
            rows.push_back(std::move(row));
        }
    }

    if (auto *emojis = client->emojis()) {
        for (auto &row : rows) {
            if (row.item.kind != Core::SwitcherItem::Kind::Friend)
                row.item.frecency = emojis->placeFrecency(row.item.id);
        }
    }
}

void SwitcherController::updateResults()
{
    std::vector<Core::SwitcherItem> items;
    items.reserve(rows.size());
    for (const auto &r : rows)
        items.push_back(r.item);

    ranked = Core::rankSwitcher(inputText(), items);
    selected = 0;
    render();
}

void SwitcherController::render()
{
    if (!switcherDocument)
        return;

    auto *resultsEl = switcherDocument->GetElementById("switcher-results");
    if (!resultsEl)
        return;

    if (ranked.empty()) {
        resultsEl->SetInnerRML(R"(<div class="switcher-empty">No results</div>)");
        requestRender();
        return;
    }

    std::string html;
    for (std::size_t i = 0; i < ranked.size(); ++i) {
        const auto &row = rows[ranked[i]];
        const bool isSelected = (static_cast<int>(i) == selected);
        const std::string_view rowClass = isSelected ? "switcher-row selected" : "switcher-row";

        std::string detailSpan;
        if (!row.detail.empty()) {
            detailSpan = std::format(R"(<span class="switcher-row-detail">{}</span>)",
                                     Platform::htmlEscape(row.detail));
        }

        html += std::format(
            R"(<div class="{}" data-action="open-row" data-index="{}"><img class="switcher-row-image" src="{}"/><span class="switcher-row-name">{}</span>{}</div>)",
            rowClass,
            i,
            Platform::htmlEscape(row.image),
            Platform::htmlEscape(row.item.name),
            detailSpan);
    }

    resultsEl->SetInnerRML(html);
    requestRender();
}

void SwitcherController::moveSelection(int delta)
{
    if (ranked.empty()) {
        selected = 0;
        return;
    }

    const int count = static_cast<int>(ranked.size());
    selected = (selected + delta) % count;
    if (selected < 0)
        selected += count;

    render();

    if (!switcherDocument)
        return;

    if (auto *resultsEl = switcherDocument->GetElementById("switcher-results")) {
        if (auto *child = resultsEl->GetChild(selected))
            child->ScrollIntoView(false);
    }
}

void SwitcherController::activate(std::size_t rankedIndex)
{
    if (rankedIndex >= ranked.size() || ranked[rankedIndex] >= rows.size())
        return;

    const Row targetRow = rows[ranked[rankedIndex]];

    auto *instance = shell.currentInstance();
    if (!instance)
        return;

    const Core::Snowflake account = instance->accountId();

    close();

    switch (targetRow.item.kind) {
    case Core::SwitcherItem::Kind::Server:
        shell.openGuild(account, targetRow.item.id);
        break;
    case Core::SwitcherItem::Kind::TextChannel:
    case Core::SwitcherItem::Kind::VoiceChannel:
        shell.openChannel(account, targetRow.item.guildId, targetRow.item.id);
        break;
    case Core::SwitcherItem::Kind::Dm:
        shell.openChannel(account, Core::Snowflake::Invalid, targetRow.item.id);
        break;
    case Core::SwitcherItem::Kind::Friend:
        shell.openDmWithUser(account, targetRow.item.id);
        break;
    }
}

::Rml::ElementFormControlInput *SwitcherController::input() const
{
    if (!switcherDocument)
        return nullptr;
    return rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(switcherDocument->GetElementById("switcher-input"));
}

std::string SwitcherController::inputText() const
{
    auto *inputEl = input();
    return inputEl ? inputEl->GetValue() : std::string();
}

} // namespace Rml
} // namespace UI
} // namespace Styx
