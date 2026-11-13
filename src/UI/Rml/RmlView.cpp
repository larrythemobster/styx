#include "RmlView.hpp"
#include "RmlRuntime.hpp"
#include "RmlRender.hpp"
#include "Core/Logging.hpp"
#include "Platform/Desktop.hpp"
#include "Platform/Image.hpp"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#endif

namespace Styx::UI::Rml {
namespace {
namespace KI = ::Rml::Input;
SDL_GLContext sharedContext = nullptr;
#ifdef _WIN32
HGLRC sharedNativeContext = nullptr;
#endif
std::unordered_map<SDL_WindowID, View *> windows;
bool quitting = false;

#ifdef _WIN32
struct CustomTitleBar {
    SDL_Window *window;
    WNDPROC previousProc;
    float heightDp;
    float controlsWidthDp;
    float leftControlsWidthDp;
};
std::unordered_map<HWND, CustomTitleBar> customTitleBars;

SDL_HitTestResult titleBarHitTest(SDL_WindowFlags flags, int width, int height, float scale,
                                  float heightDp, float controlsWidthDp, float leftControlsWidthDp, const SDL_Point &point)
{
    if (flags & SDL_WINDOW_FULLSCREEN) return SDL_HITTEST_NORMAL;

    if ((flags & SDL_WINDOW_RESIZABLE) && !(flags & SDL_WINDOW_MAXIMIZED)) {
        const float edge = 6.f * scale;
        const bool left = point.x < edge;
        const bool right = point.x >= width - edge;
        const bool top = point.y < edge;
        const bool bottom = point.y >= height - edge;
        if (top && left) return SDL_HITTEST_RESIZE_TOPLEFT;
        if (top && right) return SDL_HITTEST_RESIZE_TOPRIGHT;
        if (bottom && left) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        if (top) return SDL_HITTEST_RESIZE_TOP;
        if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;
        if (left) return SDL_HITTEST_RESIZE_LEFT;
        if (right) return SDL_HITTEST_RESIZE_RIGHT;
    }
    if (point.y < heightDp * scale && point.x >= leftControlsWidthDp * scale && point.x < width - controlsWidthDp * scale)
        return SDL_HITTEST_DRAGGABLE;
    return SDL_HITTEST_NORMAL;
}

SDL_HitTestResult SDLCALL customTitleBarHitTest(SDL_Window *window, const SDL_Point *point, void *data)
{
    const auto &titleBar = *static_cast<CustomTitleBar *>(data);
    int width = 0, height = 0, pixelWidth = 0;
    SDL_GetWindowSize(window, &width, &height);
    SDL_GetWindowSizeInPixels(window, &pixelWidth, nullptr);
    const float pixelDensity = width > 0 ? float(pixelWidth) / width : 1.f;
    const float displayScale = SDL_GetWindowDisplayScale(window);
    const float scale = pixelDensity > 0 && displayScale > 0 ? displayScale / pixelDensity : 1.f;
    return titleBarHitTest(SDL_GetWindowFlags(window), width, height, scale,
                           titleBar.heightDp, titleBar.controlsWidthDp, titleBar.leftControlsWidthDp, *point);
}

LRESULT CALLBACK customTitleBarProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto it = customTitleBars.find(hwnd);
    if (it == customTitleBars.end()) return DefWindowProcW(hwnd, message, wParam, lParam);
    const auto previousProc = it->second.previousProc;
    const auto window = it->second.window;
    const LRESULT result = CallWindowProcW(previousProc, hwnd, message, wParam, lParam);
    if (message == WM_GETMINMAXINFO && !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN)) {
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
            auto *info = reinterpret_cast<MINMAXINFO *>(lParam);
            info->ptMaxPosition.x = monitor.rcWork.left - monitor.rcMonitor.left;
            info->ptMaxPosition.y = monitor.rcWork.top - monitor.rcMonitor.top;
            info->ptMaxSize.x = monitor.rcWork.right - monitor.rcWork.left;
            info->ptMaxSize.y = monitor.rcWork.bottom - monitor.rcWork.top;
        }
    }
    return result;
}
#endif

