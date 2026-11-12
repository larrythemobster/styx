#include "UI/Rml/RmlRender.hpp"
#include "UI/Rml/RmlRuntime.hpp"
#include "UI/Rml/RmlView.hpp"
#include "UI/Rml/ContextMenu.hpp"
#include "Core/Theme/Manager.hpp"
#include "Core/Theme/Generator.hpp"
#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <cmath>
#include <unordered_map>
#include <utility>
#include <algorithm>

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
#include <SDL3/SDL_opengl_glext.h>

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

class RenderTarget {
public:
    explicit RenderTarget(View &owner) : view(owner)
    {
        view.makeCurrent();
        const auto generate = reinterpret_cast<PFNGLGENFRAMEBUFFERSPROC>(SDL_GL_GetProcAddress("glGenFramebuffers"));
        bind = reinterpret_cast<PFNGLBINDFRAMEBUFFERPROC>(SDL_GL_GetProcAddress("glBindFramebuffer"));
        destroy = reinterpret_cast<PFNGLDELETEFRAMEBUFFERSPROC>(SDL_GL_GetProcAddress("glDeleteFramebuffers"));
        const auto attach = reinterpret_cast<PFNGLFRAMEBUFFERTEXTURE2DPROC>(SDL_GL_GetProcAddress("glFramebufferTexture2D"));
        const auto status = reinterpret_cast<PFNGLCHECKFRAMEBUFFERSTATUSPROC>(SDL_GL_GetProcAddress("glCheckFramebufferStatus"));
        REQUIRE(generate);
        REQUIRE(bind);
        REQUIRE(destroy);
        REQUIRE(attach);
        REQUIRE(status);
        const auto size = view.context()->GetDimensions();
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size.x, size.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        generate(1, &framebuffer);
        bind(GL_FRAMEBUFFER, framebuffer);
        attach(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        REQUIRE(status(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        view.doneCurrent();
    }

    ~RenderTarget() noexcept(false)
    {
        view.makeCurrent();
        bind(GL_FRAMEBUFFER, 0);
        destroy(1, &framebuffer);
        glDeleteTextures(1, &texture);
        view.doneCurrent();
    }

private:
    View &view;
    GLuint framebuffer = 0;
    GLuint texture = 0;
    PFNGLBINDFRAMEBUFFERPROC bind = nullptr;
    PFNGLDELETEFRAMEBUFFERSPROC destroy = nullptr;
};
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
    RenderTarget target(view);
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
        glReadBuffer(GL_COLOR_ATTACHMENT0);
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

TEST_CASE("Cascading context menus retain their parent and stay within the viewport", "[rml][menu]")
{
    View view("menu-nesting");
    prepare(view, 420, 260);
    ContextMenu menu(view);
    bool selected = false;
    MenuItem submenu{"Settings"};
    submenu.children.push_back({"Profile", [&] { selected = true; }});
    menu.show({submenu}, {419, 259});
    auto *doc = view.context()->GetDocument("context-menu-root");
    REQUIRE(doc);
    auto inBounds = [&](const char *id) {
        view.context()->Update();
        auto *panel = doc->GetElementById(id);
        REQUIRE(panel);
        REQUIRE(panel->IsVisible());
        const auto offset = panel->GetAbsoluteOffset(::Rml::BoxArea::Border);
        const auto dimensions = view.context()->GetDimensions();
        REQUIRE(offset.x >= 0);
        REQUIRE(offset.y >= 0);
        REQUIRE(offset.x + panel->GetOffsetWidth() <= dimensions.x);
        REQUIRE(offset.y + panel->GetOffsetHeight() <= dimensions.y);
    };
    inBounds("context-menu-panel");
    key(view, SDLK_DOWN);
    key(view, SDLK_RIGHT);
    inBounds("context-menu-panel");
    inBounds("context-submenu-1");
    REQUIRE_FALSE(selected);
    key(view, SDLK_LEFT);
    REQUIRE_FALSE(doc->GetElementById("context-submenu-1"));
    key(view, SDLK_RETURN);
    key(view, SDLK_RETURN);
    REQUIRE(selected);
    REQUIRE_FALSE(menu.isVisible());
}

TEST_CASE("Hover opens cascading menus without activating an action", "[rml][menu]")
{
    const float anchorX = GENERATE(20.f, 880.f);
    View view("menu-hover");
    prepare(view, 900, 600);
    ContextMenu menu(view);
    int selected = 0;
    MenuItem folder{"Folder"};
    folder.children.push_back({"Destination", [&] { ++selected; }});
    MenuItem move{"Move to..."};
    move.children.push_back(std::move(folder));
    menu.show({move}, {anchorX, 20});
    auto *doc = view.context()->GetDocument("context-menu-root");
    REQUIRE(doc);
    auto hover = [&](const char *id) {
        view.context()->Update();
        auto *row = doc->GetElementById(id);
        REQUIRE(row);
        const auto origin = row->GetAbsoluteOffset(::Rml::BoxArea::Border);
        const auto scale = view.devicePixelRatioF();
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.windowID = SDL_GetWindowID(view.window());
        event.motion.x = (origin.x + row->GetOffsetWidth() / 2) / scale;
        event.motion.y = (origin.y + row->GetOffsetHeight() / 2) / scale;
        REQUIRE(SDL_PushEvent(&event));
        View::processEvents();
        view.context()->Update();
        return Styx::Platform::Point{event.motion.x, event.motion.y};
    };
    hover("menu-item-0");
    REQUIRE(doc->GetElementById("context-submenu-1"));
    REQUIRE(selected == 0);
    hover("menu-item-1-0");
    REQUIRE(doc->GetElementById("context-submenu-2"));
    REQUIRE(doc->GetElementById("context-menu-panel")->IsVisible());
    REQUIRE(selected == 0);
    const auto point = hover("menu-item-2-0");
    click(view, int(point.x), int(point.y));
    REQUIRE(selected == 1);
    REQUIRE_FALSE(menu.isVisible());
}

TEST_CASE("Preference checkboxes stay open through repeated keyboard and pointer changes", "[rml][menu]")
{
    const bool nested = GENERATE(false, true);
    const bool keyboard = GENERATE(false, true);
    View view("menu-preferences");
    prepare(view, 640, 480);
    ContextMenu menu(view);
    MenuItem first{"First preference"};
    first.checked = false;
    first.toggle = [](bool, MenuItem::ToggleComplete complete) { complete(true); };
    MenuItem second = first;
    second.label = "Second preference";
    if (nested) {
        MenuItem parent{"Preferences"};
        parent.children = {first, second};
        menu.show({parent}, {20, 20});
        key(view, SDLK_DOWN);
        key(view, SDLK_RIGHT);
    } else {
        menu.show({first, second}, {20, 20});
        key(view, SDLK_DOWN);
    }
    auto *doc = view.context()->GetDocument("context-menu-root");
    REQUIRE(doc);
    const std::string prefix = nested ? "menu-item-1-" : "menu-item-";
    auto *firstRow = doc->GetElementById(prefix + "0");
    auto *secondRow = doc->GetElementById(prefix + "1");
    auto *panel = doc->GetElementById(nested ? "context-submenu-1" : "context-menu-panel");
    REQUIRE(firstRow);
    REQUIRE(secondRow);
    REQUIRE(panel);
    auto press = [&](::Rml::Element *row) {
        view.context()->Update();
        const auto position = row->GetAbsoluteOffset(::Rml::BoxArea::Border);
        const auto scale = view.devicePixelRatioF();
        click(view, int((position.x + row->GetOffsetWidth() / 2) / scale),
                    int((position.y + row->GetOffsetHeight() / 2) / scale));
    };
    if (keyboard) key(view, SDLK_SPACE); else press(firstRow);
    REQUIRE(menu.isVisible());
    REQUIRE(firstRow->GetAttribute<std::string>("aria-checked", "") == "true");
    REQUIRE(secondRow->GetAttribute<std::string>("aria-checked", "") == "false");
    if (keyboard) { key(view, SDLK_DOWN); key(view, SDLK_RETURN); } else press(secondRow);
    REQUIRE(menu.isVisible());
    REQUIRE(firstRow->GetAttribute<std::string>("aria-checked", "") == "true");
    REQUIRE(secondRow->GetAttribute<std::string>("aria-checked", "") == "true");
    if (keyboard) { key(view, SDLK_UP); key(view, SDLK_SPACE); } else press(firstRow);
    REQUIRE(firstRow->GetAttribute<std::string>("aria-checked", "") == "false");
    REQUIRE(secondRow->GetAttribute<std::string>("aria-checked", "") == "true");
    REQUIRE(doc->GetElementById(nested ? "context-submenu-1" : "context-menu-panel") == panel);
    REQUIRE(doc->GetElementById(prefix + "0") == firstRow);
    click(view, 620, 450);
    REQUIRE_FALSE(menu.isVisible());
}

TEST_CASE("Independent preference requests retain accepted state after another request fails", "[rml][menu]")
{
    View view("menu-pending-preferences");
    prepare(view, 640, 480);
    ContextMenu menu(view);
    MenuItem::ToggleComplete firstComplete;
    MenuItem::ToggleComplete secondComplete;
    MenuItem first{"First preference"};
    first.checked = false;
    first.toggle = [&](bool, MenuItem::ToggleComplete complete) { firstComplete = std::move(complete); };
    MenuItem second{"Second preference"};
    second.checked = false;
    second.toggle = [&](bool, MenuItem::ToggleComplete complete) { secondComplete = std::move(complete); };
    menu.show({first, second}, {20, 20});
    auto *doc = view.context()->GetDocument("context-menu-root");
    auto *firstRow = doc->GetElementById("menu-item-0");
    auto *secondRow = doc->GetElementById("menu-item-1");
    key(view, SDLK_DOWN);
    key(view, SDLK_SPACE);
    REQUIRE(menu.isVisible());
    REQUIRE(firstComplete);
    REQUIRE(firstRow->HasAttribute("disabled"));
    REQUIRE_FALSE(secondRow->HasAttribute("disabled"));
    REQUIRE(firstRow->GetAttribute<std::string>("aria-checked", "") == "false");
    key(view, SDLK_DOWN);
    key(view, SDLK_SPACE);
    REQUIRE(secondComplete);
    secondComplete(true);
    REQUIRE(menu.isVisible());
    REQUIRE(secondRow->GetAttribute<std::string>("aria-checked", "") == "true");
    REQUIRE(firstRow->HasAttribute("disabled"));
    firstComplete(false);
    REQUIRE_FALSE(firstRow->HasAttribute("disabled"));
    REQUIRE(firstRow->GetAttribute<std::string>("aria-checked", "") == "false");
    REQUIRE(secondRow->GetAttribute<std::string>("aria-checked", "") == "true");
    key(view, SDLK_UP);
    key(view, SDLK_SPACE);
    firstComplete(true);
    REQUIRE(firstRow->GetAttribute<std::string>("aria-checked", "") == "true");
    REQUIRE(secondRow->GetAttribute<std::string>("aria-checked", "") == "true");
    key(view, SDLK_ESCAPE);
    REQUIRE_FALSE(menu.isVisible());
}

TEST_CASE("Preference radio groups stay open and change only after a successful save", "[rml][menu]")
{
    View view("menu-radio-preferences");
    prepare(view, 640, 480);
    ContextMenu menu(view);
    MenuItem::ToggleComplete complete;
    MenuItem first{"All messages"};
    first.checked = true;
    first.radio = true;
    first.toggle = [&](bool, MenuItem::ToggleComplete done) { complete = std::move(done); };
    MenuItem second = first;
    second.label = "Mentions";
    second.checked = false;
    MenuItem parent{"Notifications"};
    parent.subtitle = first.label;
    parent.children = {first, second};
    menu.show({parent}, {20, 20});
    key(view, SDLK_DOWN);
    key(view, SDLK_RIGHT);
    key(view, SDLK_SPACE);
    REQUIRE_FALSE(complete);
    REQUIRE(menu.isVisible());
    auto *doc = view.context()->GetDocument("context-menu-root");
    auto *firstRow = doc->GetElementById("menu-item-1-0");
    auto *secondRow = doc->GetElementById("menu-item-1-1");
    key(view, SDLK_DOWN);
    key(view, SDLK_RETURN);
    REQUIRE(complete);
    REQUIRE(firstRow->HasAttribute("disabled"));
    REQUIRE(secondRow->HasAttribute("disabled"));
    complete(false);
    REQUIRE(firstRow->GetAttribute<std::string>("aria-checked", "") == "true");
    REQUIRE(secondRow->GetAttribute<std::string>("aria-checked", "") == "false");
    key(view, SDLK_SPACE);
    complete(true);
    REQUIRE(menu.isVisible());
    REQUIRE(doc->GetElementById("context-submenu-1")->IsVisible());
    REQUIRE(firstRow->GetAttribute<std::string>("aria-checked", "") == "false");
    REQUIRE(secondRow->GetAttribute<std::string>("aria-checked", "") == "true");
    REQUIRE(doc->GetElementById("menu-item-0")->QuerySelector(".menu-subtitle")->GetInnerRML() == second.label);
}

TEST_CASE("Late preference completion cannot change a replacement menu", "[rml][menu]")
{
    View view("menu-replaced-preferences");
    prepare(view, 640, 480);
    ContextMenu menu(view);
    MenuItem::ToggleComplete complete;
    MenuItem old{"Old preference"};
    old.checked = false;
    old.toggle = [&](bool, MenuItem::ToggleComplete done) { complete = std::move(done); };
    menu.show({old}, {20, 20});
    key(view, SDLK_DOWN);
    key(view, SDLK_SPACE);
    REQUIRE(complete);
    click(view, 620, 450);
    REQUIRE_FALSE(menu.isVisible());
    MenuItem replacement{"New preference"};
    replacement.checked = false;
    replacement.toggle = [](bool, MenuItem::ToggleComplete done) { done(true); };
    menu.show({replacement}, {20, 20});
    complete(true);
    auto *row = view.context()->GetDocument("context-menu-root")->GetElementById("menu-item-0");
    REQUIRE(menu.isVisible());
    REQUIRE(row->GetAttribute<std::string>("aria-checked", "") == "false");
    REQUIRE_FALSE(row->HasAttribute("disabled"));
    key(view, SDLK_DOWN);
    key(view, SDLK_SPACE);
    REQUIRE(row->GetAttribute<std::string>("aria-checked", "") == "true");
}


TEST_CASE("Theme changes repaint open documents and preserve drafts", "[rml][theme]")
{
    namespace Theme = Styx::Core::Theme;
    auto &theme = Theme::Manager::instance();
    struct RestoreTheme {
        Theme::Manager &theme;
        std::unordered_map<Theme::Token, Styx::Platform::Color> colors;
        std::vector<std::pair<Theme::FontRole, Theme::Font>> fonts;
        explicit RestoreTheme(Theme::Manager &manager) : theme(manager)
        {
            for (const auto &token : Theme::registry())
                if (theme.hasOverride(token.token))
                    colors.emplace(token.token, theme.color(token.token));
            for (const auto &font : Theme::fontRegistry())
                if (theme.hasFontOverride(font.role))
                    fonts.emplace_back(font.role, theme.font(font.role));
        }
        ~RestoreTheme()
        {
            theme.resetAll();
            theme.setOverrides(colors);
            for (const auto &[role, font] : fonts)
                theme.setFontOverride(role, font);
            theme.apply();
            theme.applyFonts();
        }
    } restore(theme);
    View view("theme-rendering");
    prepare(view, 640, 480);
    view.loadDocument("main.rml");
    auto *document = view.document();
    document->SetInnerRML(
        "<button id=\"sample\" class=\"settings-btn settings-btn-primary\"><span class=\"settings-btn-label\">Theme text</span></button>"
        "<div id=\"message\" class=\"message-body\">A message that wraps when its font changes</div>"
        "<pre id=\"code\" class=\"code-block\">const int value = 42;</pre>"
        "<input id=\"draft\" type=\"text\" value=\"Unsent draft\"/>");
    view.context()->ProcessMouseMove(630, 470, 0);
    view.renderNow();
    auto *sample = document->GetElementById("sample");
    auto *draft = document->GetElementById("draft");
    REQUIRE(draft->Focus());
    const auto widthBefore = ::Rml::ElementUtilities::GetStringWidth(sample, "Theme text");
    auto font = theme.font(Theme::FontRole::Ui);
    font.pointSize *= 2;
    theme.setFontOverride(Theme::FontRole::Ui, font);
    theme.applyFonts();
    view.renderNow();
    REQUIRE(::Rml::ElementUtilities::GetStringWidth(sample, "Theme text") > widthBefore * 1.5f);

    RenderTarget target(view);
    for (const auto seed : {0xf04d58u, 0x38b865u}) {
        view.context()->ProcessMouseMove(630, 470, 0);
        theme.setOverrides(Theme::generate(Styx::Platform::Color::rgb(seed), Theme::Scheme::Monochromatic, false));
        theme.apply();
        view.renderNow();
        REQUIRE(view.document() == document);
        REQUIRE(view.context()->GetFocusElement() == draft);
        REQUIRE(rmlui_dynamic_cast<::Rml::ElementFormControlInput *>(draft)->GetValue() == "Unsent draft");
        const auto expected = theme.color(Theme::Token::Highlight);
        const auto actual = sample->GetProperty<::Rml::Colourb>("background-color");
        REQUIRE(actual == ::Rml::Colourb(expected.r, expected.g, expected.b, expected.a));
        const auto background = document->GetProperty<::Rml::Colourb>("background-color");
        REQUIRE(background.red > 220);
        REQUIRE(background.green > 220);
        REQUIRE(background.blue > 220);
        view.makeCurrent();
        std::array<unsigned char, 4> pixel{};
        glReadPixels(int(sample->GetAbsoluteLeft()) + 8,
                     view.context()->GetDimensions().y - int(sample->GetAbsoluteTop()) - 9,
                     1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
        view.doneCurrent();
        REQUIRE(std::abs(int(pixel[0]) - expected.r) <= 1);
        REQUIRE(std::abs(int(pixel[1]) - expected.g) <= 1);
        REQUIRE(std::abs(int(pixel[2]) - expected.b) <= 1);
        view.context()->ProcessMouseMove(int(sample->GetAbsoluteLeft()) + 8, int(sample->GetAbsoluteTop()) + 8, 0);
        view.renderNow();
        auto luminance = [](::Rml::Colourb color) {
            auto channel = [](unsigned char value) {
                const double c = value / 255.0;
                return c <= .04045 ? c / 12.92 : std::pow((c + .055) / 1.055, 2.4);
            };
            return .2126 * channel(color.red) + .7152 * channel(color.green) + .0722 * channel(color.blue);
        };
        const auto foreground = luminance(sample->GetProperty<::Rml::Colourb>("color"));
        const auto hovered = luminance(sample->GetProperty<::Rml::Colourb>("background-color"));
        REQUIRE((std::max(foreground, hovered) + .05) / (std::min(foreground, hovered) + .05) >= 4.5);
    }
    {
        View fresh("new-themed-document");
        prepare(fresh, 320, 240);
        fresh.loadDocument("main.rml");
        fresh.renderNow();
        REQUIRE(fresh.document()->GetProperty<::Rml::Colourb>("background-color")
                == document->GetProperty<::Rml::Colourb>("background-color"));
    }
    theme.setOverrides(Theme::generate(Styx::Platform::Color::rgb(0x5865f2), Theme::Scheme::Monochromatic, true));
    theme.apply();
    view.renderNow();
    REQUIRE(document->GetProperty<::Rml::Colourb>("background-color").red < 60);
    REQUIRE(document->GetProperty<::Rml::Colourb>("background-color").green < 60);
    REQUIRE(document->GetProperty<::Rml::Colourb>("background-color").blue < 60);
}

TEST_CASE("The last server remains reachable above the account and voice dock", "[rml][rail]")
{
    const int height = GENERATE(360, 600, 900);
    const bool connected = GENERATE(false, true);
    const bool folderOpen = GENERATE(false, true);
    View view("server-rail-scrolling");
    prepare(view, 960, height);
    view.loadDocument("main.rml");
    auto *document = view.document();
    REQUIRE(document);
    document->SetClass("rail-folder-open", folderOpen);
    auto *rail = document->GetElementById("rail");
    auto *dock = document->GetElementById("user-dock");
    auto *voice = document->GetElementById("voice-panel");
    REQUIRE(rail);
    REQUIRE(dock);
    REQUIRE(voice);
    voice->SetProperty("display", connected ? "block" : "none");
    voice->SetInnerRML("<div style=\"height: 88dp;\">Voice connected</div>");
    document->GetElementById("user-panel")->SetInnerRML("<div>Account</div>");
    std::string markup;
    for (const auto *column : {"primary", "expanded"}) {
        markup += "<div class=\"rail-" + std::string(column) + "\">";
        for (int i = 0; i < 32; ++i) {
            const std::string id = i == 31 ? " id=\"last-" + std::string(column) + "\"" : "";
            markup += "<div class=\"rail-item\"><div class=\"rail-btn\"" + id + ">" + std::to_string(i) + "</div></div>";
        }
        markup += "</div>";
    }
    rail->SetInnerRML(markup);
    view.renderNow();
    for (const auto *column : {"primary", "expanded"}) {
        if (!folderOpen && std::string_view(column) == "expanded") continue;
        auto *scroll = rail->QuerySelector(".rail-" + std::string(column));
        REQUIRE(scroll);
        scroll->SetScrollTop(scroll->GetScrollHeight());
        view.renderNow();
        auto *last = document->GetElementById("last-" + std::string(column));
        REQUIRE(last);
        CHECK(last->GetAbsoluteTop() >= scroll->GetAbsoluteTop());
        CHECK(last->GetAbsoluteTop() + last->GetOffsetHeight() <= dock->GetAbsoluteTop() + .5f);
        const auto center = ::Rml::Vector2f(last->GetAbsoluteLeft() + last->GetOffsetWidth() / 2,
            last->GetAbsoluteTop() + last->GetOffsetHeight() / 2);
        auto *hit = view.context()->GetElementAtPoint(center);
        while (hit && hit != last) hit = hit->GetParentNode();
        CHECK(hit == last);
    }
}
