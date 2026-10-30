#include "Desktop.hpp"

#include "Dispatcher.hpp"
#include "Utf8.hpp"
#include "Core/Logging.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <commdlg.h>
#else
#include <SDL3/SDL.h>
#endif

#include <memory>
#include <utility>

namespace Styx::Platform::Desktop {

namespace {

bool g_active = false;
Signal<bool> g_activeChanged;

#ifdef _WIN32
std::vector<COMDLG_FILTERSPEC> makeWin32Filters(const std::vector<FileFilter> &filters,
                                                std::vector<std::wstring> &storage)
{
    storage.clear();
    storage.reserve(filters.size() * 2);
    std::vector<COMDLG_FILTERSPEC> specs;
    specs.reserve(filters.size());

    for (const auto &f : filters) {
        storage.push_back(toUtf16(f.name));
        std::string pat = f.pattern;
        if (!pat.empty() && pat.front() != '*') {
            std::string converted;
            for (std::string_view ext : split(pat, ';')) {
                if (!converted.empty())
                    converted += ';';
                converted += "*.";
                converted += ext;
            }
            pat = std::move(converted);
        }
        storage.push_back(toUtf16(pat));
    }

    for (size_t i = 0; i < filters.size(); ++i) {
        specs.push_back({ storage[i * 2].c_str(), storage[i * 2 + 1].c_str() });
    }
    return specs;
}
#else
struct DialogState {
    FileCallback callback;
    std::vector<FileFilter> filters;
    std::vector<SDL_DialogFileFilter> sdlFilters;
};

std::string sdlFilterPattern(const std::string &pattern)
{
    std::string result;
    for (std::string_view extension : split(pattern, ';')) {
        if (extension == "*" || extension == "*.*")
            return "*";
        if (extension.substr(0, 2) == "*.")
            extension.remove_prefix(2);
        if (!result.empty())
            result += ';';
        result.append(extension.data(), extension.size());
    }
    return result;
}

void SDLCALL dialogFinished(void *userdata, const char *const *files, int)
{
    std::unique_ptr<DialogState> state(static_cast<DialogState *>(userdata));
    std::vector<std::string> results;
    if (files) {
        for (; *files; ++files)
            results.emplace_back(*files);
    }
    if (state->callback) {
        Dispatcher::main().post([callback = std::move(state->callback), results = std::move(results)]() mutable {
            callback(std::move(results));
        });
    }
}

void showDialog(bool save, const std::string &title, const std::string &defaultName,
                std::vector<FileFilter> filters, FileCallback callback, bool multiple)
{
    auto state = std::make_unique<DialogState>();
    state->callback = std::move(callback);
    state->filters = std::move(filters);
    state->sdlFilters.reserve(state->filters.size());
    for (auto &filter : state->filters) {
        filter.pattern = sdlFilterPattern(filter.pattern);
        state->sdlFilters.push_back({filter.name.c_str(), filter.pattern.c_str()});
    }

    auto *sdlFilters = state->sdlFilters.empty() ? nullptr : state->sdlFilters.data();
    const auto filterCount = static_cast<int>(state->sdlFilters.size());
    const char *location = defaultName.empty() ? nullptr : defaultName.c_str();
    if (title.empty()) {
        if (save)
            SDL_ShowSaveFileDialog(dialogFinished, state.release(), nullptr, sdlFilters, filterCount, location);
        else
            SDL_ShowOpenFileDialog(dialogFinished, state.release(), nullptr, sdlFilters, filterCount, location, multiple);
        return;
    }

    SDL_PropertiesID properties = SDL_CreateProperties();
    if (!properties ||
        !SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, title.c_str()) ||
        (location && !SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_LOCATION_STRING, location)) ||
        (sdlFilters && !SDL_SetPointerProperty(properties, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, sdlFilters)) ||
        (sdlFilters && !SDL_SetNumberProperty(properties, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, filterCount)) ||
        (!save && !SDL_SetBooleanProperty(properties, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, multiple))) {
        if (properties)
            SDL_DestroyProperties(properties);
        dialogFinished(state.release(), nullptr, -1);
        return;
    }
    SDL_ShowFileDialogWithProperties(save ? SDL_FILEDIALOG_SAVEFILE : SDL_FILEDIALOG_OPENFILE,
                                    dialogFinished, state.release(), properties);
    SDL_DestroyProperties(properties);
}
#endif

} // namespace

std::string clipboardText()
{
#ifdef _WIN32
    if (!OpenClipboard(nullptr))
        return {};
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (!hData) {
        CloseClipboard();
        return {};
    }
    const auto *wideText = static_cast<const wchar_t *>(GlobalLock(hData));
    std::string result;
    if (wideText) {
        result = fromUtf16(wideText);
        GlobalUnlock(hData);
    }
    CloseClipboard();
    return result;
#else
    char *text = SDL_GetClipboardText();
    if (!text)
        return {};
    std::string result(text);
    SDL_free(text);
    return result;
#endif
}

