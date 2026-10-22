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

#include <QGuiApplication>

namespace Styx {
namespace UI {
namespace Rml {

namespace {

QString categoryIconName(const QString &sectionId)
{
    static const QHash<QString, const char *> icons = {
        { QStringLiteral("people"), "smile" },
        { QStringLiteral("nature"), "leaf" },
        { QStringLiteral("food"), "utensils" },
        { QStringLiteral("activity"), "gamepad-2" },
        { QStringLiteral("travel"), "car" },
        { QStringLiteral("objects"), "lightbulb" },
        { QStringLiteral("symbols"), "heart" },
        { QStringLiteral("flags"), "flag" },
    };
    return QString::fromLatin1(icons.value(sectionId, "smile"));
}

} // namespace

ExpressionPickerController::ExpressionPickerController(Shell &shell, QObject *parent)
    : Controller(shell, nullptr, parent)
{
    connect(&emojiModel, &EmojiGridModel::activeEmojiChanged, this, [this]() {
        const Core::PickerEmoji *emoji = emojiModel.activeEmoji();
        updateInspector(emoji);
        renderVisibleGrid();
        requestRender();
    });

    connect(&emojiModel, &EmojiGridModel::topSectionChanged, this, [this](int) {
        renderRail();
        requestRender();
    });

    connect(&emojiModel, &EmojiGridModel::scrollRequested, this, [this](int newY) {
        if (gridScrollEl) {
            gridScrollEl->SetScrollTop(static_cast<float>(newY));
            lastRenderedScrollY = newY;
            renderVisibleGrid();
            requestRender();
        }
    });

    connect(&emojiModel, &EmojiGridModel::emojiClicked, this, [this](const Core::PickerEmoji &emoji, Qt::KeyboardModifiers modifiers) {
        pickEmoji(emoji, modifiers.testFlag(Qt::ShiftModifier));
    });
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

    pickerDoc = view->context()->LoadDocument(":/ui/expression_picker.rml");
    if (!pickerDoc) {
        qCWarning(LogUI) << "ExpressionPickerController: could not load expression_picker.rml";
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
            const QString tabStr = attribute(target, "data-tab");
            if (tabStr == QLatin1String("gifs"))
                showTab(Tab::Gifs);
            else if (tabStr == QLatin1String("emoji"))
                showTab(Tab::Emoji);
        }
    }, tabsEl);

    // Search input
    listen("change", [this](::Rml::Event &) {
        if (auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(searchInputEl))
            onSearchChanged(QString::fromStdString(input->GetValue()));
    }, searchInputEl);

    listen("keyup", [this](::Rml::Event &) {
        if (auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(searchInputEl))
            onSearchChanged(QString::fromStdString(input->GetValue()));
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
            } else if (key == KI::KI_G && !isReaction) {
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
                const int sec = attribute(header, "data-section").toInt();
                emojiModel.toggleCollapsed(sec);
                renderVisibleGrid();
                requestRender();
                return;
            }
        }

        if (auto *cell = closest(event.GetTargetElement(), "data-index")) {
            const int sec = attribute(cell, "data-section").toInt();
            const int idx = attribute(cell, "data-index").toInt();
            const bool shift = (event.GetParameter<int>("shift_key", 0) != 0);
            emojiModel.clickCell({ sec, idx }, shift ? Qt::ShiftModifier : Qt::NoModifier);
        }
    }, gridScrollEl);

    // Emoji Grid mouseover preview
    listen("mouseover", [this](::Rml::Event &event) {
        if (auto *cell = closest(event.GetTargetElement(), "data-index")) {
            const int sec = attribute(cell, "data-section").toInt();
            const int idx = attribute(cell, "data-index").toInt();
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
            const int sec = attribute(btn, "data-rail-sec").toInt();
            emojiModel.scrollToSection(sec);
        }
    }, railEl);

    // GIF Grid clicks
    listen("click", [this](::Rml::Event &event) {
        if (auto *tile = closest(event.GetTargetElement(), "data-gif-index")) {
            const int idx = attribute(tile, "data-gif-index").toInt();
            if (idx >= 0 && idx < filteredGifs.size())
                pickGif(QString::fromStdString(filteredGifs.at(idx).url));
        }
    }, gifScrollEl);
}

