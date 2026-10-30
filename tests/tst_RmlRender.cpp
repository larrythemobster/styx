#include "UI/Rml/RmlRender.hpp"
#include "UI/Rml/RmlRuntime.hpp"
#include "UI/Rml/RmlView.hpp"
#include "UI/Rml/ContextMenu.hpp"

#include <RmlUi/Core.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _WIN32
#include <windows.h>
#endif
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <array>
#include <string>
#include <vector>
#include <memory>

using namespace Styx::UI::Rml;

namespace {
#ifdef _WIN32
HWND window(View &view)
{
    return static_cast<HWND>(view.nativeHandle());
}
#endif

void key(View &view, SDL_Keycode code)
{
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.windowID = SDL_GetWindowID(view.window());
    event.key.key = code;
    event.key.down = true;
    REQUIRE(SDL_PushEvent(&event));
    event.type = SDL_EVENT_KEY_UP;
    event.key.down = false;
    REQUIRE(SDL_PushEvent(&event));
    View::processEvents();
}

void click(View &view, int x, int y)
{
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.windowID = SDL_GetWindowID(view.window());
    event.motion.x = float(x);
    event.motion.y = float(y);
    REQUIRE(SDL_PushEvent(&event));
    event = {};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.windowID = SDL_GetWindowID(view.window());
    event.button.button = SDL_BUTTON_LEFT;
    event.button.down = true;
    event.button.x = float(x);
    event.button.y = float(y);
    REQUIRE(SDL_PushEvent(&event));
    event.type = SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.down = false;
    REQUIRE(SDL_PushEvent(&event));
    View::processEvents();
}

#ifdef _WIN32
class ForeignContext {
public:
    explicit ForeignContext(HWND owner)
    {
        handle = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        REQUIRE(handle);
        dc = GetDC(handle);
        REQUIRE(dc);
        HDC ownerDc = GetDC(owner);
        REQUIRE(ownerDc);
        const int format = GetPixelFormat(ownerDc);
        PIXELFORMATDESCRIPTOR descriptor{};
        const int described = DescribePixelFormat(ownerDc, format, sizeof(descriptor), &descriptor);
        ReleaseDC(owner, ownerDc);
        REQUIRE(described);
        REQUIRE(SetPixelFormat(dc, format, &descriptor));
        context = wglCreateContext(dc);
        REQUIRE(context);
    }

    ~ForeignContext()
    {
        if (wglGetCurrentContext() == context)
            wglMakeCurrent(nullptr, nullptr);
        if (context)
            wglDeleteContext(context);
        if (dc)
            ReleaseDC(handle, dc);
        if (handle)
            DestroyWindow(handle);
    }

    void makeCurrent() { REQUIRE(wglMakeCurrent(dc, context)); }

private:
    HWND handle = nullptr;
    HDC dc = nullptr;
    HGLRC context = nullptr;
};
#endif

void prepare(View &view, int width, int height)
{
    view.resize(width, height);
    view.show();
    REQUIRE(view.window());
    REQUIRE(view.context());
    REQUIRE(Runtime::instance().isInitialised());
    view.renderNow();
}
}

TEST_CASE("Texture release waits for the owning OpenGL context", "[rml][render]")
{
#ifdef _WIN32
    const bool foreignContext = GENERATE(false, true);
#endif
    View view("resource-lifetime");
    prepare(view, 320, 200);
    view.makeCurrent();
    const ::Rml::byte pixel[] = { 255, 0, 0, 255 };
    auto *renderer = Runtime::instance().renderer();
    REQUIRE(renderer);
    const auto texture = renderer->GenerateTexture({ pixel, sizeof(pixel) }, { 1, 1 });
    REQUIRE(glIsTexture(static_cast<GLuint>(texture)) == GL_TRUE);
    view.doneCurrent();

#ifdef _WIN32
    if (foreignContext) {
        view.makeCurrent();
        ForeignContext foreign(window(view));
        foreign.makeCurrent();
        GLuint foreignTexture = 0;
        glGenTextures(1, &foreignTexture);
        glBindTexture(GL_TEXTURE_2D, foreignTexture);
        renderer->ReleaseTexture(texture);
        REQUIRE(glIsTexture(foreignTexture) == GL_TRUE);
        view.makeCurrent();
        REQUIRE(glIsTexture(static_cast<GLuint>(texture)) == GL_TRUE);
        view.doneCurrent();
        view.requestRender();
        view.renderNow();
        view.makeCurrent();
        REQUIRE(glIsTexture(static_cast<GLuint>(texture)) == GL_FALSE);
        view.doneCurrent();
        foreign.makeCurrent();
        REQUIRE(glIsTexture(foreignTexture) == GL_TRUE);
        glDeleteTextures(1, &foreignTexture);
    } else
#endif
    {
        renderer->ReleaseTexture(texture);
        view.makeCurrent();
        REQUIRE(glIsTexture(static_cast<GLuint>(texture)) == GL_TRUE);
        view.doneCurrent();
        view.requestRender();
        view.renderNow();
        view.makeCurrent();
        REQUIRE(glIsTexture(static_cast<GLuint>(texture)) == GL_FALSE);
        view.doneCurrent();
    }
}