int modifiers(SDL_Keymod mod = SDL_GetModState())
{
    int result = 0;
    if (mod & SDL_KMOD_CTRL) result |= KI::KM_CTRL;
    if (mod & SDL_KMOD_SHIFT) result |= KI::KM_SHIFT;
    if (mod & SDL_KMOD_ALT) result |= KI::KM_ALT;
    if (mod & SDL_KMOD_GUI) result |= KI::KM_META;
    if (mod & SDL_KMOD_CAPS) result |= KI::KM_CAPSLOCK;
    if (mod & SDL_KMOD_NUM) result |= KI::KM_NUMLOCK;
    return result;
}
KI::KeyIdentifier mapKey(SDL_Keycode key)
{
    if (key >= SDLK_A && key <= SDLK_Z) return static_cast<KI::KeyIdentifier>(KI::KI_A + key - SDLK_A);
    if (key >= SDLK_0 && key <= SDLK_9) return static_cast<KI::KeyIdentifier>(KI::KI_0 + key - SDLK_0);
    if (key >= SDLK_F1 && key <= SDLK_F12) return static_cast<KI::KeyIdentifier>(KI::KI_F1 + key - SDLK_F1);
    if (key >= SDLK_F13 && key <= SDLK_F24) return static_cast<KI::KeyIdentifier>(KI::KI_F13 + key - SDLK_F13);
    switch (key) {
    case SDLK_BACKSPACE: return KI::KI_BACK;
    case SDLK_TAB: return KI::KI_TAB;
    case SDLK_RETURN: return KI::KI_RETURN;
    case SDLK_KP_ENTER: return KI::KI_NUMPADENTER;
    case SDLK_ESCAPE: return KI::KI_ESCAPE;
    case SDLK_SPACE: return KI::KI_SPACE;
    case SDLK_DELETE: return KI::KI_DELETE;
    case SDLK_INSERT: return KI::KI_INSERT;
    case SDLK_HOME: return KI::KI_HOME;
    case SDLK_END: return KI::KI_END;
    case SDLK_PAGEUP: return KI::KI_PRIOR;
    case SDLK_PAGEDOWN: return KI::KI_NEXT;
    case SDLK_LEFT: return KI::KI_LEFT;
    case SDLK_RIGHT: return KI::KI_RIGHT;
    case SDLK_UP: return KI::KI_UP;
    case SDLK_DOWN: return KI::KI_DOWN;
    case SDLK_LSHIFT: return KI::KI_LSHIFT;
    case SDLK_RSHIFT: return KI::KI_RSHIFT;
    case SDLK_LCTRL: return KI::KI_LCONTROL;
    case SDLK_RCTRL: return KI::KI_RCONTROL;
    case SDLK_LALT: return KI::KI_LMENU;
    case SDLK_RALT: return KI::KI_RMENU;
    case SDLK_LGUI: return KI::KI_LMETA;
    case SDLK_RGUI: return KI::KI_RMETA;
    case SDLK_MINUS: return KI::KI_OEM_MINUS;
    case SDLK_EQUALS: return KI::KI_OEM_PLUS;
    case SDLK_COMMA: return KI::KI_OEM_COMMA;
    case SDLK_PERIOD: return KI::KI_OEM_PERIOD;
    case SDLK_SLASH: return KI::KI_OEM_2;
    case SDLK_SEMICOLON: return KI::KI_OEM_1;
    case SDLK_APOSTROPHE: return KI::KI_OEM_7;
    case SDLK_LEFTBRACKET: return KI::KI_OEM_4;
    case SDLK_RIGHTBRACKET: return KI::KI_OEM_6;
    case SDLK_BACKSLASH: return KI::KI_OEM_5;
    case SDLK_GRAVE: return KI::KI_OEM_3;
    default: return KI::KI_UNKNOWN;
    }
}
int mouseButton(Uint8 button)
{
    switch (button) {
    case SDL_BUTTON_LEFT: return 0;
    case SDL_BUTTON_RIGHT: return 1;
    case SDL_BUTTON_MIDDLE: return 2;
    default: return -1;
    }
}
void require(bool ok) { if (!ok) throw std::runtime_error(SDL_GetError()); }
}