void ExpressionPickerController::openForChat(Core::EmojiManager *emojiManager,
                                             Core::FavoriteGifs *favoriteGifs,
                                             Core::Snowflake accountId,
                                             Core::Snowflake channelId,
                                             Tab tab,
                                             const QRect &anchor)
{
    openTimer.restart();
    ensureDocument();
    if (!pickerDoc)
        return;

    emojis = emojiManager;
    this->favoriteGifs = favoriteGifs;
    currentAccountId = accountId;
    currentChannelId = channelId;
    intention = Core::EmojiIntention::Chat;
    isReaction = false;

    if (tabsEl)
        tabsEl->SetProperty("display", "flex");

    browseSections = emojis ? emojis->pickerSections(channelId, intention) : QList<Core::PickerSection>();
    guildNames.clear();
    for (const auto &sec : browseSections) {
        if (sec.guildId.isValid())
            guildNames.insert(sec.guildId, sec.title);
    }

    placeCard(anchor);

    if (auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(searchInputEl)) {
        input->SetValue("");
        input->SetAttribute("placeholder", "Find the perfect emoji");
    }
    if (searchClearBtn)
        searchClearBtn->SetProperty("display", "none");

    searchQuery.clear();
    searching = false;

    showTab(tab);

    pickerDoc->Show();
    pickerDoc->PullToFront();
    if (backdropEl)
        backdropEl->SetClass("open", true);
    visible = true;

    if (searchInputEl)
        searchInputEl->Focus();

    requestRender();
    qCInfo(LogUI) << "ExpressionPicker opened for chat in" << openTimer.elapsed() << "ms";
}

void ExpressionPickerController::openForReaction(Core::EmojiManager *emojiManager,
                                                 Core::Snowflake accountId,
                                                 Core::Snowflake channelId,
                                                 Core::Snowflake messageId,
                                                 const QRect &anchor)
{
    openTimer.restart();
    ensureDocument();
    if (!pickerDoc)
        return;

    emojis = emojiManager;
    currentAccountId = accountId;
    currentChannelId = channelId;
    reactionMessageId = messageId;
    intention = Core::EmojiIntention::Reaction;
    isReaction = true;

    if (tabsEl)
        tabsEl->SetProperty("display", "none");

    browseSections = emojis ? emojis->pickerSections(channelId, intention) : QList<Core::PickerSection>();
    guildNames.clear();
    for (const auto &sec : browseSections) {
        if (sec.guildId.isValid())
            guildNames.insert(sec.guildId, sec.title);
    }

    placeCard(anchor);

    if (auto *input = dynamic_cast<::Rml::ElementFormControlInput *>(searchInputEl)) {
        input->SetValue("");
        input->SetAttribute("placeholder", "Find the perfect reaction");
    }
    if (searchClearBtn)
        searchClearBtn->SetProperty("display", "none");

    searchQuery.clear();
    searching = false;

    showTab(Tab::Emoji);

    pickerDoc->Show();
    pickerDoc->PullToFront();
    if (backdropEl)
        backdropEl->SetClass("open", true);
    visible = true;

    if (searchInputEl)
        searchInputEl->Focus();

    requestRender();
    qCInfo(LogUI) << "ExpressionPicker opened for reaction in" << openTimer.elapsed() << "ms";
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
    emit closed();
}

