#include "UI/Rml/EmojiGridModel.hpp"

namespace Styx {
namespace UI {
namespace Rml {

void EmojiGridModel::setSections(const std::vector<Core::PickerSection> &sections)
{
    shownSections = sections;
    active = Cell();
    currentScrollY = 0;
    reportedTopSection = -1;
    relayout();
    notifyTopSection();
}

void EmojiGridModel::setCollapsedSections(const std::unordered_set<std::string> &sectionIds)
{
    if (collapsed == sectionIds)
        return;
    collapsed = sectionIds;
    relayout();
    collapsedSectionsChanged();
}

void EmojiGridModel::toggleCollapsed(int section)
{
    if (section < 0 || section >= shownSections.size())
        return;

    const std::string &id = shownSections.at(section).id;
    if (collapsed.contains(id))
        collapsed.erase(id);
    else
        collapsed.insert(id);

    relayout();
    collapsedSectionsChanged();

    if (active.section == section && collapsed.contains(id))
        activateFirst();
}

void EmojiGridModel::setViewportHeight(int height)
{
    if (viewHeight == height)
        return;
    viewHeight = std::max(1, height);
    relayout();
    notifyTopSection();
}

void EmojiGridModel::setScrollY(int y)
{
    const int clamped = std::clamp(y, 0, maxScrollY());
    if (currentScrollY == clamped)
        return;
    currentScrollY = clamped;
    notifyTopSection();
    layoutChanged();
}

void EmojiGridModel::scrollToSection(int section)
{
    if (section >= 0 && section < geometry.size()) {
        const int targetY = geometry.at(section).top;
        setScrollY(targetY);
        scrollRequested(currentScrollY);
    }
}

int EmojiGridModel::topSection() const
{
    return geometry.empty() ? -1 : std::max(0, sectionAtY(currentScrollY));
}

void EmojiGridModel::relayout()
{
    geometry.clear();
    int y = 0;
    for (const Core::PickerSection &section : shownSections) {
        SectionGeometry sectionGeometry;
        sectionGeometry.top = y;
        sectionGeometry.headerHeight = section.title.empty() ? 0 : HeaderHeight;
        sectionGeometry.rows = collapsed.contains(section.id) ? 0 : static_cast<int>((section.emojis.size() + Columns - 1) / Columns);
        geometry.push_back(sectionGeometry);
        y = sectionGeometry.bottom() + SectionGap;
    }
    totalContentHeight = y;
    currentScrollY = std::clamp(currentScrollY, 0, maxScrollY());
    layoutChanged();
}

int EmojiGridModel::sectionAtY(int contentY) const
{
    const auto after = std::upper_bound(geometry.cbegin(), geometry.cend(), contentY,
                                        [](int y, const SectionGeometry &sectionGeometry) {
                                            return y < sectionGeometry.top;
                                        });
    return static_cast<int>(after - geometry.cbegin()) - 1;
}

const Core::PickerSection *EmojiGridModel::section(int index) const
{
    if (index >= 0 && index < shownSections.size())
        return &shownSections.at(index);
    return nullptr;
}

const Core::PickerEmoji *EmojiGridModel::emojiAt(const Cell &cell) const
{
    if (!cell.isValid() || cell.section >= shownSections.size() || cell.index >= shownSections.at(cell.section).emojis.size())
        return nullptr;
    return &shownSections.at(cell.section).emojis.at(cell.index);
}

const Core::PickerEmoji *EmojiGridModel::activeEmoji() const
{
    return emojiAt(active);
}

bool EmojiGridModel::hasCells(int section) const
{
    return section >= 0 && section < geometry.size() && geometry.at(section).rows > 0;
}

int EmojiGridModel::adjacentSectionWithCells(int section, int step) const
{
    for (int candidate = section + step; candidate >= 0 && candidate < geometry.size(); candidate += step) {
        if (hasCells(candidate))
            return candidate;
    }
    return -1;
}

void EmojiGridModel::setActiveCell(const Cell &cell)
{
    if (active == cell)
        return;
    active = cell;
    activeEmojiChanged();
}

void EmojiGridModel::activateFirst()
{
    const int first = adjacentSectionWithCells(-1, 1);
    setActiveCell(first < 0 ? Cell() : Cell{ first, 0 });
}

bool EmojiGridModel::activeIsFirst() const
{
    return active.isValid() && active.index == 0 && adjacentSectionWithCells(active.section, -1) < 0;
}

void EmojiGridModel::moveActive(int columnDelta, int rowDelta)
{
    if (!active.isValid() || !hasCells(active.section)) {
        activateFirst();
        return;
    }

    const auto countIn = [this](int section) { return static_cast<int>(shownSections.at(section).emojis.size()); };
    const int count = countIn(active.section);
    Cell target = active;

    if (columnDelta != 0) {
        target.index += columnDelta;
        if (target.index < 0) {
            const int previous = adjacentSectionWithCells(active.section, -1);
            if (previous < 0)
                return;
            target = { previous, countIn(previous) - 1 };
        } else if (target.index >= count) {
            const int next = adjacentSectionWithCells(active.section, 1);
            if (next < 0)
                return;
            target = { next, 0 };
        }
    }

    if (rowDelta != 0) {
        const int column = active.index % Columns;
        const int row = active.index / Columns + rowDelta;
        if (row < 0) {
            const int previous = adjacentSectionWithCells(active.section, -1);
            if (previous < 0)
                return;
            const int lastRowStart = ((countIn(previous) - 1) / Columns) * Columns;
            target = { previous, std::min(lastRowStart + column, countIn(previous) - 1) };
        } else if (row > (count - 1) / Columns) {
            const int next = adjacentSectionWithCells(active.section, 1);
            if (next < 0)
                return;
            target = { next, std::min(column, countIn(next) - 1) };
        } else {
            target.index = std::min(row * Columns + column, count - 1);
        }
    }

    setActiveCell(target);
    ensureVisible(target);
}

void EmojiGridModel::ensureVisible(const Cell &cell)
{
    if (!cell.isValid() || cell.section >= geometry.size())
        return;

    const SectionGeometry &sectionGeometry = geometry.at(cell.section);
    const int cellTop = sectionGeometry.cellsTop() + (cell.index / Columns) * CellPx;
    const int visibleTop = currentScrollY + sectionGeometry.headerHeight;
    const int visibleBottom = currentScrollY + viewHeight;

    if (cellTop < visibleTop) {
        setScrollY(cellTop - sectionGeometry.headerHeight);
        scrollRequested(currentScrollY);
    } else if (cellTop + CellPx > visibleBottom) {
        setScrollY(cellTop + CellPx - viewHeight);
        scrollRequested(currentScrollY);
    }
}

void EmojiGridModel::notifyTopSection()
{
    const int top = topSection();
    if (top != reportedTopSection) {
        reportedTopSection = top;
        topSectionChanged(top);
    }
}

const EmojiGridModel::SectionGeometry *EmojiGridModel::geometryFor(int section) const
{
    if (section >= 0 && section < geometry.size())
        return &geometry.at(section);
    return nullptr;
}

Platform::Rect EmojiGridModel::cellRect(const Cell &cell) const
{
    if (!cell.isValid() || cell.section >= geometry.size())
        return {};
    const SectionGeometry &sectionGeometry = geometry.at(cell.section);
    return Platform::Rect{static_cast<float>(SidePadding + (cell.index % Columns) * CellPx),
                 static_cast<float>(sectionGeometry.cellsTop() + (cell.index / Columns) * CellPx - currentScrollY),
                 static_cast<float>(CellPx), static_cast<float>(CellPx)};
}

int EmojiGridModel::stickyHeaderY(int section) const
{
    if (section < 0 || section >= geometry.size())
        return 0;
    const SectionGeometry &sectionGeometry = geometry.at(section);
    return std::min(std::max(0, sectionGeometry.top - currentScrollY),
                    sectionGeometry.bottom() - currentScrollY - sectionGeometry.headerHeight);
}

EmojiGridModel::Hit EmojiGridModel::hitTest(const Platform::Point &viewportPos) const
{
    Hit hit;
    if (geometry.empty())
        return hit;

    const int top = topSection();
    if (top >= 0 && top < geometry.size() && geometry.at(top).headerHeight > 0) {
        const int headerY = stickyHeaderY(top);
        if (viewportPos.y >= headerY && viewportPos.y < headerY + HeaderHeight) {
            hit.headerSection = top;
            return hit;
        }
    }

    const int contentY = viewportPos.y + currentScrollY;
    const int section = sectionAtY(contentY);
    if (section < 0)
        return hit;

    const SectionGeometry &sectionGeometry = geometry.at(section);
    if (contentY < sectionGeometry.cellsTop()) {
        hit.headerSection = section;
        return hit;
    }

    const int x = viewportPos.x - SidePadding;
    if (contentY >= sectionGeometry.bottom() || x < 0 || x >= Columns * CellPx)
        return hit;

    const int index = ((contentY - sectionGeometry.cellsTop()) / CellPx) * Columns + x / CellPx;
    if (index < shownSections.at(section).emojis.size())
        hit.cell = { section, index };
    return hit;
}

void EmojiGridModel::mouseClick(const Platform::Point &viewportPos, bool keepOpen)
{
    const Hit hit = hitTest(viewportPos);
    if (hit.headerSection >= 0) {
        toggleCollapsed(hit.headerSection);
    } else if (hit.cell.isValid()) {
        clickCell(hit.cell, keepOpen);
    }
}

void EmojiGridModel::clickCell(const Cell &cell, bool keepOpen)
{
    if (const Core::PickerEmoji *emoji = emojiAt(cell)) {
        setActiveCell(cell);
        emojiClicked(*emoji, keepOpen);
    }
}

std::vector<EmojiGridModel::VirtualSection> EmojiGridModel::computeVirtualSections(int overscanPx) const
{
    std::vector<VirtualSection> result;
    const int viewportTop = std::max(0, currentScrollY - overscanPx);
    const int viewportBottom = currentScrollY + viewHeight + overscanPx;

    for (int s = 0; s < shownSections.size(); ++s) {
        const Core::PickerSection &sec = shownSections.at(s);
        const SectionGeometry &geom = geometry.at(s);
        const bool isCollapsed = collapsed.contains(sec.id);

        VirtualSection vs;
        vs.sectionIndex = s;
        vs.id = sec.id;
        vs.title = sec.title;
        vs.guildId = sec.guildId;
        vs.collapsed = isCollapsed;
        vs.top = geom.top;
        vs.height = geom.bottom() - geom.top;

        if (geom.bottom() < viewportTop || geom.top > viewportBottom) {
            // Completely outside visible window
            vs.topSpacer = geom.rows * CellPx;
            result.push_back(vs);
            continue;
        }

        if (isCollapsed || geom.rows == 0) {
            result.push_back(vs);
            continue;
        }

        const int cellsTop = geom.cellsTop();
        const int firstVisibleRow = std::clamp((viewportTop - cellsTop) / CellPx, 0, geom.rows - 1);
        const int lastVisibleRow = std::clamp((viewportBottom - cellsTop) / CellPx, 0, geom.rows - 1);

        vs.topSpacer = firstVisibleRow * CellPx;

        const int totalEmojis = static_cast<int>(sec.emojis.size());
        for (int r = firstVisibleRow; r <= lastVisibleRow; ++r) {
            VisibleRow vr;
            vr.sectionIndex = s;
            vr.rowIndex = r;
            vr.firstEmojiIndex = r * Columns;
            vr.emojiCount = std::min(Columns, totalEmojis - vr.firstEmojiIndex);
            if (vr.emojiCount > 0)
                vs.visibleRows.push_back(vr);
        }

        vs.bottomSpacer = (geom.rows - 1 - lastVisibleRow) * CellPx;
        result.push_back(vs);
    }

    return result;
}

} // namespace Rml
} // namespace UI
} // namespace Styx