void setClipboardText(const std::string &text)
{
#ifdef _WIN32
    if (!OpenClipboard(nullptr))
        return;
    EmptyClipboard();
    const std::wstring wide = toUtf16(text);
    const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem) {
        void *dest = GlobalLock(hMem);
        if (dest) {
            std::memcpy(dest, wide.c_str(), bytes);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        }
    }
    CloseClipboard();
#else
    SDL_SetClipboardText(text.c_str());
#endif
}

bool openUrl(const std::string &url)
{
#ifdef _WIN32
    const std::wstring wide = toUtf16(url);
    const auto res = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return res > 32;
#else
    return SDL_OpenURL(url.c_str());
#endif
}

bool applicationActive()
{
    return g_active;
}

Signal<bool> &activationChanged()
{
    return g_activeChanged;
}

void setApplicationActive(bool active)
{
    if (g_active != active) {
        g_active = active;
        g_activeChanged.publish(g_active);
    }
}

Point cursorPosition()
{
#ifdef _WIN32
    POINT pt;
    if (GetCursorPos(&pt))
        return { static_cast<float>(pt.x), static_cast<float>(pt.y) };
#else
    float x = 0.0f;
    float y = 0.0f;
    SDL_GetGlobalMouseState(&x, &y);
    return {x, y};
#endif
    return { 0.0f, 0.0f };
}

void openFiles(std::string title, std::vector<FileFilter> filters, FileCallback callback, bool multiple)
{
#ifdef _WIN32
    Dispatcher::main().post([title = std::move(title), filters = std::move(filters),
                             callback = std::move(callback), multiple]() {
        std::vector<std::string> results;
        IFileOpenDialog *pDialog = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&pDialog));
        if (SUCCEEDED(hr)) {
            DWORD dwFlags = 0;
            pDialog->GetOptions(&dwFlags);
            if (multiple)
                pDialog->SetOptions(dwFlags | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM);
            else
                pDialog->SetOptions(dwFlags | FOS_FORCEFILESYSTEM);

            if (!title.empty()) {
                const std::wstring wTitle = toUtf16(title);
                pDialog->SetTitle(wTitle.c_str());
            }

            std::vector<std::wstring> storage;
            auto specs = makeWin32Filters(filters, storage);
            if (!specs.empty())
                pDialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());

            hr = pDialog->Show(nullptr);
            if (SUCCEEDED(hr)) {
                IShellItemArray *pItems = nullptr;
                if (SUCCEEDED(pDialog->GetResults(&pItems))) {
                    DWORD count = 0;
                    pItems->GetCount(&count);
                    for (DWORD i = 0; i < count; ++i) {
                        IShellItem *pItem = nullptr;
                        if (SUCCEEDED(pItems->GetItemAt(i, &pItem))) {
                            PWSTR pPath = nullptr;
                            if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pPath)) && pPath) {
                                results.push_back(fromUtf16(pPath));
                                CoTaskMemFree(pPath);
                            }
                            pItem->Release();
                        }
                    }
                    pItems->Release();
                }
            }
            pDialog->Release();
        }
        if (callback)
            callback(std::move(results));
    });
#else
    Dispatcher::main().post([title = std::move(title), filters = std::move(filters),
                             callback = std::move(callback), multiple]() mutable {
        showDialog(false, title, {}, std::move(filters), std::move(callback), multiple);
    });
#endif
}

void saveFile(std::string title, std::string defaultName, FileCallback callback)
{
#ifdef _WIN32
    Dispatcher::main().post([title = std::move(title), defaultName = std::move(defaultName),
                             callback = std::move(callback)]() {
        std::vector<std::string> results;
        IFileSaveDialog *pDialog = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&pDialog));
        if (SUCCEEDED(hr)) {
            if (!title.empty()) {
                const std::wstring wTitle = toUtf16(title);
                pDialog->SetTitle(wTitle.c_str());
            }
            if (!defaultName.empty()) {
                const std::wstring wDef = toUtf16(defaultName);
                pDialog->SetFileName(wDef.c_str());
            }

            hr = pDialog->Show(nullptr);
            if (SUCCEEDED(hr)) {
                IShellItem *pItem = nullptr;
                if (SUCCEEDED(pDialog->GetResult(&pItem))) {
                    PWSTR pPath = nullptr;
                    if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pPath)) && pPath) {
                        results.push_back(fromUtf16(pPath));
                        CoTaskMemFree(pPath);
                    }
                    pItem->Release();
                }
            }
            pDialog->Release();
        }
        if (callback)
            callback(std::move(results));
    });
#else
    Dispatcher::main().post([title = std::move(title), defaultName = std::move(defaultName),
                             callback = std::move(callback)]() mutable {
        showDialog(true, title, defaultName, {}, std::move(callback), false);
    });
#endif
}

} // namespace Styx::Platform::Desktop
