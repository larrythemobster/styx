#include "App.hpp"
#include "Platform/Dispatcher.hpp"
#include "UI/Rml/RmlView.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <climits>
#include <stdexcept>

namespace Styx {
namespace { Uint32 wakeEvent = 0; }
bool App::stopping = false;
int App::exitCode = 0;

App::App(int &, char **)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) throw std::runtime_error(SDL_GetError());
    wakeEvent = SDL_RegisterEvents(1);
    if (!wakeEvent) { SDL_Quit(); throw std::runtime_error(SDL_GetError()); }
    stopping = false;
    exitCode = 0;
    Platform::Dispatcher::main().setWakeHook([] {
        SDL_Event event{};
        event.type = wakeEvent;
        SDL_PushEvent(&event);
    });
}
App::~App()
{
    Platform::Dispatcher::main().setWakeHook({});
    SDL_Quit();
}
void App::quit(int code)
{
    stopping = true;
    exitCode = code;
    SDL_Event event{};
    event.type = wakeEvent;
    SDL_PushEvent(&event);
}
int App::exec()
{
    auto &dispatcher = Platform::Dispatcher::main();
    while (!stopping && !UI::Rml::View::quitRequested()) {
        UI::Rml::View::processEvents();
        dispatcher.runPending();
        UI::Rml::View::renderPending();
        if (stopping || UI::Rml::View::quitRequested()) break;
        auto delay = dispatcher.nextTimeout();
        auto frame = UI::Rml::View::nextRenderDelay();
        if (frame && (!delay || *frame < *delay)) delay = frame;
        SDL_Event event{};
        if (delay) {
            const int timeout = static_cast<int>(std::clamp<int64_t>(delay->count(), 0, INT_MAX));
            if (SDL_WaitEventTimeout(&event, timeout)) UI::Rml::View::dispatchEvent(event);
        } else if (SDL_WaitEvent(&event)) {
            UI::Rml::View::dispatchEvent(event);
        }
    }
    return exitCode;
}
}
