#pragma once

#include "Controller.hpp"
#include "UI/Rml/EmojiGridModel.hpp"

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QRect>
#include <QString>

#include "Core/Emoji/EmojiManager.hpp"
#include "Core/Emoji/PickerEmoji.hpp"
#include "Core/Gifs/FavoriteGifs.hpp"
#include "Core/Snowflake.hpp"
#include "Proto/UserSettings.hpp"

namespace Rml {
class ElementDocument;
class ElementFormControlInput;
}

namespace Styx {
namespace UI {
namespace Rml {

class ExpressionPickerController final : public Controller
{
    Q_OBJECT
public:
    enum class Tab {
        Gifs,
        Emoji,
    };

    explicit ExpressionPickerController(Shell &shell, QObject *parent = nullptr);
    ~ExpressionPickerController() override;

    void openForChat(Core::EmojiManager *emojiManager,
                     Core::FavoriteGifs *favoriteGifs,
                     Core::Snowflake accountId,
                     Core::Snowflake channelId,
                     Tab tab = Tab::Emoji,
                     const QRect &anchor = {});

    void openForReaction(Core::EmojiManager *emojiManager,
                         Core::Snowflake accountId,
                         Core::Snowflake channelId,
                         Core::Snowflake messageId,
                         const QRect &anchor = {});

    void close();
    [[nodiscard]] bool isOpen() const { return visible; }
    [[nodiscard]] Tab currentTab() const { return activeTab; }

signals:
    void emojiPicked(const Styx::Core::PickerEmoji &emoji, bool keepOpen);
    void gifPicked(const QString &url);
    void closed();

private:
    void ensureDocument();
    void setupEventListeners();
    void showTab(Tab tab);
    void renderRail();
    void renderVisibleGrid();
    void renderGifs();
    void updateInspector(const Core::PickerEmoji *emoji);
    void onSearchChanged(const QString &text);
    void placeCard(const QRect &anchor);
    void pickEmoji(const Core::PickerEmoji &emoji, bool keepOpen);
    void pickGif(const QString &url);

    ::Rml::ElementDocument *pickerDoc = nullptr;
    ::Rml::Element *backdropEl = nullptr;
    ::Rml::Element *cardEl = nullptr;
    ::Rml::Element *tabsEl = nullptr;
    ::Rml::Element *tabGifsBtn = nullptr;
    ::Rml::Element *tabEmojiBtn = nullptr;
    ::Rml::Element *searchInputEl = nullptr;
    ::Rml::Element *searchClearBtn = nullptr;
    ::Rml::Element *emojiViewEl = nullptr;
    ::Rml::Element *railEl = nullptr;
    ::Rml::Element *gridScrollEl = nullptr;
    ::Rml::Element *gifViewEl = nullptr;
    ::Rml::Element *gifScrollEl = nullptr;
    ::Rml::Element *inspectorImgEl = nullptr;
    ::Rml::Element *inspectorNameEl = nullptr;
    ::Rml::Element *inspectorGuildEl = nullptr;

    EmojiGridModel emojiModel;
    QPointer<Core::EmojiManager> emojis;
    QPointer<Core::FavoriteGifs> favoriteGifs;
    Core::Snowflake currentAccountId;
    Core::Snowflake currentChannelId;
    Core::Snowflake reactionMessageId;
    Core::EmojiIntention intention = Core::EmojiIntention::Chat;

    Tab activeTab = Tab::Emoji;
    bool visible = false;
    bool isReaction = false;
    bool searching = false;
    QString searchQuery;

    QList<Core::PickerSection> browseSections;
    QHash<Core::Snowflake, QString> guildNames;
    QList<Proto::FavoriteGif> filteredGifs;

    int lastRenderedScrollY = -999;
    QElapsedTimer openTimer;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
