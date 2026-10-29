#include "UI/Rml/EmojiGridModel.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace Styx;
using UI::Rml::EmojiGridModel;

namespace {
Core::PickerSection makeSection(const std::string &id, const std::string &title, int count)
{
    Core::PickerSection section;
    section.id = id;
    section.title = title;
    for (int i = 0; i < count; ++i) {
        Core::PickerEmoji emoji;
        emoji.name = id + std::to_string(i);
        emoji.surrogates = "\xF0\x9F\x98\x80";
        section.emojis.push_back(emoji);
    }
    return section;
}

struct GridFixture {
    EmojiGridModel grid;

    GridFixture()
    {
        grid.setViewportHeight(360);
        grid.setSections({makeSection("a", "First", 11), makeSection("b", "Second", 3), makeSection("c", "Third", 200)});
    }

    std::string activeName() const
    {
        const auto *emoji = grid.activeEmoji();
        return emoji ? emoji->name : std::string{};
    }
};
}

TEST_CASE_METHOD(GridFixture, "Emoji grid first cell becomes active")
{
    REQUIRE(EmojiGridModel::Columns == 9);
    REQUIRE(grid.activeEmoji() == nullptr);
    int activeChanges = 0;
    auto connection = grid.activeEmojiChanged.connect([&] { ++activeChanges; });
    grid.activateFirst();
    REQUIRE(activeName() == "a0");
    REQUIRE(grid.activeIsFirst());
    REQUIRE(activeChanges == 1);
    grid.moveActive(1, 0);
    REQUIRE_FALSE(grid.activeIsFirst());
}

TEST_CASE_METHOD(GridFixture, "Emoji grid horizontal moves wrap across rows and sections")
{
    grid.activateFirst();
    for (int i = 0; i < 8; ++i)
        grid.moveActive(1, 0);
    REQUIRE(activeName() == "a8");
    grid.moveActive(1, 0);
    REQUIRE(activeName() == "a9");
    grid.moveActive(1, 0);
    grid.moveActive(1, 0);
    REQUIRE(activeName() == "b0");
    grid.moveActive(-1, 0);
    REQUIRE(activeName() == "a10");
}

TEST_CASE_METHOD(GridFixture, "Emoji grid vertical moves clamp to shorter rows")
{
    grid.activateFirst();
    for (int i = 0; i < 4; ++i)
        grid.moveActive(1, 0);
    REQUIRE(activeName() == "a4");
    grid.moveActive(0, 1);
    REQUIRE(activeName() == "a10");
    grid.moveActive(0, 1);
    REQUIRE(activeName() == "b1");
    grid.moveActive(0, 1);
    REQUIRE(activeName() == "c1");
    grid.moveActive(0, -1);
    REQUIRE(activeName() == "b1");
    grid.moveActive(0, -1);
    REQUIRE(activeName() == "a10");
    grid.moveActive(0, -1);
    REQUIRE(activeName() == "a1");
}

TEST_CASE_METHOD(GridFixture, "Emoji grid moves stop at the edges")
{
    grid.activateFirst();
    grid.moveActive(-1, 0);
    REQUIRE(activeName() == "a0");
    grid.moveActive(0, -1);
    REQUIRE(activeName() == "a0");
    grid.setSections({makeSection("a", "Only", 2)});
    grid.activateFirst();
    grid.moveActive(1, 0);
    grid.moveActive(1, 0);
    REQUIRE(activeName() == "a1");
    grid.moveActive(0, 1);
    REQUIRE(activeName() == "a1");
}

TEST_CASE_METHOD(GridFixture, "Emoji grid keyboard selection scrolls into view")
{
    grid.activateFirst();
    REQUIRE(grid.scrollY() == 0);
    constexpr int RowsDown = 20;
    for (int i = 0; i < RowsDown; ++i)
        grid.moveActive(0, 1);
    REQUIRE(activeName() == "c" + std::to_string((RowsDown - 3) * EmojiGridModel::Columns));
    REQUIRE(grid.scrollY() > 0);
    REQUIRE(grid.topSection() == 2);
}

TEST_CASE_METHOD(GridFixture, "Emoji grid click reports emoji and keep-open modifier")
{
    grid.setSections({makeSection("a", "", 20)});
    std::vector<std::string> clickedNames;
    bool keepOpen = false;
    auto connection = grid.emojiClicked.connect([&](const Core::PickerEmoji &emoji, bool modifier) {
        clickedNames.push_back(emoji.name);
        keepOpen = modifier;
    });
    const Platform::Point secondRowFirstCell{EmojiGridModel::CellPx - 1, EmojiGridModel::CellPx + EmojiGridModel::CellPx / 2};
    grid.mouseClick(secondRowFirstCell, true);
    REQUIRE((clickedNames == std::vector<std::string>{"a9"}));
    REQUIRE(keepOpen);
}

TEST_CASE_METHOD(GridFixture, "Emoji grid header click collapses section")
{
    int changes = 0;
    auto connection = grid.collapsedSectionsChanged.connect([&] { ++changes; });
    const Platform::Point firstHeader{EmojiGridModel::CellPx, 4};
    grid.mouseClick(firstHeader);
    REQUIRE(changes == 1);
    REQUIRE(grid.collapsedSections().contains("a"));
    grid.activateFirst();
    REQUIRE(activeName() == "b0");
    grid.moveActive(-1, 0);
    REQUIRE(activeName() == "b0");
    grid.mouseClick(firstHeader);
    REQUIRE(grid.collapsedSections().empty());
    grid.activateFirst();
    REQUIRE(activeName() == "a0");
}

TEST_CASE_METHOD(GridFixture, "Emoji grid top section follows scroll")
{
    std::vector<int> changes;
    auto connection = grid.topSectionChanged.connect([&](int section) { changes.push_back(section); });
    REQUIRE(grid.topSection() == 0);
    grid.scrollToSection(2);
    REQUIRE(grid.topSection() == 2);
    REQUIRE_FALSE(changes.empty());
    REQUIRE(changes.back() == 2);
    grid.scrollToSection(1);
    REQUIRE(grid.topSection() == 1);
}