View::View(const std::string &contextName) : name(contextName)
{
    if (!SDL_WasInit(SDL_INIT_VIDEO)) require(SDL_InitSubSystem(SDL_INIT_VIDEO));
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    nativeWindow = SDL_CreateWindow("Styx", 1280, 800,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (!nativeWindow) throw std::runtime_error(SDL_GetError());
    windows.emplace(SDL_GetWindowID(nativeWindow), this);
    quitting = false;
    track(Runtime::instance().texturesChanged.connect([this] { requestRender(); }));
    track(Runtime::instance().stylesChanged.connect([this] { requestRender(); }));
    auto icon = Platform::Image::decodeFile(Platform::Resources::path("app/styx.svg"));
    if (icon) {
        auto *surface = SDL_CreateSurfaceFrom(icon->width, icon->height, SDL_PIXELFORMAT_RGBA32,
            icon->pixels.data(), static_cast<int>(icon->stride()));
        if (surface) { SDL_SetWindowIcon(nativeWindow, surface); SDL_DestroySurface(surface); }
    }
}
View::~View()
{
    if (sharedContext) {
        makeCurrent();
        if (rmlContext) ::Rml::RemoveContext(name);
        rmlContext = nullptr;
        rmlDocument = nullptr;
        auto &runtime = Runtime::instance();
        if (::Rml::GetNumContexts() == 0) runtime.shutdown();
        else if (runtime.renderer()) runtime.renderer()->flushReleasedResources();
        doneCurrent();
    }
    windows.erase(SDL_GetWindowID(nativeWindow));
    if (windows.empty() && sharedContext) {
        SDL_GL_DestroyContext(sharedContext);
        sharedContext = nullptr;
    }
    if (cursor) SDL_DestroyCursor(cursor);
#ifdef _WIN32
    if (auto it = customTitleBars.find(static_cast<HWND>(nativeHandle())); it != customTitleBars.end()) {
        SDL_SetWindowHitTest(nativeWindow, nullptr, nullptr);
        SetWindowLongPtrW(it->first, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(it->second.previousProc));
        customTitleBars.erase(it);
    }
#endif
    SDL_DestroyWindow(nativeWindow);
}
RenderContextScope::RenderContextScope()
{
    if (!sharedContext || windows.empty()) throw std::logic_error("No window for GL resource upload");
#ifdef _WIN32
    if (wglGetCurrentContext() == sharedNativeContext) return;
    previousNativeContext = wglGetCurrentContext();
    previousNativeDc = wglGetCurrentDC();
#else
    if (SDL_GL_GetCurrentContext() == sharedContext) return;
#endif
    previousWindow = SDL_GL_GetCurrentWindow();
    previousContext = SDL_GL_GetCurrentContext();
    windows.begin()->second->makeCurrent();
    changed = true;
}
RenderContextScope::~RenderContextScope() noexcept(false)
{
    if (!changed) return;
    require(SDL_GL_MakeCurrent(previousWindow, static_cast<SDL_GLContext>(previousContext)));
#ifdef _WIN32
    if (wglGetCurrentContext() != previousNativeContext &&
        !wglMakeCurrent(static_cast<HDC>(previousNativeDc), static_cast<HGLRC>(previousNativeContext)))
        throw std::runtime_error("Cannot restore the caller's OpenGL context");
#endif
}
void View::makeCurrent()
{
    if (!sharedContext) return;
#ifdef _WIN32
    if (SDL_GL_GetCurrentContext() == sharedContext && wglGetCurrentContext() != sharedNativeContext)
        require(SDL_GL_MakeCurrent(nativeWindow, nullptr));
#endif
    require(SDL_GL_MakeCurrent(nativeWindow, sharedContext));
}
void View::doneCurrent() { require(SDL_GL_MakeCurrent(nativeWindow, nullptr)); }
void View::initialise()
{
    if (rmlContext) return;
    if (!sharedContext) {
        sharedContext = SDL_GL_CreateContext(nativeWindow);
        if (!sharedContext) throw std::runtime_error(SDL_GetError());
#ifdef _WIN32
        sharedNativeContext = wglGetCurrentContext();
#endif
        SDL_GL_SetSwapInterval(1);
    }
    makeCurrent();
    std::string error;
    if (!Runtime::instance().initialise(&error)) {
        doneCurrent();
        initialisationFailed.publish(error);
        throw std::runtime_error(error);
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(nativeWindow, &w, &h);
    rmlContext = ::Rml::CreateContext(name, {w, h});
    if (!rmlContext) { doneCurrent(); throw std::runtime_error("Cannot create RmlUi context: " + name); }
    rmlContext->SetDensityIndependentPixelRatio(SDL_GetWindowDisplayScale(nativeWindow));
    doneCurrent();
    SDL_StartTextInput(nativeWindow);
    if (!pendingDocument.empty()) {
        auto path = std::exchange(pendingDocument, {});
        loadDocument(path);
    }
}
void View::loadDocument(const std::string &path)
{
    if (!rmlContext) { pendingDocument = path; return; }
    if (rmlDocument) rmlDocument->Close();
    rmlDocument = rmlContext->LoadDocument(path);
    if (!rmlDocument) throw std::runtime_error("Cannot load RmlUi document: " + path);
    rmlDocument->Show();
    documentLoaded.publish(rmlDocument);
    requestRender();
}
void View::requestRender(int animateMs)
{
    dirty = true;
    animateUntil = std::max(animateUntil, std::chrono::steady_clock::now() + std::chrono::milliseconds(animateMs));
}
void View::updateDimensions()
{
    if (!rmlContext) return;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(nativeWindow, &w, &h);
    rmlContext->SetDimensions({w, h});
    rmlContext->SetDensityIndependentPixelRatio(SDL_GetWindowDisplayScale(nativeWindow));
    requestRender();
}
void View::renderNow()
{
    if (!rmlContext || !isVisible() || SDL_GetWindowFlags(nativeWindow) & SDL_WINDOW_MINIMIZED) return;
    makeCurrent();
    auto &runtime = Runtime::instance();
    auto *renderer = runtime.renderer();
    renderer->flushReleasedResources();
    const auto size = rmlContext->GetDimensions();
    renderer->SetViewport(size.x, size.y);
    if (runtime.releaseChangedTextures() && rmlDocument) {
        ::Rml::ElementList images;
        rmlDocument->GetElementsByTagName(images, "img");
        for (auto *image : images) {
            auto source = image->GetAttribute<::Rml::String>("src", "");
            if (source.starts_with("frame:")) image->SetAttribute("src", source);
        }
    }
    dirty = false;
    rmlContext->Update();
    layoutUpdated.publish();
    renderer->BeginFrame();
    renderer->Clear();
    rmlContext->Render();
    renderer->EndFrame();
    SDL_GL_SwapWindow(nativeWindow);
    doneCurrent();
    applyCursor();
}
void View::applyCursor()
{
    auto value = Runtime::instance().cursorName();
    if (value == lastCursor) return;
    lastCursor = value;
    SDL_SystemCursor kind = SDL_SYSTEM_CURSOR_DEFAULT;
    if (value == "pointer") kind = SDL_SYSTEM_CURSOR_POINTER;
    else if (value == "text") kind = SDL_SYSTEM_CURSOR_TEXT;
    else if (value == "move") kind = SDL_SYSTEM_CURSOR_MOVE;
    else if (value == "resize" || value == "ew-resize") kind = SDL_SYSTEM_CURSOR_EW_RESIZE;
    else if (value == "ns-resize") kind = SDL_SYSTEM_CURSOR_NS_RESIZE;
    else if (value == "not-allowed" || value == "unavailable") kind = SDL_SYSTEM_CURSOR_NOT_ALLOWED;
    auto *next = SDL_CreateSystemCursor(kind);
    if (!next) return;
    SDL_SetCursor(next);
    if (cursor) SDL_DestroyCursor(cursor);
    cursor = next;
}
void View::handleEvent(const SDL_Event &event)
{
    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) { close(); return; }
    if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) Platform::Desktop::setApplicationActive(true);
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        Platform::Desktop::setApplicationActive(SDL_GetKeyboardFocus() != nullptr);
        focusLost.publish();
    }
    if (event.type == SDL_EVENT_WINDOW_MAXIMIZED || event.type == SDL_EVENT_WINDOW_RESTORED ||
        event.type == SDL_EVENT_WINDOW_MINIMIZED || event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ||
        event.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) windowStateChanged.publish();
    if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) updateDimensions();
    if (!rmlContext) return;
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION: {
        const auto scale = devicePixelRatioF();
        rmlContext->ProcessMouseMove(static_cast<int>(event.motion.x * scale), static_cast<int>(event.motion.y * scale), modifiers());
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        const auto button = mouseButton(event.button.button);
        if (button >= 0) {
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) rmlContext->ProcessMouseButtonDown(button, modifiers());
            else rmlContext->ProcessMouseButtonUp(button, modifiers());
        }
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        auto direction = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? 1.f : -1.f;
        rmlContext->ProcessMouseWheel(::Rml::Vector2f(event.wheel.x * direction, event.wheel.y * direction), modifiers());
        break;
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE: rmlContext->ProcessMouseLeave(); break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const auto key = mapKey(event.key.key);
        const auto mods = modifiers(event.key.mod);
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        keyChanged.publish(key, mods, down);
        if (down)
            rmlContext->ProcessKeyDown(key, mods);
        else
            rmlContext->ProcessKeyUp(key, mods);
        break;
    }
    case SDL_EVENT_TEXT_INPUT: rmlContext->ProcessTextInput(event.text.text); break;
    default: break;
    }
    requestRender(250);
}
void View::dispatchEvent(const SDL_Event &event)
{
    if (event.type == SDL_EVENT_QUIT) { quitting = true; return; }
    if (event.type == SDL_EVENT_SYSTEM_THEME_CHANGED) { systemThemeChanged().publish(); return; }
    auto id = SDL_GetWindowID(SDL_GetWindowFromEvent(&event));
    if (auto it = windows.find(id); it != windows.end()) it->second->handleEvent(event);
}
void View::processEvents() { SDL_Event event; while (SDL_PollEvent(&event)) dispatchEvent(event); }
Core::Theme::DeviceTheme View::systemTheme()
{
    switch (SDL_GetSystemTheme()) {
    case SDL_SYSTEM_THEME_LIGHT: return Core::Theme::DeviceTheme::Light;
    case SDL_SYSTEM_THEME_DARK: return Core::Theme::DeviceTheme::Dark;
    default: return Core::Theme::DeviceTheme::Unknown;
    }
}
Platform::Signal<> &View::systemThemeChanged()
{
    static Platform::Signal<> signal;
    return signal;
}
void View::renderPending()
{
    const auto now = std::chrono::steady_clock::now();
    for (auto [id, view] : windows) {
        if (view->dirty || now < view->animateUntil || (view->rmlContext && view->rmlContext->GetNextUpdateDelay() <= 0)) view->renderNow();
    }
}
std::optional<std::chrono::milliseconds> View::nextRenderDelay()
{
    std::optional<std::chrono::milliseconds> delay;
    for (auto [id, view] : windows) {
        if (!view->isVisible() || !view->rmlContext) continue;
        if (view->dirty) return std::chrono::milliseconds(0);
        auto seconds = view->rmlContext->GetNextUpdateDelay();
        if (std::chrono::steady_clock::now() < view->animateUntil) seconds = std::min(seconds, 0.016);
        if (std::isfinite(seconds) && seconds < 86400) {
            const auto next = std::chrono::milliseconds(static_cast<int64_t>(std::ceil(std::max(0.0, seconds) * 1000)));
            if (!delay || next < *delay) delay = next;
        }
    }
    return delay;
}
bool View::quitRequested()
{
    return quitting || (!windows.empty() && std::all_of(windows.begin(), windows.end(), [](const auto &entry) { return entry.second->closed; }));
}
void View::show() { closed = false; require(SDL_ShowWindow(nativeWindow)); initialise(); shown.publish(); requestRender(); }
void View::hide() { SDL_HideWindow(nativeWindow); }
void View::close() { if (closeRequested && !closeRequested()) return; closed = true; hide(); }
void View::minimize() { SDL_MinimizeWindow(nativeWindow); }
void View::raise() { SDL_RaiseWindow(nativeWindow); }
void View::activateWindow() { raise(); }
void View::requestAttention() { SDL_FlashWindow(nativeWindow, SDL_FLASH_UNTIL_FOCUSED); }
void View::showMaximized() { SDL_MaximizeWindow(nativeWindow); show(); }
void View::showNormal() { SDL_SetWindowFullscreen(nativeWindow, false); SDL_RestoreWindow(nativeWindow); show(); }
void View::setCustomTitleBar(bool enabled, float heightDp, float controlsWidthDp, float leftControlsWidthDp)
{
#ifdef _WIN32
    const auto hwnd = static_cast<HWND>(nativeHandle());
    auto it = customTitleBars.find(hwnd);
    if (enabled) {
        if (it != customTitleBars.end()) {
            it->second.heightDp = heightDp;
            it->second.controlsWidthDp = controlsWidthDp;
            it->second.leftControlsWidthDp = leftControlsWidthDp;
            return;
        }
        const auto previousProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
        auto inserted = customTitleBars.emplace(hwnd, CustomTitleBar{nativeWindow, previousProc, heightDp, controlsWidthDp, leftControlsWidthDp});
        SetLastError(0);
        if (!SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(customTitleBarProc)) && GetLastError()) {
            customTitleBars.erase(inserted.first);
            throw std::runtime_error("Cannot install custom window procedure");
        }
        if (!SDL_SetWindowHitTest(nativeWindow, customTitleBarHitTest, &inserted.first->second)) {
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previousProc));
            customTitleBars.erase(inserted.first);
            throw std::runtime_error(SDL_GetError());
        }
        if (!SDL_SetWindowBordered(nativeWindow, false)) {
            SDL_SetWindowHitTest(nativeWindow, nullptr, nullptr);
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previousProc));
            customTitleBars.erase(inserted.first);
            throw std::runtime_error(SDL_GetError());
        }
        const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    } else if (it != customTitleBars.end()) {
        SDL_SetWindowHitTest(nativeWindow, nullptr, nullptr);
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(it->second.previousProc));
        customTitleBars.erase(it);
        require(SDL_SetWindowBordered(nativeWindow, true));
        const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_DEFAULT;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    }
