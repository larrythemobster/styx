#include "RailController.hpp"

#include "RmlRuntime.hpp"
#include "Shell.hpp"
#include "Discord/CdnUrls.hpp"
#include "Platform/Utf8.hpp"
#include "UI/ChannelList/ChannelNode.hpp"
#include "UI/ChannelList/ChannelTreeModel.hpp"
#include "UI/ChannelList/ServerRailModel.hpp"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <charconv>
#include <cwctype>
#include <format>

namespace Styx::UI::Rml {
using Core::Snowflake;

namespace {
Snowflake parseId(const std::string &text)
{
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? Snowflake(value) : Snowflake::Invalid;
}
}

RailController::RailController(Shell &shell, ::Rml::Element *root)
    : Controller(shell, root)
{
    listen("click", [this](::Rml::Event &event) {
        auto *item = closest(event.GetTargetElement(), "data-kind");
        if (!item) return;
        const auto kind = attribute(item, "data-kind");
        const auto accountId = parseId(attribute(item, "data-account-id"));
        const auto id = parseId(attribute(item, "data-id"));
        if (kind == "home") shell.openHome(accountId);
        else if (kind == "folder") { if (auto *rail = shell.rail()) rail->toggleFolder(accountId, id); }
        else if (kind == "server") shell.openGuild(accountId, id);
    });
    listen("mousedown", [this](::Rml::Event &event) {
        if (event.GetParameter<int>("button", 0) != 1) return;
        auto *item = closest(event.GetTargetElement(), "data-kind");
        if (!item) return;
        const auto kind = attribute(item, "data-kind");
        const auto accountId = parseId(attribute(item, "data-account-id"));
        const auto id = parseId(attribute(item, "data-id"));
        if (kind == "server") shell.showContextMenuForGuild(accountId, id);
        else if (kind == "home") shell.openAccounts();
    });
    track(shell.locationChanged.connect([this](const Location &loc) {
        if (auto *rail = shell.rail()) {
            if (loc.isHome()) rail->setSelected(ServerRailModel::Kind::AccountHome, loc.accountId, loc.accountId);
            else rail->setSelected(ServerRailModel::Kind::Server, loc.accountId, loc.guildId);
        }
        scheduleRebuild();
    }));
    track(shell.instanceChanged.connect([this](Core::ClientInstance *) { scheduleRebuild(); }));
    track(shell.accountReady.connect([this](Snowflake) { scheduleRebuild(); }));
    if (auto *rail = shell.rail()) {
        track(rail->reset.connect([this]() { scheduleRebuild(); }));
        track(rail->changed.connect([this]() { scheduleRebuild(); }));
    }
    renderRail();
}

void RailController::scheduleRebuild()
{
    rebuildTimer.start(std::chrono::milliseconds(0), false, [this]() { rebuild(); });
}

void RailController::rebuild() { renderRail(); }

std::string RailController::acronym(const std::string &name)
{
    const auto wide = Platform::toUtf16(name);
    std::wstring result;
    bool word = false;
    for (wchar_t ch : wide) {
        if (std::iswspace(ch)) { word = false; continue; }
        if (!word) {
            result += static_cast<wchar_t>(std::towupper(ch));
            if (result.size() >= 4) break;
            word = true;
        } else if (result.size() && result.back() >= 0xd800 && result.back() <= 0xdbff && ch >= 0xdc00 && ch <= 0xdfff) {
            result += ch;
        }
    }
    return Platform::fromUtf16(result);
}

std::string RailController::renderFolderGrid(Snowflake accountId, Snowflake folderId)
{
    std::string rml;
    auto *tree = shell.channelTree();
    auto *folder = tree ? tree->folderNode(accountId, folderId) : nullptr;
    if (!folder) return rml;
    int count = 0;
    for (const auto &child : folder->children) {
        if (child->type != ChannelNode::Type::Server) continue;
        if (count++ >= 4) break;
        if (!child->TEMP_iconHash.empty()) {
            auto src = shell.imageSource(Discord::Cdn::guildIcon(child->id, child->TEMP_iconHash, 32), Platform::Size{20, 20});
            rml += std::format("<div class=\"folder-grid-item\"><img src=\"{}\"/></div>", escape(src));
        } else {
            auto wide = Platform::toUtf16(acronym(child->name));
            if (wide.size() > 2) wide.resize(2);
            rml += std::format("<div class=\"folder-grid-item\"><span class=\"folder-grid-acr\">{}</span></div>", escape(Platform::fromUtf16(wide)));
        }
    }
    return rml;
}

void RailController::renderRail()
{
    auto *model = shell.rail();
    if (!regionRoot || !model) return;
    std::string primary, expanded;
    Snowflake currentFolder = Snowflake::Invalid, lastFolder = Snowflake::Invalid;
    std::size_t row = 0;
    for (const auto &entry : model->rows()) {
        const bool unread = model->isUnread(entry);
        const bool selected = model->isSelected(entry);
        const bool isExpanded = model->isExpanded(entry);
        const int mentions = model->mentionCount(entry);
        const char *kind = entry.kind == ServerRailModel::Kind::AccountHome ? "home" : entry.kind == ServerRailModel::Kind::Folder ? "folder" : "server";
        std::string classes = std::format("rail-item kind-{}", kind);
        if (selected) classes += " selected";
        if (unread) classes += " unread";
        if (entry.kind == ServerRailModel::Kind::Folder && isExpanded) classes += " expanded";
        if (entry.inFolderGroup) classes += " in-folder-group";
        std::string item = std::format("<div class=\"{}\" data-kind=\"{}\" data-account-id=\"{}\" data-id=\"{}\"><div class=\"pill\"></div>", classes, kind, entry.accountId.toString(), entry.id.toString());
        std::string tooltip = entry.name;
        if (entry.kind == ServerRailModel::Kind::AccountHome) {
            item += "<div class=\"rail-btn\"><img class=\"rail-icon\" src=\"icon:messages-square\"/>";
            if (tooltip.empty()) tooltip = "Direct Messages";
        } else if (entry.kind == ServerRailModel::Kind::Folder) {
            if (isExpanded) currentFolder = entry.id;
            std::string style;
            if (entry.folderColor) {
                const auto color = *entry.folderColor;
                style = std::format(" style=\"background-color: rgba({},{},{},0.3);\"", (color >> 16) & 255, (color >> 8) & 255, color & 255);
            }
            item += std::format("<div class=\"rail-btn folder-btn\"{}>", style);
            if (isExpanded) item += "<img class=\"rail-icon\" src=\"icon:chevron-down\"/>";
            else item += "<div class=\"folder-grid\">" + renderFolderGrid(entry.accountId, entry.id) + "</div>";
            if (tooltip.empty()) tooltip = "Folder";
        } else {
            item += "<div class=\"rail-btn\">";
            const auto *node = entry.node;
            if (node && !node->TEMP_iconHash.empty()) {
                auto src = shell.imageSource(Discord::Cdn::guildIcon(entry.id, node->TEMP_iconHash, 64), Platform::Size{48, 48});
                item += std::format("<img class=\"rail-avatar\" src=\"{}\"/>", escape(src));
            } else item += std::format("<span class=\"rail-monogram\">{}</span>", escape(acronym(entry.name)));
        }
        if (mentions > 0) item += std::format("<div class=\"mention-badge\">{}</div>", mentions > 99 ? "99+" : std::to_string(mentions));
        item += std::format("</div><div class=\"rail-tooltip\">{}</div></div>", escape(tooltip));
        if (entry.kind == ServerRailModel::Kind::Server && entry.inFolderGroup) {
            if (lastFolder != currentFolder) {
                if (!expanded.empty()) expanded += "<div class=\"rail-separator\"></div>";
                lastFolder = currentFolder;
            }
            expanded += item;
        } else {
            if (entry.isFirstOfAccount && row > 0) primary += "<div class=\"rail-separator account-separator\"></div>";
            primary += item;
            if (entry.kind == ServerRailModel::Kind::AccountHome) primary += "<div class=\"rail-separator\"></div>";
        }
        ++row;
    }
    if (auto *doc = shell.document()) doc->SetClass("rail-folder-open", !expanded.empty());
    std::string rml = "<div class=\"rail-primary\">" + primary + "</div>";
    if (!expanded.empty()) rml += "<div class=\"rail-expanded\">" + expanded + "</div>";
    regionRoot->SetInnerRML(rml);
    requestRender(200);
}
} // namespace Styx::UI::Rml
