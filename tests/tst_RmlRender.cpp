#include "UI/Rml/RmlRender.hpp"
#include "UI/Rml/RmlRuntime.hpp"
#include "UI/Rml/RmlView.hpp"
#include "UI/Rml/ContextMenu.hpp"

#include <RmlUi/Core.h>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QMouseEvent>
#include <QTest>

using namespace Styx::UI::Rml;

class TestRmlRender : public QObject
{
    Q_OBJECT
private slots:
    void textureReleaseWaitsForOwnerContext_data();
    void textureReleaseWaitsForOwnerContext();
    void backgroundDocumentRefreshKeepsRendering();
    void contextMenuKeyboardAndPointerActions();
    void contextMenuNestedNavigationAndViewportBounds();
};

void TestRmlRender::textureReleaseWaitsForOwnerContext_data()
{
    QTest::addColumn<bool>("foreignContext");
    QTest::newRow("no-current-context") << false;
    QTest::newRow("different-current-context") << true;
}

void TestRmlRender::textureReleaseWaitsForOwnerContext()
{
    QFETCH(bool, foreignContext);
    View view(QStringLiteral("resource-lifetime"));
    view.resize(320, 200);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QTRY_VERIFY(view.QOpenGLWidget::context() && Runtime::instance().isInitialised());
    view.makeCurrent();
    auto *gl = view.QOpenGLWidget::context()->functions();
    const ::Rml::byte pixel[] = { 255, 0, 0, 255 };
    auto *renderer = Runtime::instance().renderer();
    const auto texture = renderer->GenerateTexture({ pixel, sizeof(pixel) }, { 1, 1 });
    QVERIFY(gl->glIsTexture(static_cast<GLuint>(texture)));
    view.doneCurrent();

    QOffscreenSurface surface;
    QOpenGLContext foreign;
    GLuint foreignTexture = 0;
    if (foreignContext) {
        surface.setFormat(view.QOpenGLWidget::context()->format());
        surface.create();
        QVERIFY(surface.isValid());
        foreign.setFormat(surface.format());
        QVERIFY(foreign.create());
        QVERIFY(foreign.makeCurrent(&surface));
        auto *foreignGl = foreign.functions();
        foreignGl->glGenTextures(1, &foreignTexture);
        foreignGl->glBindTexture(GL_TEXTURE_2D, foreignTexture);
    }

    renderer->ReleaseTexture(texture);
    if (foreignContext) {
        QVERIFY(foreign.functions()->glIsTexture(foreignTexture));
        foreign.doneCurrent();
    }

    view.makeCurrent();
    QVERIFY(gl->glIsTexture(static_cast<GLuint>(texture)));
    view.doneCurrent();
    view.requestRender();
    auto textureExists = [&] {
        view.makeCurrent();
        const bool exists = gl->glIsTexture(static_cast<GLuint>(texture));
        view.doneCurrent();
        return exists;
    };
    QTRY_VERIFY_WITH_TIMEOUT(!textureExists(), 2000);

    if (foreignContext) {
        QVERIFY(foreign.makeCurrent(&surface));
        QVERIFY(foreign.functions()->glIsTexture(foreignTexture));
        foreign.functions()->glDeleteTextures(1, &foreignTexture);
        foreign.doneCurrent();
    }
}

void TestRmlRender::backgroundDocumentRefreshKeepsRendering()
{
    View view(QStringLiteral("background-refresh"));
    view.resize(320, 200);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QTRY_VERIFY(view.context());
    auto *document = view.context()->LoadDocumentFromMemory(
        "<rml><head><style>body { margin: 0px; width: 100%; height: 100%; }"
        ".tile { display: inline-block; width: 12px; height: 12px; background-color: #556677; }"
        "</style></head><body><div id='refresh'></div></body></rml>");
    QVERIFY(document);
    document->Show();
    auto *host = document->GetElementById("refresh");
    QVERIFY(host);
    view.doneCurrent();

    for (int i = 0; i < 64; ++i) {
        QVERIFY(!QOpenGLContext::currentContext());
        const QColor expected = i % 2 ? QColor("#182430") : QColor("#243448");
        const QString markup = QStringLiteral(
            "<div style='position: absolute; left: 0px; top: 0px; width: 100%; height: 100%; background-color: %1;'>%2</div>")
                                   .arg(expected.name(), QStringLiteral("<div class='tile'></div>").repeated(80));
        host->SetInnerRML(markup.toStdString());
        const QImage frame = view.grabFramebuffer();
        QCOMPARE(frame.pixelColor(frame.width() / 2, frame.height() / 2), expected);
        view.doneCurrent();
    }
}