#else
    (void)enabled;
    (void)heightDp;
    (void)controlsWidthDp;
    (void)leftControlsWidthDp;
#endif
}
void View::showFullScreen() { require(SDL_SetWindowFullscreen(nativeWindow, true)); show(); }
void View::resize(int w, int h) { require(SDL_SetWindowSize(nativeWindow, w, h)); updateDimensions(); }
void View::move(int x, int y) { require(SDL_SetWindowPosition(nativeWindow, x, y)); }
int View::width() const { int w = 0; SDL_GetWindowSize(nativeWindow, &w, nullptr); return w; }
int View::height() const { int h = 0; SDL_GetWindowSize(nativeWindow, nullptr, &h); return h; }
Platform::Point View::position() const { int x = 0, y = 0; SDL_GetWindowPosition(nativeWindow, &x, &y); return {float(x), float(y)}; }
float View::devicePixelRatioF() const { int pixels = 0; SDL_GetWindowSizeInPixels(nativeWindow, &pixels, nullptr); const int logical = width(); return logical > 0 ? float(pixels) / logical : 1.f; }
bool View::isVisible() const { return !(SDL_GetWindowFlags(nativeWindow) & SDL_WINDOW_HIDDEN); }
bool View::isActiveWindow() const { return SDL_GetKeyboardFocus() == nativeWindow; }
bool View::isMaximized() const { return SDL_GetWindowFlags(nativeWindow) & SDL_WINDOW_MAXIMIZED; }
void View::setWindowTitle(const std::string &title) { SDL_SetWindowTitle(nativeWindow, title.c_str()); }
std::string View::windowTitle() const { return SDL_GetWindowTitle(nativeWindow); }
void *View::nativeHandle() const
{
#ifdef _WIN32
    return SDL_GetPointerProperty(SDL_GetWindowProperties(nativeWindow), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(__APPLE__)
    return SDL_GetPointerProperty(SDL_GetWindowProperties(nativeWindow), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
#else
    return nativeWindow;
#endif
}
Platform::Point View::mapFromGlobal(Platform::Point point) const { auto pos = position(); return {point.x - pos.x, point.y - pos.y}; }
Platform::Point View::mapToGlobal(Platform::Point point) const { auto pos = position(); return {point.x + pos.x, point.y + pos.y}; }
}
