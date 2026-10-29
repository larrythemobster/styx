#include "Desktop.hpp"

#include "Dispatcher.hpp"
#include "Core/Logging.hpp"

#include <SDL3/SDL.h>

#include <memory>
#include <utility>

namespace Styx::Platform::Desktop {
namespace {
bool active = false;
Signal<bool> activeChanged;
struct DialogRequest {
    FileCallback callback;
    std::vector<FileFilter> filters;
    std::vector<SDL_DialogFileFilter> nativeFilters;
};

void SDLCALL dialogFinished(void *userdata, const char *const *files, int)
{
    std::unique_ptr<DialogRequest> request(static_cast<DialogRequest *>(userdata));
    std::vector<std::string> paths;
    if (files) {
        for (auto file = files; *file; ++file)
            paths.emplace_back(*file);
    } else {
        Core::log(Core::LogLevel::Warning, LogUI) << "File dialog failed: " << SDL_GetError();
    }
    Dispatcher::main().post([callback = std::move(request->callback), paths = std::move(paths)]() mutable {
        if (callback)
            callback(std::move(paths));
    });
}

void showDialog(SDL_FileDialogType type, std::string title, std::string location,
                std::vector<FileFilter> filters, FileCallback callback, bool multiple)
{
    auto request = std::make_unique<DialogRequest>();
    request->callback = std::move(callback);
    request->filters = std::move(filters);
    request->nativeFilters.reserve(request->filters.size());
    for (const auto &filter : request->filters)
        request->nativeFilters.push_back({filter.name.c_str(), filter.pattern.c_str()});
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, title.c_str());
    if (!location.empty())
        SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, location.c_str());
    if (!request->nativeFilters.empty()) {
        SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, request->nativeFilters.data());
        SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, static_cast<Sint64>(request->nativeFilters.size()));
    }
    SDL_SetBooleanProperty(props, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, multiple);
    SDL_ShowFileDialogWithProperties(type, dialogFinished, request.release(), props);
    SDL_DestroyProperties(props);
}
}

std::string clipboardText()
{
    char *text = SDL_GetClipboardText();
    if (!text)
        return {};
    std::string result(text);
    SDL_free(text);
    return result;
}
bool applicationActive() { return active; }
Signal<bool> &activationChanged() { return activeChanged; }
void setApplicationActive(bool value)
{
    if (active != value) {
        active = value;
        activeChanged.publish(active);
    }
}

void setClipboardText(const std::string &text) { SDL_SetClipboardText(text.c_str()); }
bool openUrl(const std::string &url) { return SDL_OpenURL(url.c_str()); }
Point cursorPosition()
{
    float x = 0, y = 0;
    SDL_GetGlobalMouseState(&x, &y);
    return {x, y};
}
void openFiles(std::string title, std::vector<FileFilter> filters, FileCallback callback, bool multiple)
{
    showDialog(SDL_FILEDIALOG_OPENFILE, std::move(title), {}, std::move(filters), std::move(callback), multiple);
}
void saveFile(std::string title, std::string defaultName, FileCallback callback)
{
    showDialog(SDL_FILEDIALOG_SAVEFILE, std::move(title), std::move(defaultName), {}, std::move(callback), false);
}
}
