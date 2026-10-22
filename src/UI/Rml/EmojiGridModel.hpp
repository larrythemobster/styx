#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QSet>
#include <QString>

#include <algorithm>

#include "Core/Emoji/PickerEmoji.hpp"

namespace Styx {
namespace UI {
namespace Rml {

class EmojiGridModel : public QObject
{
    Q_OBJECT
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
        QString id;
        QString title;
        Core::Snowflake guildId;
        bool collapsed = false;
        int top = 0;
        int height = 0;
        int topSpacer = 0;
        QList<VisibleRow> visibleRows;
        int bottomSpacer = 0;
    };

    explicit EmojiGridModel(QObject *parent = nullptr);

    void setSections(const QList<Core::PickerSection> &sections);
    [[nodiscard]] const QList<Core::PickerSection> &sections() const { return shownSections; }

    void setCollapsedSections(const QSet<QString> &sectionIds);
    [[nodiscard]] const QSet<QString> &collapsedSections() const { return collapsed; }
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

    [[nodiscard]] Hit hitTest(const QPoint &viewportPos) const;
    [[nodiscard]] QRect cellRect(const Cell &cell) const;
    [[nodiscard]] int stickyHeaderY(int section) const;

    void mouseClick(const QPoint &viewportPos, Qt::KeyboardModifiers modifiers = {});
    void clickCell(const Cell &cell, Qt::KeyboardModifiers modifiers = {});

    [[nodiscard]] QList<VirtualSection> computeVirtualSections(int overscanPx = 80) const;

signals:
    void activeEmojiChanged();
    void topSectionChanged(int section);
    void collapsedSectionsChanged();
    void scrollRequested(int newScrollY);
    void emojiClicked(const Styx::Core::PickerEmoji &emoji, Qt::KeyboardModifiers modifiers);
    void layoutChanged();

private:
    void relayout();
    [[nodiscard]] int sectionAtY(int contentY) const;
    [[nodiscard]] bool hasCells(int section) const;
    [[nodiscard]] int adjacentSectionWithCells(int section, int step) const;
    void ensureVisible(const Cell &cell);
    void notifyTopSection();

    QList<Core::PickerSection> shownSections;
    QList<SectionGeometry> geometry;
    QSet<QString> collapsed;

    Cell active;
    int viewHeight = 360;
    int currentScrollY = 0;
    int totalContentHeight = 0;
    int reportedTopSection = -1;
};

} // namespace Rml
} // namespace UI
} // namespace Styx
