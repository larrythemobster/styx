#include "UI/WindowsPttHook.hpp"
#include "UI/PttKeyState.hpp"

#include "Platform/Dispatcher.hpp"

#include <RmlUi/Core/Input.h>

#include <atomic>
#include <memory>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <future>
#include <thread>
#include <system_error>
#endif

namespace Styx::UI {

#ifdef _WIN32

namespace {

struct SharedState
{
    std::atomic<bool> active{false};
    std::function<void(bool)> callback;
};

} // namespace

struct WindowsPttHook::Impl
{
    std::shared_ptr<SharedState> shared;
    std::thread workerThread;
    DWORD threadId = 0;
    PttKeyState keyState{0, 0};
    std::string lastError;

    inline static thread_local Impl *currentHook = nullptr;

    static LRESULT CALLBACK keyboardProc(int code, WPARAM wParam, LPARAM lParam)
    {
        if (code == HC_ACTION && currentHook) {
            try {
                currentHook->processKeyEvent(wParam, lParam);
            } catch (...) {
                // Exceptions must not cross the Win32 callback boundary.
            }
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    void stop()
    {
        if (shared)
            shared->active.store(false, std::memory_order_release);

        if (threadId != 0) {
            PostThreadMessageW(threadId, WM_QUIT, 0, 0);
            threadId = 0;
        }

        if (workerThread.joinable())
            workerThread.join();
    }

    void processKeyEvent(WPARAM wParam, LPARAM lParam)
    {
        if (wParam != WM_KEYDOWN && wParam != WM_KEYUP && wParam != WM_SYSKEYDOWN && wParam != WM_SYSKEYUP)
            return;

        if (!lParam)
            return;

        const auto *kbd = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lParam);
        bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        bool extended = (kbd->flags & LLKHF_EXTENDED) != 0;

        int vk = normalizeVirtualKey(static_cast<int>(kbd->vkCode), static_cast<int>(kbd->scanCode), extended);

        int modifiers = 0;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000)
            modifiers |= Rml::ShortcutModifier::Primary;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
            modifiers |= Rml::ShortcutModifier::Shift;
        if (GetAsyncKeyState(VK_MENU) & 0x8000)
            modifiers |= Rml::ShortcutModifier::Alt;

        auto transition = keyState.update(vk, modifiers, down);
        if (transition.has_value()) {
            bool held = *transition;
            Platform::Dispatcher::main().post([state = this->shared, held]() {
                if (!state->active.load(std::memory_order_acquire))
                    return;
                if (state->callback)
                    state->callback(held);
            });
        }
    }
};


#else

struct WindowsPttHook::Impl
{
    std::string lastError;
};

#endif

WindowsPttHook::WindowsPttHook()
    : impl(std::make_unique<Impl>())
{
}

WindowsPttHook::~WindowsPttHook()
{
    stop();
}

bool WindowsPttHook::start(const Rml::KeyBinding &binding, std::function<void(bool)> callback)
{
#ifdef _WIN32
    stop();

    impl->lastError.clear();

    if (!binding.valid()) {
        impl->lastError = "Invalid key binding";
        return false;
    }

    int vk = virtualKey(binding.key);
    if (vk == 0) {
        impl->lastError = "Unsupported key binding";
        return false;
    }

    impl->keyState = PttKeyState(vk, binding.modifiers);
    impl->shared = std::make_shared<SharedState>();
    impl->shared->callback = std::move(callback);

    std::promise<DWORD> startup;
    auto installed = startup.get_future();
    try {
        impl->workerThread = std::thread([state = impl.get(), startup = std::move(startup)]() mutable {
            MSG msg{};
            PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
            Impl::currentHook = state;
            const HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, &Impl::keyboardProc, GetModuleHandleW(nullptr), 0);
            if (!hook) {
                const DWORD error = GetLastError();
                Impl::currentHook = nullptr;
                startup.set_value(error);
                return;
            }
            state->threadId = GetCurrentThreadId();
            state->shared->active.store(true, std::memory_order_release);
            startup.set_value(ERROR_SUCCESS);
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            UnhookWindowsHookEx(hook);
            Impl::currentHook = nullptr;
            // An unexpected message-loop exit must release an open microphone. Normal stop
            // already invalidated this state, so its queued callbacks cannot reach the owner.
            if (state->shared->active.load(std::memory_order_acquire)) {
                Platform::Dispatcher::main().post([shared = state->shared] {
                    if (shared->active.exchange(false, std::memory_order_acq_rel) && shared->callback)
                        shared->callback(false);
                });
            }
        });
    } catch (const std::system_error &error) {
        impl->lastError = "Unable to start keyboard hook thread: " + std::string(error.what());
        return false;
    }
    const DWORD error = installed.get();
    if (error != ERROR_SUCCESS) {
        impl->workerThread.join();
        impl->lastError = "Unable to install keyboard hook, error " + std::to_string(error);
        return false;
    }
    return true;
#else
    (void)binding;
    (void)callback;
    impl->lastError = "Global keyboard hook only available on Windows";
    return false;
#endif
}

void WindowsPttHook::stop()
{
#ifdef _WIN32
    impl->stop();
#endif
}

bool WindowsPttHook::active() const
{
#ifdef _WIN32
    return impl->shared && impl->shared->active.load(std::memory_order_acquire);
#else
    return false;
#endif
}

const std::string &WindowsPttHook::error() const
{
    return impl->lastError;
}

