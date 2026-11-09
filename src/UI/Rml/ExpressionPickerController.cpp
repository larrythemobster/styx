#include "UI/Rml/ExpressionPickerController.hpp"

#include "Shell.hpp"
#include "RmlRuntime.hpp"
#include "RmlView.hpp"
#include "Core/Gifs/FavoriteGifRules.hpp"
#include "Core/Logging.hpp"
#include "Discord/CdnUrls.hpp"
#include "UI/Gifs/GifMasonryLayout.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

#include "Platform/Utf8.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <unordered_map>

namespace Styx {
namespace UI {
namespace Rml {

namespace {

std::string categoryIconName(const std::string &sectionId)
{
    static const std::unordered_map<std::string, std::string> icons = {
        {"people", "smile"}, {"nature", "leaf"}, {"food", "utensils"},
        {"activity", "gamepad-2"}, {"travel", "car"}, {"objects", "lightbulb"},
        {"symbols", "heart"}, {"flags", "flag"},
    };
    const auto it = icons.find(sectionId);
    return it == icons.end() ? "smile" : it->second;
}

} // namespace

ExpressionPickerController::ExpressionPickerController(Shell &shell)
    : Controller(shell, nullptr)
{
    track(emojiModel.activeEmojiChanged.connect([this]() {
        const Core::PickerEmoji *emoji = emojiModel.activeEmoji();
        updateInspector(emoji);
        renderVisibleGrid();
        requestRender();
    }));

    track(emojiModel.topSectionChanged.connect([this](int) {
        renderRail();
        requestRender();
    }));

    track(emojiModel.scrollRequested.connect([this](int newY) {
        if (gridScrollEl) {
            gridScrollEl->SetScrollTop(static_cast<float>(newY));
            lastRenderedScrollY = newY;
            renderVisibleGrid();
            requestRender();
        }
    }));

    track(emojiModel.emojiClicked.connect([this](const Core::PickerEmoji &emoji, bool keepOpen) {
        pickEmoji(emoji, keepOpen);
    }));
}

ExpressionPickerController::~ExpressionPickerController()
{
    if (pickerDoc) {
        pickerDoc->Close();
        pickerDoc = nullptr;
    }
}

void ExpressionPickerController::ensureDocument()
{
    if (pickerDoc)
        return;

    auto *view = shell.view();
    if (!view || !view->context())
        return;

    pickerDoc = view->context()->LoadDocument("ui/expression_picker.rml");
    if (!pickerDoc) {
        Core::log(Core::LogLevel::Warning, LogUI) << "ExpressionPickerController: could not load expression_picker.rml";
        return;
    }

    regionRoot = pickerDoc;

    backdropEl = pickerDoc->GetElementById("picker-backdrop");
    cardEl = pickerDoc->GetElementById("picker-card");
    tabsEl = pickerDoc->GetElementById("picker-tabs");
    tabGifsBtn = pickerDoc->GetElementById("tab-gifs");
    tabEmojiBtn = pickerDoc->GetElementById("tab-emoji");
    searchInputEl = pickerDoc->GetElementById("picker-search-input");
    searchClearBtn = pickerDoc->GetElementById("picker-search-clear");
    emojiViewEl = pickerDoc->GetElementById("emoji-view");
    railEl = pickerDoc->GetElementById("emoji-rail");
    gridScrollEl = pickerDoc->GetElementById("emoji-grid-scroll");
    gifViewEl = pickerDoc->GetElementById("gif-view");
    gifScrollEl = pickerDoc->GetElementById("gif-grid-scroll");
    inspectorImgEl = pickerDoc->GetElementById("inspector-img");
    inspectorNameEl = pickerDoc->GetElementById("inspector-name");
    inspectorGuildEl = pickerDoc->GetElementById("inspector-guild");

    setupEventListeners();
}

void ExpressionPickerController::setupEventListeners()
{
    if (!pickerDoc)
        return;

    // Backdrop click outside card closes popover
    listen("click", [this](::Rml::Event &event) {
        if (event.GetTargetElement() == backdropEl)
            close();
    }, backdropEl);

    // Tab buttons
    listen("click", [this](::Rml::Event &event) {
        if (auto *target = closest(event.GetTargetElement(), "data-tab")) {
            const std::string tabStr = attribute(target, "data-tab");
            if (tabStr == "gifs")
                showTab(Tab::Gifs);
            else if (tabStr == "emoji")
                showTab(Tab::Emoji);
        }
    }, tabsEl);

    // Search input
    listen("change", [this](::Rml::Event &) {
        if (auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(searchInputEl))
            onSearchChanged(input->GetValue());
    }, searchInputEl);

    listen("keyup", [this](::Rml::Event &) {
        if (auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(searchInputEl))
            onSearchChanged(input->GetValue());
    }, searchInputEl);

    // Search clear button
    listen("click", [this](::Rml::Event &) {
        if (auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(searchInputEl))
            input->SetValue("");
        onSearchChanged({});
        if (searchInputEl)
            searchInputEl->Focus();
    }, searchClearBtn);

    // Keydown for search & navigation
    listen("keydown", [this](::Rml::Event &event) {
        using KI = ::Rml::Input::KeyIdentifier;
        const auto key = static_cast<KI>(event.GetParameter<int>("key_identifier", 0));
        const bool ctrl = (event.GetParameter<int>("ctrl_key", 0) != 0);
        const bool shift = (event.GetParameter<int>("shift_key", 0) != 0);

        if (key == KI::KI_ESCAPE) {
            close();
            event.StopPropagation();
            return;
        }

        if (ctrl) {
            if (key == KI::KI_E) {
                showTab(Tab::Emoji);
                event.StopPropagation();
                return;
            } else if (key == KI::KI_G && mode == Mode::Chat) {
                showTab(Tab::Gifs);
                event.StopPropagation();
                return;
            }
        }

        if (activeTab == Tab::Emoji) {
            switch (key) {
            case KI::KI_UP:
                emojiModel.moveActive(0, -1);
                event.StopPropagation();
                return;
            case KI::KI_DOWN:
                emojiModel.moveActive(0, 1);
                event.StopPropagation();
                return;
            case KI::KI_LEFT:
                if (!emojiModel.activeIsFirst()) {
                    emojiModel.moveActive(-1, 0);
                    event.StopPropagation();
                }
                return;
            case KI::KI_RIGHT:
                emojiModel.moveActive(1, 0);
                event.StopPropagation();
                return;
            case KI::KI_RETURN:
            case KI::KI_NUMPADENTER:
                if (const auto *emoji = emojiModel.activeEmoji()) {
                    pickEmoji(*emoji, shift);
                    event.StopPropagation();
                }
                return;
            default:
                break;
            }
        }
    }, cardEl);

    // Emoji Grid clicks & header collapses
    listen("click", [this](::Rml::Event &event) {
        if (auto *header = closest(event.GetTargetElement(), "data-section")) {
            if (header->GetClassNames().find("emoji-section-header") != ::Rml::String::npos) {
                const int sec = std::stoi(attribute(header, "data-section"));
                emojiModel.toggleCollapsed(sec);
                renderVisibleGrid();
                requestRender();
                return;
            }
        }

        if (auto *cell = closest(event.GetTargetElement(), "data-index")) {
            const int sec = std::stoi(attribute(cell, "data-section"));
            const int idx = std::stoi(attribute(cell, "data-index"));
            const bool shift = (event.GetParameter<int>("shift_key", 0) != 0);
            emojiModel.clickCell({ sec, idx }, shift);
        }
    }, gridScrollEl);

    // Emoji Grid mouseover preview
    listen("mouseover", [this](::Rml::Event &event) {
        if (auto *cell = closest(event.GetTargetElement(), "data-index")) {
            const int sec = std::stoi(attribute(cell, "data-section"));
            const int idx = std::stoi(attribute(cell, "data-index"));
            if (const auto *emoji = emojiModel.emojiAt({ sec, idx }))
                updateInspector(emoji);
        }
    }, gridScrollEl);

    // Emoji Grid scroll virtualization
    listen("scroll", [this](::Rml::Event &) {
        if (!gridScrollEl)
            return;
        const int newY = static_cast<int>(gridScrollEl->GetScrollTop());
        emojiModel.setScrollY(newY);
        if (std::abs(newY - lastRenderedScrollY) >= 32) {
            lastRenderedScrollY = newY;
            renderVisibleGrid();
            requestRender();
        }
    }, gridScrollEl);

    // Category rail clicks
    listen("click", [this](::Rml::Event &event) {
        if (auto *btn = closest(event.GetTargetElement(), "data-rail-sec")) {
            if (searching) {
                if (auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(searchInputEl))
                    input->SetValue("");
                onSearchChanged({});
            }
            const int sec = std::stoi(attribute(btn, "data-rail-sec"));
            emojiModel.scrollToSection(sec);
        }
    }, railEl);

    // GIF Grid clicks
    listen("click", [this](::Rml::Event &event) {
        if (auto *tile = closest(event.GetTargetElement(), "data-gif-index")) {
            const int idx = std::stoi(attribute(tile, "data-gif-index"));
            if (idx >= 0 && idx < filteredGifs.size())
                pickGif(filteredGifs.at(idx).url);
        }
    }, gifScrollEl);
}

void ExpressionPickerController::openForChat(Core::EmojiManager *emojiManager,
                                             Core::FavoriteGifs *favoriteGifs,
                                             Core::Snowflake accountId,
                                             Core::Snowflake channelId,
                                             Tab tab,
                                             const Platform::Rect &anchor)
{
    openPicker(emojiManager, favoriteGifs, accountId, channelId, Core::Snowflake::Invalid, Mode::Chat, tab, anchor);
}

void ExpressionPickerController::openForReaction(Core::EmojiManager *emojiManager,
                                                 Core::Snowflake accountId,
                                                 Core::Snowflake channelId,
                                                 Core::Snowflake messageId,
                                                 const Platform::Rect &anchor)
{
    openPicker(emojiManager, nullptr, accountId, channelId, messageId, Mode::Reaction, Tab::Emoji, anchor);
}

void ExpressionPickerController::openForStatus(Core::EmojiManager *emojiManager,
                                               Core::Snowflake accountId,
                                               const Platform::Rect &anchor)
{
    openPicker(emojiManager, nullptr, accountId, Core::Snowflake::Invalid, Core::Snowflake::Invalid, Mode::Status, Tab::Emoji, anchor);
}

void ExpressionPickerController::openPicker(Core::EmojiManager *emojiManager,
                                            Core::FavoriteGifs *favoriteGifs,
                                            Core::Snowflake accountId,
                                            Core::Snowflake channelId,
                                            Core::Snowflake messageId,
                                            Mode mode,
                                            Tab tab,
                                            const Platform::Rect &anchor)
{
    openTimer = std::chrono::steady_clock::now();
    ensureDocument();
    if (!pickerDoc)
        return;

    this->mode = mode;
    emojis = emojiManager;
    this->favoriteGifs = favoriteGifs;
    currentAccountId = accountId;
    currentChannelId = channelId;
    reactionMessageId = messageId;
    intention = (mode == Mode::Reaction) ? Core::EmojiIntention::Reaction : Core::EmojiIntention::Chat;

    browseSections = emojis ? emojis->pickerSections(channelId, intention) : std::vector<Core::PickerSection>();
    guildNames.clear();
    for (const auto &sec : browseSections) {
        if (sec.guildId.isValid())
            guildNames[sec.guildId] = sec.title;
    }

    placeCard(anchor);

    const char *placeholder = "Find the perfect emoji";
    if (mode == Mode::Reaction)
        placeholder = "Find the perfect reaction";
    else if (mode == Mode::Status)
        placeholder = "Choose a status emoji";

    if (auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(searchInputEl)) {
        input->SetValue("");
        input->SetAttribute("placeholder", placeholder);
    }
    if (searchClearBtn)
        searchClearBtn->SetProperty("display", "none");

    searchQuery.clear();
    searching = false;

    if (tabsEl)
        tabsEl->SetProperty("display", mode == Mode::Chat ? "flex" : "none");

    showTab(tab);

    pickerDoc->Show();
    pickerDoc->PullToFront();
    if (backdropEl)
        backdropEl->SetClass("open", true);
    visible = true;

    if (searchInputEl)
        searchInputEl->Focus();

    requestRender();
    const char *modeLabel = (mode == Mode::Reaction) ? "reaction" : (mode == Mode::Status) ? "status" : "chat";
    Core::log(Core::LogLevel::Info, LogUI) << "ExpressionPicker opened for" << modeLabel << "in"
                                           << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - openTimer).count()
                                           << "ms";
}

void ExpressionPickerController::close()
{
    if (!visible)
        return;

    visible = false;
    if (backdropEl)
        backdropEl->SetClass("open", false);
    if (pickerDoc)
        pickerDoc->Hide();

    requestRender();
    closed();
}

void ExpressionPickerController::showTab(Tab tab)
{
    if (mode != Mode::Chat)
        tab = Tab::Emoji;

    activeTab = tab;
    if (tabEmojiBtn)
        tabEmojiBtn->SetClass("active", tab == Tab::Emoji);
    if (tabGifsBtn)
        tabGifsBtn->SetClass("active", tab == Tab::Gifs);

    if (tab == Tab::Emoji) {
        if (emojiViewEl)
            emojiViewEl->SetProperty("display", "flex");
        if (gifViewEl)
            gifViewEl->SetProperty("display", "none");

        emojiModel.setSections(browseSections);
        emojiModel.activateFirst();
        lastRenderedScrollY = 0;
        if (gridScrollEl)
            gridScrollEl->SetScrollTop(0);

        renderRail();
        renderVisibleGrid();
        updateInspector(emojiModel.activeEmoji());
    } else {
        if (emojiViewEl)
            emojiViewEl->SetProperty("display", "none");
        if (gifViewEl)
            gifViewEl->SetProperty("display", "flex");

        if (gifScrollEl)
            gifScrollEl->SetScrollTop(0);
        renderGifs();
        updateInspector(nullptr);
    }

    requestRender();
}

void ExpressionPickerController::onSearchChanged(const std::string &text)
{
    searchQuery = Platform::trim(text);
    if (searchClearBtn)
        searchClearBtn->SetProperty("display", searchQuery.empty() ? "none" : "flex");

    if (activeTab == Tab::Emoji) {
        if (searchQuery.empty() || !emojis) {
            searching = false;
            emojiModel.setSections(browseSections);
            emojiModel.activateFirst();
            renderRail();
        } else {
            searching = true;
            std::string query = searchQuery;
            if (query.starts_with(':'))
                query.erase(0, 1);
            if (query.ends_with(':'))
                query.pop_back();

            const auto results = emojis->searchPicker(query, currentChannelId, intention);
            std::vector<Core::PickerSection> sections;
            if (!results.empty()) {
                Core::PickerSection sec;
                sec.title = "Search Results";
                sec.emojis = results;
                sections.push_back(sec);
            }
            emojiModel.setSections(sections);
            emojiModel.activateFirst();
            if (railEl)
                railEl->SetInnerRML("");
        }
        lastRenderedScrollY = 0;
        if (gridScrollEl)
            gridScrollEl->SetScrollTop(0);
        renderVisibleGrid();
        updateInspector(emojiModel.activeEmoji());
    } else {
        renderGifs();
    }

    requestRender();
}

void ExpressionPickerController::renderRail()
{
    if (!railEl)
        return;
    std::string rml;
    const int topSec = emojiModel.topSection();
    const auto &sections = emojiModel.sections();
    for (int i = 0; i < static_cast<int>(sections.size()); ++i) {
        const auto &sec = sections[i];
        const char *active = i == topSec ? " active" : "";
        if (sec.id == "recent") {
            rml += std::format("<button class=\"rail-btn{}\" data-rail-sec=\"{}\"><img class=\"rail-icon\" src=\"icon:clock\"/></button>", active, i);
        } else if (sec.guildId.isValid()) {
            if (!sec.guildIconUrl.empty()) {
                const auto src = Runtime::instance().imageSource(sec.guildIconUrl, {24, 24}, currentAccountId);
                rml += std::format("<button class=\"rail-btn{}\" data-rail-sec=\"{}\"><img class=\"rail-guild-img\" src=\"{}\"/></button>", active, i, Rml::escape(src));
            } else {
                std::string initials;
                int count = 0;
                for (auto part : Platform::split(sec.title, ' ', true)) {
                    const auto points = Platform::codepoints(part);
                    if (!points.empty()) {
                        initials += Platform::unicodeUpper(Platform::fromCodepoints({points.front()}));
                        if (++count == 2) break;
                    }
                }
                if (initials.empty()) initials = "?";
                rml += std::format("<button class=\"rail-btn{}\" data-rail-sec=\"{}\"><div class=\"rail-guild-initials\">{}</div></button>", active, i, Rml::escape(initials));
            }
        } else {
            rml += std::format("<button class=\"rail-btn{}\" data-rail-sec=\"{}\"><img class=\"rail-icon\" src=\"icon:{}\"/></button>", active, i, categoryIconName(sec.id));
        }
    }
    railEl->SetInnerRML(rml);
}

void ExpressionPickerController::renderVisibleGrid()
{
    if (!gridScrollEl) return;
    const auto virtualSections = emojiModel.computeVirtualSections(120);
    if (virtualSections.empty()) {
        gridScrollEl->SetInnerRML("<div class=\"empty-results\">No emoji match your search</div>");
        return;
    }
    std::string rml;
    for (const auto &vs : virtualSections) {
        rml += std::format("<div class=\"emoji-section\" data-section=\"{}\">", vs.sectionIndex);
        if (!vs.title.empty())
            rml += std::format("<div class=\"emoji-section-header\" data-section=\"{}\"><span class=\"emoji-section-title\">{}</span></div>", vs.sectionIndex, Rml::escape(vs.title));
        if (vs.topSpacer > 0) rml += std::format("<div style=\"height: {}dp;\"></div>", vs.topSpacer);
        if (const auto *sec = emojiModel.section(vs.sectionIndex)) {
            for (const auto &vr : vs.visibleRows) {
                rml += "<div class=\"emoji-row\">";
                for (int col = 0; col < vr.emojiCount; ++col) {
                    const int index = vr.firstEmojiIndex + col;
                    const auto &emoji = sec->emojis.at(index);
                    const bool active = emojiModel.activeCell().section == vs.sectionIndex && emojiModel.activeCell().index == index;
                    const auto src = emoji.isCustom() ? Runtime::instance().imageSource(Discord::Cdn::emoji(emoji.customId, 64, false), {32, 32}, currentAccountId) : Runtime::emojiSource(emoji.surrogates);
                    rml += std::format("<div class=\"emoji-cell{}\" data-section=\"{}\" data-index=\"{}\"><img class=\"emoji-img\" src=\"{}\"/></div>", active ? " active" : "", vs.sectionIndex, index, Rml::escape(src));
                }
                rml += "</div>";
            }
        }
        if (vs.bottomSpacer > 0) rml += std::format("<div style=\"height: {}dp;\"></div>", vs.bottomSpacer);
        rml += "</div>";
    }
    gridScrollEl->SetInnerRML(rml);
}

void ExpressionPickerController::renderGifs()
{
    if (!gifScrollEl) return;
    const auto allGifs = favoriteGifs ? favoriteGifs->newestFirst() : std::vector<Proto::FavoriteGif>();
    filteredGifs.clear();
    const std::string query(Platform::trim(searchQuery));
    for (const auto &gif : allGifs)
        if (query.empty() || Core::FavoriteGifRules::matchesSearch(gif.url, query)) filteredGifs.push_back(gif);
    if (filteredGifs.empty()) {
        gifScrollEl->SetInnerRML("<div class=\"empty-results\">No favorite GIFs found</div>");
        return;
    }
    std::vector<Platform::Size> sizes;
    sizes.reserve(filteredGifs.size());
    for (const auto &gif : filteredGifs) sizes.push_back({static_cast<int>(gif.width), static_cast<int>(gif.height)});
    const auto arrangement = GifMasonry::arrange(sizes, 420, 2);
    std::string rml = std::format("<div class=\"gif-container\" style=\"height: {}dp;\">", arrangement.contentHeight);
    for (size_t i = 0; i < filteredGifs.size() && i < arrangement.tiles.size(); ++i) {
        const auto &tile = arrangement.tiles[i];
        const auto stillUrl = Core::FavoriteGifRules::stillServedByDiscord(filteredGifs[i]);
        const auto src = !stillUrl.empty() ? Runtime::instance().imageSource(stillUrl, {static_cast<int>(tile.width), static_cast<int>(tile.height)}, currentAccountId) : "placeholder.png";
        rml += std::format("<div class=\"gif-tile\" data-gif-index=\"{}\" style=\"left: {}dp; top: {}dp; width: {}dp; height: {}dp;\"><img class=\"gif-img\" src=\"{}\"/></div>", i, tile.x, tile.y, tile.width, tile.height, Rml::escape(src));
    }
    rml += "</div>";
    gifScrollEl->SetInnerRML(rml);
}

void ExpressionPickerController::updateInspector(const Core::PickerEmoji *emoji)
{
    if (!inspectorImgEl || !inspectorNameEl || !inspectorGuildEl)
        return;

    if (!emoji) {
        inspectorImgEl->SetProperty("display", "none");
        inspectorNameEl->SetInnerRML("");
        inspectorGuildEl->SetInnerRML("");
        return;
    }

    inspectorImgEl->SetProperty("display", "inline-block");
    std::string src;
    if (emoji->isCustom())
        src = Runtime::instance().imageSource(Discord::Cdn::emoji(emoji->customId, 64, false), Platform::Size{28, 28}, currentAccountId);
    else
        src = Runtime::emojiSource(emoji->surrogates);

    inspectorImgEl->SetAttribute("src", src);
    inspectorNameEl->SetInnerRML(Rml::escape(":" + emoji->name + ":"));

    if (emoji->isCustom() && guildNames.contains(emoji->guildId))
        inspectorGuildEl->SetInnerRML(Rml::escape("from " + guildNames.at(emoji->guildId)));
    else
        inspectorGuildEl->SetInnerRML("");
}

void ExpressionPickerController::placeCard(const Platform::Rect &anchor)
{
    if (!cardEl)
        return;

    if (mode != Mode::Chat && !anchor.empty()) {
        const int x = std::clamp(static_cast<int>(anchor.x + anchor.width - 1 - 440), 20, 1200);
        const int y = std::clamp(static_cast<int>(anchor.y - 480 - 8), 20, 700);
        cardEl->SetProperty("left", ::Rml::String(std::to_string(x) + "dp"));
        cardEl->SetProperty("top", ::Rml::String(std::to_string(y) + "dp"));
        cardEl->RemoveProperty("right");
        cardEl->RemoveProperty("bottom");
    } else {
        cardEl->SetProperty("right", "16dp");
        cardEl->SetProperty("bottom", "76dp");
        cardEl->RemoveProperty("left");
        cardEl->RemoveProperty("top");
    }
}

void ExpressionPickerController::pickEmoji(const Core::PickerEmoji &emoji, bool keepOpen)
{
    emojiPicked(emoji, keepOpen);
    if (!keepOpen)
        close();
}

void ExpressionPickerController::pickGif(const std::string &url)
{
    gifPicked(url);
    close();
}

} // namespace Rml
} // namespace UI
} // namespace Styx
