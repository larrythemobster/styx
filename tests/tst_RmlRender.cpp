#include "UI/Rml/RmlRender.hpp"
#include "UI/Rml/RmlRuntime.hpp"
#include "UI/Rml/RmlView.hpp"

#include <RmlUi/Core.h>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QTest>

using namespace Acheron::UI::Rml;

class TestRmlRender : public QObject
{
    Q_OBJECT
private slots:
    void textureReleaseWaitsForOwnerContext_data();
    void textureReleaseWaitsForOwnerContext();
    void backgroundDocumentRefreshKeepsRendering();
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

QTEST_MAIN(TestRmlRender)
#include "tst_RmlRender.moc"