void TestRmlRender::contextMenuKeyboardAndPointerActions()
{
    View view(QStringLiteral("menu-actions"));
    view.resize(640, 480);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QTRY_VERIFY(view.context());
    ContextMenu menu(view);
    int selected = 0;
    const std::vector<MenuItem> entries{
        { QStringLiteral("Disabled"), [&] { selected = -1; }, false },
        { {}, {}, false, false, true },
        { QStringLiteral("First"), [&] { selected = 1; } },
        { QStringLiteral("Second"), [&] { selected = 2; } }
    };
    menu.show(entries, QPoint(20, 20));
    QTest::keyClick(&view, Qt::Key_Down);
    QTest::keyClick(&view, Qt::Key_Return);
    QCOMPARE(selected, 2);
    QVERIFY(!menu.isVisible());

    menu.show(entries, QPoint(20, 20));
    view.context()->Update();
    auto *doc = view.context()->GetDocument("context-menu-root");
    QVERIFY(doc);
    auto center = [&](const char *id) {
        auto *element = doc->GetElementById(id);
        const auto p = element->GetAbsoluteOffset(::Rml::BoxArea::Border);
        return QPoint(int((p.x + element->GetOffsetWidth() / 2) / view.devicePixelRatioF()),
                      int((p.y + element->GetOffsetHeight() / 2) / view.devicePixelRatioF()));
    };
    auto movePointer = [&](const QPoint &position) {
        QMouseEvent move(QEvent::MouseMove, QPointF(position), QPointF(view.mapToGlobal(position)),
                         Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&view, &move);
    };
    movePointer(center("menu-item-0"));
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, center("menu-item-0"));
    QCOMPARE(selected, 2);
    QVERIFY(menu.isVisible());
    movePointer(center("menu-item-2"));
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, center("menu-item-2"));
    QCOMPARE(selected, 1);
    QVERIFY(!menu.isVisible());

    menu.show(entries, QPoint(20, 20));
    QTest::keyClick(&view, Qt::Key_Escape);
    QVERIFY(!menu.isVisible());
    QCOMPARE(selected, 1);
    menu.show(entries, QPoint(20, 20));
    movePointer(QPoint(600, 450));
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, QPoint(600, 450));
    QVERIFY(!menu.isVisible());
    QCOMPARE(selected, 1);
}

void TestRmlRender::contextMenuNestedNavigationAndViewportBounds()
{
    View view(QStringLiteral("menu-nesting"));
    view.resize(420, 260);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QTRY_VERIFY(view.context());
    ContextMenu menu(view);
    bool selected = false;
    MenuItem submenu{ QStringLiteral("Settings") };
    submenu.children.push_back({ QStringLiteral("Profile"), [&] { selected = true; } });
    menu.show({ submenu }, QPoint(419, 259));
    view.context()->Update();
    auto *doc = view.context()->GetDocument("context-menu-root");
    QVERIFY(doc);
    auto *panel = doc->GetElementById("context-menu-panel");
    const auto offset = panel->GetAbsoluteOffset(::Rml::BoxArea::Border);
    const auto dimensions = view.context()->GetDimensions();
    QVERIFY(offset.x >= 0 && offset.y >= 0);
    QVERIFY(offset.x + panel->GetOffsetWidth() <= dimensions.x);
    QVERIFY(offset.y + panel->GetOffsetHeight() <= dimensions.y);
    QTest::keyClick(&view, Qt::Key_Right);
    QVERIFY(doc->GetElementById("context-menu-back"));
    QTest::keyClick(&view, Qt::Key_Left);
    QVERIFY(!doc->GetElementById("context-menu-back"));
    QTest::keyClick(&view, Qt::Key_Return);
    QTest::keyClick(&view, Qt::Key_Return);
    QVERIFY(selected);
    QVERIFY(!menu.isVisible());
}

QTEST_MAIN(TestRmlRender)
#include "tst_RmlRender.moc"
