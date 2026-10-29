#pragma once

#include "Platform/Native.hpp"
#include "Platform/Trackable.hpp"
#include <string>
#include <vector>
#include <unordered_set>

#include <algorithm>

#include "Core/Emoji/PickerEmoji.hpp"

namespace Styx {
namespace UI {
namespace Rml {

class EmojiGridModel : public Platform::Trackable
{
public:
    static constexpr int Columns = 9;
    static constexpr int CellPx = 40;
    static constexpr int HeaderHeight = 28;
    static constexpr int SectionGap = 8;
    static constexpr int SidePadding = 8;

    struct Cell
    {
        int section = -1;
        int index = -1;

        [[nodiscard]] bool isValid() const { return section >= 0 && index >= 0; }
        bool operator==(const Cell &other) const { return section == other.section && index == other.index; }
    };

    struct SectionGeometry
    {
        int top = 0;
        int headerHeight = 0;
        int rows = 0;

        [[nodiscard]] int cellsTop() const { return top + headerHeight; }
        [[nodiscard]] int bottom() const { return cellsTop() + rows * CellPx; }
    };

    struct Hit
    {
        Cell cell;
        int headerSection = -1;
    };

    struct VisibleRow
    {
        int sectionIndex = -1;
        int rowIndex = -1;
        int firstEmojiIndex = -1;
        int emojiCount = 0;
    };

    struct VirtualSection
    {
        int sectionIndex = -1;
        std::string id;
        std::string title;
        Core::Snowflake guildId;
        bool collapsed = false;
        int top = 0;
        int height = 0;
        int topSpacer = 0;
        std::vector<VisibleRow> visibleRows;
        int bottomSpacer = 0;
    };

    EmojiGridModel() = default;

    void setSections(const std::vector<Core::PickerSection> &sections);
    [[nodiscard]] const std::vector<Core::PickerSection> &sections() const { return shownSections; }

    void setCollapsedSections(const std::unordered_set<std::string> &sectionIds);
    [[nodiscard]] const std::unordered_set<std::string> &collapsedSections() const { return collapsed; }
    void toggleCollapsed(int section);

    void setViewportHeight(int height);
    [[nodiscard]] int viewportHeight() const { return viewHeight; }

    void setScrollY(int y);
    [[nodiscard]] int scrollY() const { return currentScrollY; }
    [[nodiscard]] int maxScrollY() const { return std::max(0, totalContentHeight - viewHeight); }
    [[nodiscard]] int contentHeight() const { return totalContentHeight; }

    void scrollToSection(int section);
    [[nodiscard]] int topSection() const;

    void activateFirst();
    void moveActive(int columnDelta, int rowDelta);
    [[nodiscard]] bool activeIsFirst() const;
    [[nodiscard]] const Core::PickerEmoji *activeEmoji() const;
    [[nodiscard]] const Cell &activeCell() const { return active; }
    void setActiveCell(const Cell &cell);

    [[nodiscard]] const SectionGeometry *geometryFor(int section) const;
    [[nodiscard]] const Core::PickerSection *section(int index) const;
    [[nodiscard]] const Core::PickerEmoji *emojiAt(const Cell &cell) const;

    [[nodiscard]] Hit hitTest(const Platform::Point &viewportPos) const;
    [[nodiscard]] Platform::Rect cellRect(const Cell &cell) const;
    [[nodiscard]] int stickyHeaderY(int section) const;

    void mouseClick(const Platform::Point &viewportPos, bool keepOpen = false);
    void clickCell(const Cell &cell, bool keepOpen = false);

    [[nodiscard]] std::vector<VirtualSection> computeVirtualSections(int overscanPx = 80) const;

public:
    Platform::Signal<> activeEmojiChanged;
    Platform::Signal<int> topSectionChanged;
    Platform::Signal<> collapsedSectionsChanged;
    Platform::Signal<int> scrollRequested;
    Platform::Signal<const Core::PickerEmoji &, bool> emojiClicked;
    Platform::Signal<> layoutChanged;

private:
    void relayout();
    [[nodiscard]] int sectionAtY(int contentY) const;
    [[nodiscard]] bool hasCells(int section) const;
    [[nodiscard]] int adjacentSectionWithCells(int section, int step) const;
    void ensureVisible(const Cell &cell);
    void notifyTopSection();

    std::vector<Core::PickerSection> shownSections;
    std::vector<SectionGeometry> geometry;
    std::unordered_set<std::string> collapsed;

    Cell active;
    int viewHeight = 360;
    int currentScrollY = 0;
    int totalContentHeight = 0;
    int reportedTopSection = -1;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
