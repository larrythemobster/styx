#pragma once

#include "Controller.hpp"
#include "UI/Rml/EmojiGridModel.hpp"

#include <chrono>
#include <string>
#include <vector>
#include <unordered_map>
#include "Platform/Native.hpp"
#include "Platform/Trackable.hpp"

#include "Core/Emoji/EmojiManager.hpp"
#include "Core/Emoji/PickerEmoji.hpp"
#include "Core/Gifs/FavoriteGifs.hpp"
#include "Core/Snowflake.hpp"

namespace Rml {
class ElementDocument;
class ElementFormControlInput;
}

namespace Styx {
namespace UI {
namespace Rml {

class ExpressionPickerController final : public Controller
{
public:
    enum class Tab {
        Gifs,
        Emoji,
    };

    explicit ExpressionPickerController(Shell &shell);
    ~ExpressionPickerController() override;

    void openForChat(Core::EmojiManager *emojiManager,
                     Core::FavoriteGifs *favoriteGifs,
                     Core::Snowflake accountId,
                     Core::Snowflake channelId,
                     Tab tab = Tab::Emoji,
                     const Platform::Rect &anchor = {});

    void openForReaction(Core::EmojiManager *emojiManager,
                         Core::Snowflake accountId,
                         Core::Snowflake channelId,
                         Core::Snowflake messageId,
                         const Platform::Rect &anchor = {});

    void close();
    [[nodiscard]] bool isOpen() const { return visible; }
    [[nodiscard]] Tab currentTab() const { return activeTab; }

public:
    Platform::Signal<const Core::PickerEmoji &, bool> emojiPicked;
    Platform::Signal<const std::string &> gifPicked;
    Platform::Signal<> closed;

private:
    void ensureDocument();
    void setupEventListeners();
    void showTab(Tab tab);
    void renderRail();
    void renderVisibleGrid();
    void renderGifs();
    void updateInspector(const Core::PickerEmoji *emoji);
    void onSearchChanged(const std::string &text);
    void placeCard(const Platform::Rect &anchor);
    void pickEmoji(const Core::PickerEmoji &emoji, bool keepOpen);
    void pickGif(const std::string &url);

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
    Platform::Guard<Core::EmojiManager> emojis;
    Platform::Guard<Core::FavoriteGifs> favoriteGifs;
    Core::Snowflake currentAccountId;
    Core::Snowflake currentChannelId;
    Core::Snowflake reactionMessageId;
    Core::EmojiIntention intention = Core::EmojiIntention::Chat;

    Tab activeTab = Tab::Emoji;
    bool visible = false;
    bool isReaction = false;
    bool searching = false;
    std::string searchQuery;

    std::vector<Core::PickerSection> browseSections;
    std::unordered_map<Core::Snowflake, std::string> guildNames;
    std::vector<Proto::FavoriteGif> filteredGifs;

    int lastRenderedScrollY = -999;
    std::chrono::steady_clock::time_point openTimer;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
