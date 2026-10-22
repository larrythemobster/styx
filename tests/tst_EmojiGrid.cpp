#include "UI/Rml/EmojiGridModel.hpp"

#include <QSignalSpy>
#include <QTest>

using namespace Styx;
using UI::Rml::EmojiGridModel;

class TestEmojiGrid : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void init();
    void cleanup();

    void firstCellBecomesActive();
    void horizontalMovesWrapAcrossRowsAndSections();
    void verticalMovesClampToShorterRows();
    void movesStopAtTheGridEdges();
    void keyboardSelectionScrollsIntoView();
    void clickReportsEmojiAndModifiers();
    void headerClickCollapsesSection();
    void topSectionFollowsScroll();

private:
    [[nodiscard]] QString activeName() const;

    EmojiGridModel *grid = nullptr;
};

static Core::PickerSection makeSection(const QString &id, const QString &title, int count)
{
    Core::PickerSection section;
    section.id = id;
    section.title = title;
    for (int i = 0; i < count; i++) {
        Core::PickerEmoji emoji;
        emoji.name = id + QString::number(i);
        emoji.surrogates = QString::fromUtf8("\xF0\x9F\x98\x80");
        section.emojis.append(emoji);
    }
    return section;
}

void TestEmojiGrid::initTestCase()
{
    QCOMPARE(EmojiGridModel::Columns, 9);
}

void TestEmojiGrid::init()
{
    grid = new EmojiGridModel;
    grid->setViewportHeight(360);
    grid->setSections({ makeSection("a", "First", 11), makeSection("b", "Second", 3), makeSection("c", "Third", 200) });
}

void TestEmojiGrid::cleanup()
{
    delete grid;
    grid = nullptr;
}

QString TestEmojiGrid::activeName() const
{
    const Core::PickerEmoji *emoji = grid->activeEmoji();
    return emoji ? emoji->name : QString();
}

void TestEmojiGrid::firstCellBecomesActive()
{
    QVERIFY(grid->activeEmoji() == nullptr);

    QSignalSpy activeChanged(grid, &EmojiGridModel::activeEmojiChanged);
    grid->activateFirst();
    QCOMPARE(activeName(), QStringLiteral("a0"));
    QVERIFY(grid->activeIsFirst());
    QCOMPARE(activeChanged.count(), 1);

    grid->moveActive(1, 0);
    QVERIFY(!grid->activeIsFirst());
}

void TestEmojiGrid::horizontalMovesWrapAcrossRowsAndSections()
{
    grid->activateFirst();
    for (int i = 0; i < 8; i++)
        grid->moveActive(1, 0);
    QCOMPARE(activeName(), QStringLiteral("a8"));

    grid->moveActive(1, 0);
    QCOMPARE(activeName(), QStringLiteral("a9"));
    grid->moveActive(1, 0);
    grid->moveActive(1, 0);
    QCOMPARE(activeName(), QStringLiteral("b0"));

    grid->moveActive(-1, 0);
    QCOMPARE(activeName(), QStringLiteral("a10"));
}

void TestEmojiGrid::verticalMovesClampToShorterRows()
{
    grid->activateFirst();
    for (int i = 0; i < 4; i++)
        grid->moveActive(1, 0);
    QCOMPARE(activeName(), QStringLiteral("a4"));

    grid->moveActive(0, 1);
    QCOMPARE(activeName(), QStringLiteral("a10"));
    grid->moveActive(0, 1);
    QCOMPARE(activeName(), QStringLiteral("b1"));
    grid->moveActive(0, 1);
    QCOMPARE(activeName(), QStringLiteral("c1"));

    grid->moveActive(0, -1);
    QCOMPARE(activeName(), QStringLiteral("b1"));
    grid->moveActive(0, -1);
    QCOMPARE(activeName(), QStringLiteral("a10"));
    grid->moveActive(0, -1);
    QCOMPARE(activeName(), QStringLiteral("a1"));
}

void TestEmojiGrid::movesStopAtTheGridEdges()
{
    grid->activateFirst();
    grid->moveActive(-1, 0);
    QCOMPARE(activeName(), QStringLiteral("a0"));
    grid->moveActive(0, -1);
    QCOMPARE(activeName(), QStringLiteral("a0"));

    grid->setSections({ makeSection("a", "Only", 2) });
    grid->activateFirst();
    grid->moveActive(1, 0);
    grid->moveActive(1, 0);
    QCOMPARE(activeName(), QStringLiteral("a1"));
    grid->moveActive(0, 1);
    QCOMPARE(activeName(), QStringLiteral("a1"));
}

void TestEmojiGrid::keyboardSelectionScrollsIntoView()
{
    grid->activateFirst();
    QCOMPARE(grid->scrollY(), 0);

    constexpr int RowsDown = 20;
    for (int i = 0; i < RowsDown; i++)
        grid->moveActive(0, 1);

    QCOMPARE(activeName(), QStringLiteral("c%1").arg((RowsDown - 3) * EmojiGridModel::Columns));
    QVERIFY(grid->scrollY() > 0);
    QCOMPARE(grid->topSection(), 2);
}

void TestEmojiGrid::clickReportsEmojiAndModifiers()
{
    grid->setSections({ makeSection("a", QString(), 20) });

    QStringList clickedNames;
    Qt::KeyboardModifiers clickedWith;
    connect(grid, &EmojiGridModel::emojiClicked, this, [&](const Core::PickerEmoji &emoji, Qt::KeyboardModifiers modifiers) {
        clickedNames.append(emoji.name);
        clickedWith = modifiers;
    });

    const QPoint secondRowFirstCell(EmojiGridModel::CellPx - 1, EmojiGridModel::CellPx + EmojiGridModel::CellPx / 2);
    grid->mouseClick(secondRowFirstCell, Qt::ShiftModifier);

    QCOMPARE(clickedNames, QStringList({ QStringLiteral("a9") }));
    QVERIFY(clickedWith.testFlag(Qt::ShiftModifier));
}

void TestEmojiGrid::headerClickCollapsesSection()
{
    QSignalSpy collapsedChanged(grid, &EmojiGridModel::collapsedSectionsChanged);
    const QPoint firstHeader(EmojiGridModel::CellPx, 4);
    grid->mouseClick(firstHeader, Qt::NoModifier);

    QCOMPARE(collapsedChanged.count(), 1);
    QVERIFY(grid->collapsedSections().contains(QStringLiteral("a")));

    grid->activateFirst();
    QCOMPARE(activeName(), QStringLiteral("b0"));
    grid->moveActive(-1, 0);
    QCOMPARE(activeName(), QStringLiteral("b0"));

    grid->mouseClick(firstHeader, Qt::NoModifier);
    QVERIFY(grid->collapsedSections().isEmpty());
    grid->activateFirst();
    QCOMPARE(activeName(), QStringLiteral("a0"));
}

void TestEmojiGrid::topSectionFollowsScroll()
{
    QSignalSpy topChanged(grid, &EmojiGridModel::topSectionChanged);
    QCOMPARE(grid->topSection(), 0);

    grid->scrollToSection(2);
    QCOMPARE(grid->topSection(), 2);
    QCOMPARE(topChanged.last().at(0).toInt(), 2);

    grid->scrollToSection(1);
    QCOMPARE(grid->topSection(), 1);
}

QTEST_GUILESS_MAIN(TestEmojiGrid)

#include "tst_EmojiGrid.moc"