TEST_CASE("Image uploads outside a render pass restore the caller's context", "[rml][render]")
{
    View view("background-image-upload");
    prepare(view, 320, 200);
#ifdef _WIN32
    const bool useForeign = GENERATE(false, true);
    std::unique_ptr<ForeignContext> foreign;
    if (useForeign) {
        view.makeCurrent();
        foreign = std::make_unique<ForeignContext>(window(view));
        foreign->makeCurrent();
    }
    const auto previousNative = wglGetCurrentContext();
#endif
    const auto previous = SDL_GL_GetCurrentContext();
    auto *renderer = Runtime::instance().renderer();
    ::Rml::Vector2i dimensions;
    const auto texture = renderer->LoadTexture(dimensions, "icon:x");
    REQUIRE(SDL_GL_GetCurrentContext() == previous);
#ifdef _WIN32
    REQUIRE(wglGetCurrentContext() == previousNative);
#endif
    view.makeCurrent();
    REQUIRE(glIsTexture(static_cast<GLuint>(texture)) == GL_TRUE);
    view.doneCurrent();
    renderer->ReleaseTexture(texture);
}

TEST_CASE("Background document refresh keeps rendering", "[rml][render]")
{
    View view("background-refresh");
    prepare(view, 320, 200);
    auto *document = view.context()->LoadDocumentFromMemory(
        "<rml><head><style>body { margin: 0px; width: 100%; height: 100%; }"
        ".tile { display: inline-block; width: 12px; height: 12px; background-color: #556677; }"
        "</style></head><body><div id='refresh'></div></body></rml>");
    REQUIRE(document);
    document->Show();
    auto *host = document->GetElementById("refresh");
    REQUIRE(host);
    view.doneCurrent();

    for (int i = 0; i < 64; ++i) {
        REQUIRE(SDL_GL_GetCurrentContext() == nullptr);
        const std::array<GLubyte, 3> expected = i % 2
            ? std::array<GLubyte, 3>{ 0x18, 0x24, 0x30 }
            : std::array<GLubyte, 3>{ 0x24, 0x34, 0x48 };
        std::string markup = "<div style='position: absolute; left: 0px; top: 0px; width: 100%; height: 100%; background-color: ";
        markup += i % 2 ? "#182430;'>" : "#243448;'>";
        for (int tile = 0; tile < 80; ++tile)
            markup += "<div class='tile'></div>";
        markup += "</div>";
        host->SetInnerRML(markup);
        view.requestRender();
        view.renderNow();
        view.makeCurrent();
        std::array<GLubyte, 4> actual{};
        glReadBuffer(GL_FRONT);
        const auto dimensions = view.context()->GetDimensions();
        glReadPixels(dimensions.x / 2, dimensions.y / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
        CHECK(actual[0] == expected[0]);
        CHECK(actual[1] == expected[1]);
        CHECK(actual[2] == expected[2]);
        view.doneCurrent();
    }
}

TEST_CASE("Context menu keyboard and pointer actions", "[rml][menu]")
{
    View view("menu-actions");
    prepare(view, 640, 480);
    ContextMenu menu(view);
    int selected = 0;
    const std::vector<MenuItem> entries{
        { "Disabled", [&] { selected = -1; }, false },
        { {}, {}, false, false, true },
        { "First", [&] { selected = 1; } },
        { "Second", [&] { selected = 2; } }
    };
    menu.show(entries, { 20, 20 });
    key(view, SDLK_DOWN);
    key(view, SDLK_RETURN);
    REQUIRE(selected == 2);
    REQUIRE_FALSE(menu.isVisible());

    menu.show(entries, { 20, 20 });
    view.context()->Update();
    auto *doc = view.context()->GetDocument("context-menu-root");
    REQUIRE(doc);
    auto center = [&](const char *id) {
        auto *element = doc->GetElementById(id);
        REQUIRE(element);
        const auto p = element->GetAbsoluteOffset(::Rml::BoxArea::Border);
        const auto scale = view.devicePixelRatioF();
        return Styx::Platform::Point{(p.x + element->GetOffsetWidth() / 2) / scale,
                                     (p.y + element->GetOffsetHeight() / 2) / scale};
    };
    auto point = center("menu-item-0");
    click(view, point.x, point.y);
    REQUIRE(selected == 2);
    REQUIRE(menu.isVisible());
    point = center("menu-item-2");
    click(view, point.x, point.y);
    REQUIRE(selected == 1);
    REQUIRE_FALSE(menu.isVisible());

    menu.show(entries, { 20, 20 });
    key(view, SDLK_ESCAPE);
    REQUIRE_FALSE(menu.isVisible());
    REQUIRE(selected == 1);
    menu.show(entries, { 20, 20 });
    click(view, 600, 450);
    REQUIRE_FALSE(menu.isVisible());
    REQUIRE(selected == 1);
}

TEST_CASE("Context menu nested navigation stays within the viewport", "[rml][menu]")
{
    View view("menu-nesting");
    prepare(view, 420, 260);
    ContextMenu menu(view);
    bool selected = false;
    MenuItem submenu{ "Settings" };
    submenu.children.push_back({ "Profile", [&] { selected = true; } });
    menu.show({ submenu }, { 419, 259 });
    view.context()->Update();
    auto *doc = view.context()->GetDocument("context-menu-root");
    REQUIRE(doc);
    auto *panel = doc->GetElementById("context-menu-panel");
    REQUIRE(panel);
    const auto offset = panel->GetAbsoluteOffset(::Rml::BoxArea::Border);
    const auto dimensions = view.context()->GetDimensions();
    REQUIRE(offset.x >= 0);
    REQUIRE(offset.y >= 0);
    REQUIRE(offset.x + panel->GetOffsetWidth() <= dimensions.x);
    REQUIRE(offset.y + panel->GetOffsetHeight() <= dimensions.y);
    key(view, SDLK_RIGHT);
    REQUIRE(doc->GetElementById("context-menu-back"));
    key(view, SDLK_LEFT);
    REQUIRE_FALSE(doc->GetElementById("context-menu-back"));
    key(view, SDLK_RETURN);
    key(view, SDLK_RETURN);
    REQUIRE(selected);
    REQUIRE_FALSE(menu.isVisible());
}
