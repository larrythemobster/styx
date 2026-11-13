#pragma once

#include "Core/Theme/Appearance.hpp"
#include "Platform/Native.hpp"
#include "Platform/Timer.hpp"
#include "Platform/Trackable.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

union SDL_Event;
struct SDL_Window;
struct SDL_Cursor;
namespace Rml { class Context; class ElementDocument; }

namespace Styx::UI::Rml {

class RenderContextScope {
public:
    RenderContextScope();
    ~RenderContextScope() noexcept(false);
    RenderContextScope(const RenderContextScope &) = delete;
    RenderContextScope &operator=(const RenderContextScope &) = delete;
private:
    SDL_Window *previousWindow = nullptr;
    void *previousContext = nullptr;
#ifdef _WIN32
    void *previousNativeContext = nullptr;
    void *previousNativeDc = nullptr;
#endif
    bool changed = false;
};

class View : public Platform::Trackable {
public:
    explicit View(const std::string &contextName);
    ~View() override;
    ::Rml::Context *context() const { return rmlContext; }
    ::Rml::ElementDocument *document() const { return rmlDocument; }
    void loadDocument(const std::string &path);
    void requestRender(int animateMs = 0);
    void renderNow();
    void makeCurrent();
    void doneCurrent();
    void show();
    void hide();
    void close();
    void minimize();
    void raise();
    void activateWindow();
    // Flashes the taskbar button until the window is focused.
    void requestAttention();
    void showMaximized();
    void showFullScreen();
    void showNormal();
    void setCustomTitleBar(bool enabled, float heightDp = 28, float controlsWidthDp = 236, float leftControlsWidthDp = 80);
    void resize(int width, int height);
    void move(int x, int y);
    int width() const;
    int height() const;
    Platform::Point position() const;
    float devicePixelRatioF() const;
    bool isVisible() const;
    bool isActiveWindow() const;
    bool isMaximized() const;
    void setWindowTitle(const std::string &title);
    std::string windowTitle() const;
    void *nativeHandle() const;
    SDL_Window *window() const { return nativeWindow; }
    Platform::Point mapFromGlobal(Platform::Point point) const;
    Platform::Point mapToGlobal(Platform::Point point) const;

    Platform::Signal<::Rml::ElementDocument *> documentLoaded;
    Platform::Signal<const std::string &> initialisationFailed;
    Platform::Signal<> shown;
    Platform::Signal<> layoutUpdated;
    Platform::Signal<> windowStateChanged;
    // Every key press and release in this window, before RmlUi sees it: an
    // Rml::Input::KeyIdentifier, the Rml::Input::KeyModifier state, and whether it went down.
    // Repeats while held arrive as further presses.
    Platform::Signal<int, int, bool> keyChanged;
    Platform::Signal<> focusLost;
    std::function<bool()> closeRequested;

    static void processEvents();
    static void dispatchEvent(const SDL_Event &event);
    static void renderPending();
    static std::optional<std::chrono::milliseconds> nextRenderDelay();
    static bool quitRequested();
    // The operating system's light/dark preference and its change notifications.
    static Core::Theme::DeviceTheme systemTheme();
    static Platform::Signal<> &systemThemeChanged();

private:
    void initialise();
    void updateDimensions();
    void handleEvent(const SDL_Event &event);
    void applyCursor();
    SDL_Window *nativeWindow = nullptr;
    SDL_Cursor *cursor = nullptr;
    std::string name;
    std::string pendingDocument;
    std::string lastCursor;
    ::Rml::Context *rmlContext = nullptr;
    ::Rml::ElementDocument *rmlDocument = nullptr;
    std::chrono::steady_clock::time_point animateUntil{};
    bool dirty = true;
    bool closed = false;
};
}