int WindowsPttHook::virtualKey(int rmlKey)
{
#ifdef _WIN32
    if (rmlKey >= ::Rml::Input::KI_A && rmlKey <= ::Rml::Input::KI_Z)
        return 'A' + (rmlKey - ::Rml::Input::KI_A);
    if (rmlKey >= ::Rml::Input::KI_0 && rmlKey <= ::Rml::Input::KI_9)
        return '0' + (rmlKey - ::Rml::Input::KI_0);
    if (rmlKey >= ::Rml::Input::KI_F1 && rmlKey <= ::Rml::Input::KI_F24)
        return VK_F1 + (rmlKey - ::Rml::Input::KI_F1);
    if (rmlKey >= ::Rml::Input::KI_NUMPAD0 && rmlKey <= ::Rml::Input::KI_NUMPAD9)
        return VK_NUMPAD0 + (rmlKey - ::Rml::Input::KI_NUMPAD0);

    switch (rmlKey) {
    case ::Rml::Input::KI_MULTIPLY:
        return VK_MULTIPLY;
    case ::Rml::Input::KI_ADD:
        return VK_ADD;
    case ::Rml::Input::KI_SEPARATOR:
        return VK_SEPARATOR;
    case ::Rml::Input::KI_SUBTRACT:
        return VK_SUBTRACT;
    case ::Rml::Input::KI_DECIMAL:
        return VK_DECIMAL;
    case ::Rml::Input::KI_DIVIDE:
        return VK_DIVIDE;
    case ::Rml::Input::KI_BACK:
        return VK_BACK;
    case ::Rml::Input::KI_TAB:
        return VK_TAB;
    case ::Rml::Input::KI_RETURN:
    // Windows virtual-key codes cannot distinguish between the main Enter key and the numeric keypad
    // Enter key (both produce VK_RETURN); WH_KEYBOARD_LL indicates numpad enter via the LLKHF_EXTENDED
    // flag rather than a distinct virtual key.
    case ::Rml::Input::KI_NUMPADENTER:
        return VK_RETURN;
    case ::Rml::Input::KI_SPACE:
        return VK_SPACE;
    case ::Rml::Input::KI_ESCAPE:
        return VK_ESCAPE;
    case ::Rml::Input::KI_DELETE:
        return VK_DELETE;
    case ::Rml::Input::KI_INSERT:
        return VK_INSERT;
    case ::Rml::Input::KI_HOME:
        return VK_HOME;
    case ::Rml::Input::KI_END:
        return VK_END;
    case ::Rml::Input::KI_PRIOR:
        return VK_PRIOR;
    case ::Rml::Input::KI_NEXT:
        return VK_NEXT;
    case ::Rml::Input::KI_UP:
        return VK_UP;
    case ::Rml::Input::KI_DOWN:
        return VK_DOWN;
    case ::Rml::Input::KI_LEFT:
        return VK_LEFT;
    case ::Rml::Input::KI_RIGHT:
        return VK_RIGHT;
    case ::Rml::Input::KI_LSHIFT:
        return VK_LSHIFT;
    case ::Rml::Input::KI_RSHIFT:
        return VK_RSHIFT;
    case ::Rml::Input::KI_LCONTROL:
        return VK_LCONTROL;
    case ::Rml::Input::KI_RCONTROL:
        return VK_RCONTROL;
    case ::Rml::Input::KI_LMENU:
        return VK_LMENU;
    case ::Rml::Input::KI_RMENU:
        return VK_RMENU;
    case ::Rml::Input::KI_LMETA:
        return VK_LWIN;
    case ::Rml::Input::KI_RMETA:
        return VK_RWIN;
    case ::Rml::Input::KI_OEM_1:
        return VK_OEM_1;
    case ::Rml::Input::KI_OEM_PLUS:
        return VK_OEM_PLUS;
    case ::Rml::Input::KI_OEM_COMMA:
        return VK_OEM_COMMA;
    case ::Rml::Input::KI_OEM_MINUS:
        return VK_OEM_MINUS;
    case ::Rml::Input::KI_OEM_PERIOD:
        return VK_OEM_PERIOD;
    case ::Rml::Input::KI_OEM_2:
        return VK_OEM_2;
    case ::Rml::Input::KI_OEM_3:
        return VK_OEM_3;
    case ::Rml::Input::KI_OEM_4:
        return VK_OEM_4;
    case ::Rml::Input::KI_OEM_5:
        return VK_OEM_5;
    case ::Rml::Input::KI_OEM_6:
        return VK_OEM_6;
    case ::Rml::Input::KI_OEM_7:
        return VK_OEM_7;
    case ::Rml::Input::KI_OEM_8:
        return VK_OEM_8;
    case ::Rml::Input::KI_OEM_102:
        return VK_OEM_102;
    default:
        return 0;
    }
#else
    (void)rmlKey;
    return 0;
#endif
}

int WindowsPttHook::normalizeVirtualKey(int virtualKey, int scanCode, bool extended)
{
#ifdef _WIN32
    switch (virtualKey) {
    case VK_CONTROL:
        return extended ? VK_RCONTROL : VK_LCONTROL;
    case VK_MENU:
        return extended ? VK_RMENU : VK_LMENU;
    case VK_SHIFT: {
        UINT mapped = MapVirtualKeyW(static_cast<UINT>(scanCode), MAPVK_VSC_TO_VK_EX);
        return mapped != 0 ? static_cast<int>(mapped) : virtualKey;
    }
    default:
        return virtualKey;
    }
#else
    (void)scanCode;
    (void)extended;
    return virtualKey;
#endif
}

} // namespace Styx::UI