void ExpressionPickerController::showTab(Tab tab)
{
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

void ExpressionPickerController::onSearchChanged(const QString &text)
{
    searchQuery = text.trimmed();
    if (searchClearBtn)
        searchClearBtn->SetProperty("display", searchQuery.isEmpty() ? "none" : "flex");

    if (activeTab == Tab::Emoji) {
        if (searchQuery.isEmpty() || !emojis) {
            searching = false;
            emojiModel.setSections(browseSections);
            emojiModel.activateFirst();
            renderRail();
        } else {
            searching = true;
            QString query = searchQuery;
            if (query.startsWith(QLatin1Char(':')))
                query.remove(0, 1);
            if (query.endsWith(QLatin1Char(':')))
                query.chop(1);

            const auto results = emojis->searchPicker(query, currentChannelId, intention);
            QList<Core::PickerSection> sections;
            if (!results.isEmpty()) {
                Core::PickerSection sec;
                sec.title = QStringLiteral("Search Results");
                sec.emojis = results;
                sections.append(sec);
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

    QString rml;
    const int topSec = emojiModel.topSection();
    for (int i = 0; i < emojiModel.sections().size(); ++i) {
        const auto *sec = &emojiModel.sections().at(i);
        if (!sec)
            continue;

        const bool isActive = (i == topSec);
        if (sec->id == QLatin1String("recent")) {
            rml += QStringLiteral("<button class=\"rail-btn%1\" data-rail-sec=\"%2\">"
                                  "<img class=\"rail-icon\" src=\"icon:clock\"/>"
                                  "</button>")
                       .arg(isActive ? QStringLiteral(" active") : QString())
                       .arg(i);
        } else if (sec->guildId.isValid()) {
            if (sec->guildIconUrl.isValid()) {
                const QString iconSrc = Runtime::instance().imageSource(sec->guildIconUrl, QSize(24, 24), currentAccountId);
                rml += QStringLiteral("<button class=\"rail-btn%1\" data-rail-sec=\"%2\">"
                                      "<img class=\"rail-guild-img\" src=\"%3\"/>"
                                      "</button>")
                           .arg(isActive ? QStringLiteral(" active") : QString())
                           .arg(i)
                           .arg(iconSrc);
            } else {
                QString initials;
                const auto parts = sec->title.split(QLatin1Char(' '), Qt::SkipEmptyParts);
                for (const auto &p : parts) {
                    if (!p.isEmpty())
                        initials += p[0].toUpper();
                    if (initials.size() == 2)
                        break;
                }
                if (initials.isEmpty())
                    initials = QStringLiteral("?");
                rml += QStringLiteral("<button class=\"rail-btn%1\" data-rail-sec=\"%2\">"
                                      "<div class=\"rail-guild-initials\">%3</div>"
                                      "</button>")
                           .arg(isActive ? QStringLiteral(" active") : QString())
                           .arg(i)
                           .arg(initials);
            }
        } else {
            const QString icon = categoryIconName(sec->id);
            rml += QStringLiteral("<button class=\"rail-btn%1\" data-rail-sec=\"%2\">"
                                  "<img class=\"rail-icon\" src=\"icon:%3\"/>"
                                  "</button>")
                       .arg(isActive ? QStringLiteral(" active") : QString())
                       .arg(i)
                       .arg(icon);
        }
    }
    railEl->SetInnerRML(rml.toStdString());
}

void ExpressionPickerController::renderVisibleGrid()
{
    if (!gridScrollEl)
        return;

    const auto virtualSections = emojiModel.computeVirtualSections(120);
    if (virtualSections.isEmpty()) {
        gridScrollEl->SetInnerRML("<div class=\"empty-results\">No emoji match your search</div>");
        return;
    }

    QString rml;
    for (const auto &vs : virtualSections) {
        rml += QStringLiteral("<div class=\"emoji-section\" data-section=\"%1\">").arg(vs.sectionIndex);
        if (!vs.title.isEmpty()) {
            rml += QStringLiteral("<div class=\"emoji-section-header\" data-section=\"%1\">"
                                  "<span class=\"emoji-section-title\">%2</span>"
                                  "</div>")
                       .arg(vs.sectionIndex)
                       .arg(Rml::escape(vs.title));
        }

        if (vs.topSpacer > 0)
            rml += QStringLiteral("<div style=\"height: %1dp;\"></div>").arg(vs.topSpacer);

        const auto *sec = emojiModel.section(vs.sectionIndex);
        if (sec) {
            for (const auto &vr : vs.visibleRows) {
                rml += QStringLiteral("<div class=\"emoji-row\">");
                for (int col = 0; col < vr.emojiCount; ++col) {
                    const int emojiIdx = vr.firstEmojiIndex + col;
                    const auto &emoji = sec->emojis.at(emojiIdx);
                    const bool isActive = (emojiModel.activeCell().section == vs.sectionIndex &&
                                           emojiModel.activeCell().index == emojiIdx);

                    QString src;
                    if (emoji.isCustom())
                        src = Runtime::instance().imageSource(Discord::Cdn::emoji(emoji.customId, 64, false), QSize(32, 32), currentAccountId);
                    else
                        src = Runtime::emojiSource(emoji.surrogates);

                    rml += QStringLiteral("<div class=\"emoji-cell%1\" data-section=\"%2\" data-index=\"%3\">"
                                          "<img class=\"emoji-img\" src=\"%4\"/>"
                                          "</div>")
                               .arg(isActive ? QStringLiteral(" active") : QString())
                               .arg(vs.sectionIndex)
                               .arg(emojiIdx)
                               .arg(src);
                }
                rml += QStringLiteral("</div>");
            }
        }

        if (vs.bottomSpacer > 0)
            rml += QStringLiteral("<div style=\"height: %1dp;\"></div>").arg(vs.bottomSpacer);

        rml += QStringLiteral("</div>");
    }

    gridScrollEl->SetInnerRML(rml.toStdString());
}

void ExpressionPickerController::renderGifs()
{
    if (!gifScrollEl)
        return;

    const QList<Proto::FavoriteGif> allGifs = favoriteGifs ? favoriteGifs->newestFirst() : QList<Proto::FavoriteGif>();
    filteredGifs.clear();
    const QString query = searchQuery.trimmed();
    for (const auto &gif : allGifs) {
        if (query.isEmpty() || Core::FavoriteGifRules::matchesSearch(QString::fromStdString(gif.url), query))
            filteredGifs.append(gif);
    }

    if (filteredGifs.isEmpty()) {
        gifScrollEl->SetInnerRML("<div class=\"empty-results\">No favorite GIFs found</div>");
        return;
    }

    QList<QSize> sizes;
    sizes.reserve(filteredGifs.size());
    for (const auto &gif : filteredGifs)
        sizes.append(QSize(static_cast<int>(gif.width), static_cast<int>(gif.height)));

    constexpr int ViewportWidth = 420;
    constexpr int Columns = 2;
    const auto arrangement = GifMasonry::arrange(sizes, ViewportWidth, Columns);

    QString rml;
    rml += QStringLiteral("<div class=\"gif-container\" style=\"height: %1dp;\">").arg(arrangement.contentHeight);
    for (int i = 0; i < filteredGifs.size() && i < arrangement.tiles.size(); ++i) {
        const auto &gif = filteredGifs.at(i);
        const QRect &tile = arrangement.tiles.at(i);
        const QUrl stillUrl = Core::FavoriteGifRules::stillServedByDiscord(gif);
        const QString src = stillUrl.isValid() ? Runtime::instance().imageSource(stillUrl, tile.size(), currentAccountId)
                                              : QStringLiteral(":/resources/placeholder.png");

        rml += QStringLiteral("<div class=\"gif-tile\" data-gif-index=\"%1\" style=\"left: %2dp; top: %3dp; width: %4dp; height: %5dp;\">"
                              "<img class=\"gif-img\" src=\"%6\"/>"
                              "</div>")
                   .arg(i)
                   .arg(tile.x())
                   .arg(tile.y())
                   .arg(tile.width())
                   .arg(tile.height())
                   .arg(src);
    }
    rml += QStringLiteral("</div>");
    gifScrollEl->SetInnerRML(rml.toStdString());
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
    QString src;
    if (emoji->isCustom())
        src = Runtime::instance().imageSource(Discord::Cdn::emoji(emoji->customId, 64, false), QSize(28, 28), currentAccountId);
    else
        src = Runtime::emojiSource(emoji->surrogates);

    inspectorImgEl->SetAttribute("src", src.toStdString());
    inspectorNameEl->SetInnerRML(Rml::escape(":" + emoji->name + ":").toStdString());

    if (emoji->isCustom() && guildNames.contains(emoji->guildId))
        inspectorGuildEl->SetInnerRML(Rml::escape("from " + guildNames.value(emoji->guildId)).toStdString());
    else
        inspectorGuildEl->SetInnerRML("");
}

void ExpressionPickerController::placeCard(const QRect &anchor)
{
    if (!cardEl)
        return;

    if (isReaction && anchor.isValid() && anchor.width() > 0) {
        const int x = std::clamp(anchor.right() - 440, 20, 1200);
        const int y = std::clamp(anchor.top() - 480 - 8, 20, 700);
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
    emit emojiPicked(emoji, keepOpen);
    if (!keepOpen)
        close();
}

void ExpressionPickerController::pickGif(const QString &url)
{
    emit gifPicked(url);
    close();
}

} // namespace Rml
} // namespace UI
} // namespace Styx
