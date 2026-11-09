#include "UI/X11PttHook.hpp"

#include "Platform/Dispatcher.hpp"
#include "UI/PttKeyState.hpp"

#include <RmlUi/Core/Input.h>

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#if defined(__linux__) && defined(STYX_HAVE_X11_PTT)
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XInput2.h>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <bitset>
#include <cerrno>
#include <future>
#include <system_error>
#include <thread>
#endif

namespace Styx::UI {

namespace {

struct SharedState
{
    std::atomic<bool> active{false};
    std::function<void(bool)> callback;
};

} // namespace

#if defined(__linux__) && defined(STYX_HAVE_X11_PTT)

struct X11PttHook::Impl
{
    std::shared_ptr<SharedState> shared;
    std::thread workerThread;
    int wakeReadFd = -1;
    int wakeWriteFd = -1;
    std::string lastError;

    void stop()
    {
        if (shared)
            shared->active.store(false, std::memory_order_release);

        if (wakeWriteFd != -1) {
            char b = 1;
            while (::write(wakeWriteFd, &b, 1) < 0 && errno == EINTR) {}
        }

        if (workerThread.joinable())
            workerThread.join();

        if (wakeReadFd != -1) {
            ::close(wakeReadFd);
            wakeReadFd = -1;
        }
        if (wakeWriteFd != -1) {
            ::close(wakeWriteFd);
            wakeWriteFd = -1;
        }
    }
};

#else

struct X11PttHook::Impl
{
    std::string lastError;
};

#endif

X11PttHook::X11PttHook()
    : impl(std::make_unique<Impl>())
{
}

X11PttHook::~X11PttHook()
{
    stop();
}

bool X11PttHook::start(const Rml::KeyBinding &binding, std::function<void(bool)> callback,
                       std::string_view videoDriver)
{
#if defined(__linux__) && defined(STYX_HAVE_X11_PTT)
    stop();

    impl->lastError.clear();

    if (!binding.valid()) {
        impl->lastError = "Invalid key binding";
        return false;
    }

    if (videoDriver == "wayland") {
        impl->lastError = "Push-to-talk requires application focus under Wayland";
        return false;
    }

    if (videoDriver != "x11") {
        impl->lastError = "Unsupported video driver for X11 hook";
        return false;
    }

    unsigned long sym = keySym(binding.key);
    if (sym == 0) {
        impl->lastError = "Unsupported key binding";
        return false;
    }

    int pipeFds[2] = {-1, -1};
    if (pipe2(pipeFds, O_CLOEXEC | O_NONBLOCK) != 0) {
        impl->lastError = "Failed to create wake pipe";
        return false;
    }
    impl->wakeReadFd = pipeFds[0];
    impl->wakeWriteFd = pipeFds[1];

    impl->shared = std::make_shared<SharedState>();
    impl->shared->callback = std::move(callback);

    std::promise<std::string> startup;
    auto installed = startup.get_future();
    try {
        impl->workerThread = std::thread([shared = impl->shared,
                                           wakeReadFd = impl->wakeReadFd,
                                           binding,
                                           sym,
                                           startup = std::move(startup)]() mutable {
            Display *display = XOpenDisplay(nullptr);
            if (!display) {
                startup.set_value("Unable to open X11 display");
                return;
            }

            int xiOpcode = 0;
            int xiEvent = 0;
            int xiError = 0;
            if (!XQueryExtension(display, "XInputExtension", &xiOpcode, &xiEvent, &xiError)) {
                XCloseDisplay(display);
                startup.set_value("XInput extension not available");
                return;
            }

            int major = 2;
            int minor = 2;
            if (XIQueryVersion(display, &major, &minor) != Success || major < 2 || (major == 2 && minor < 2)) {
                XCloseDisplay(display);
                startup.set_value("XInput 2.2 or higher is required");
                return;
            }

            KeyCode targetKeycode = XKeysymToKeycode(display, sym);
            if (targetKeycode == 0) {
                XCloseDisplay(display);
                startup.set_value("Unsupported key binding or unmapped key");
                return;
            }

            KeyCode ctrlL = XKeysymToKeycode(display, XK_Control_L);
            KeyCode ctrlR = XKeysymToKeycode(display, XK_Control_R);
            KeyCode shiftL = XKeysymToKeycode(display, XK_Shift_L);
            KeyCode shiftR = XKeysymToKeycode(display, XK_Shift_R);
            KeyCode altL = XKeysymToKeycode(display, XK_Alt_L);
            KeyCode altR = XKeysymToKeycode(display, XK_Alt_R);

            std::bitset<256> pressedKeys;
            char keymap[32] = {0};
            XQueryKeymap(display, keymap);
            for (int i = 0; i < 256; ++i) {
                if (keymap[i >> 3] & (1 << (i & 7)))
                    pressedKeys.set(i);
            }
            auto isHeld = [&pressedKeys](KeyCode key) {
                return key != 0 && pressedKeys.test(key);
            };

            Bool supported = False;
            XkbSetDetectableAutoRepeat(display, True, &supported);

            XIEventMask eventMask;
            unsigned char mask[XIMaskLen(XI_RawKeyRelease)] = {0};
            eventMask.deviceid = XIAllMasterDevices;
            eventMask.mask_len = sizeof(mask);
            eventMask.mask = mask;
            XISetMask(mask, XI_RawKeyPress);
            XISetMask(mask, XI_RawKeyRelease);

            if (XISelectEvents(display, DefaultRootWindow(display), &eventMask, 1) != Success) {
                XCloseDisplay(display);
                startup.set_value("Unable to select XInput raw keyboard events");
                return;
            }
            XSync(display, False);

            PttKeyState keyState(targetKeycode, binding.modifiers);
            int x11Fd = ConnectionNumber(display);

            shared->active.store(true, std::memory_order_release);
            startup.set_value({});

            while (shared->active.load(std::memory_order_acquire)) {
                struct pollfd fds[2];
                fds[0].fd = x11Fd;
                fds[0].events = POLLIN;
                fds[0].revents = 0;
                fds[1].fd = wakeReadFd;
                fds[1].events = POLLIN;
                fds[1].revents = 0;

                int ret = poll(fds, 2, -1);
                if (ret < 0) {
                    if (errno == EINTR)
                        continue;
                    break;
                }

                if ((fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) ||
                    (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL | POLLIN)))
                    break;

                if (fds[0].revents & POLLIN) {
                    while (XPending(display) > 0) {
                        XEvent event;
                        XNextEvent(display, &event);
                        if (event.type == GenericEvent && event.xcookie.extension == xiOpcode) {
                            if (XGetEventData(display, &event.xcookie)) {
                                if (event.xcookie.evtype == XI_RawKeyPress || event.xcookie.evtype == XI_RawKeyRelease) {
                                    const auto *raw = static_cast<const XIRawEvent *>(event.xcookie.data);
                                    bool isRelease = (event.xcookie.evtype == XI_RawKeyRelease);
                                    bool isRepeat = (raw->flags & XIKeyRepeat) != 0;

                                    if (!isRelease || !isRepeat) {
                                        if (raw->detail >= 0 && raw->detail < 256) {
                                            if (isRelease)
                                                pressedKeys.reset(raw->detail);
                                            else
                                                pressedKeys.set(raw->detail);
                                        }

                                        int currentModifiers = 0;
                                        if (isHeld(ctrlL) || isHeld(ctrlR))
                                            currentModifiers |= Rml::ShortcutModifier::Primary;
                                        if (isHeld(shiftL) || isHeld(shiftR))
                                            currentModifiers |= Rml::ShortcutModifier::Shift;
                                        if (isHeld(altL) || isHeld(altR))
                                            currentModifiers |= Rml::ShortcutModifier::Alt;

                                        auto transition = keyState.update(raw->detail, currentModifiers, !isRelease);
                                        if (transition.has_value()) {
                                            bool held = *transition;
                                            Platform::Dispatcher::main().post([shared, held]() {
                                                if (!shared->active.load(std::memory_order_acquire))
                                                    return;
                                                if (shared->callback)
                                                    shared->callback(held);
                                            });
                                        }
                                    }
                                }
                                XFreeEventData(display, &event.xcookie);
                            }
                        }
                        if (!shared->active.load(std::memory_order_acquire))
                            break;
                    }
                }
            }

            if (shared->active.load(std::memory_order_acquire)) {
                Platform::Dispatcher::main().post([shared] {
                    if (shared->active.exchange(false, std::memory_order_acq_rel) && shared->callback)
                        shared->callback(false);
                });
            }

            XCloseDisplay(display);
        });
    } catch (const std::system_error &error) {
        ::close(impl->wakeReadFd);
        ::close(impl->wakeWriteFd);
        impl->wakeReadFd = -1;
        impl->wakeWriteFd = -1;
        impl->lastError = "Unable to start keyboard hook thread: " + std::string(error.what());
        return false;
    }

    std::string err = installed.get();
    if (!err.empty()) {
        impl->stop();
        impl->lastError = std::move(err);
        return false;
    }
    return true;
#else
    (void)binding;
    (void)callback;
    (void)videoDriver;
    impl->lastError = "Global keyboard hook only available on Linux with X11";
    return false;
#endif
}

void X11PttHook::stop()
{
#if defined(__linux__) && defined(STYX_HAVE_X11_PTT)
    impl->stop();
#endif
}

bool X11PttHook::active() const
{
#if defined(__linux__) && defined(STYX_HAVE_X11_PTT)
    return impl->shared && impl->shared->active.load(std::memory_order_acquire);
#else
    return false;
#endif
}

const std::string &X11PttHook::error() const
{
    return impl->lastError;
}

unsigned long X11PttHook::keySym(int rmlKey)
{
#if defined(__linux__) && defined(STYX_HAVE_X11_PTT)
    if (rmlKey >= ::Rml::Input::KI_A && rmlKey <= ::Rml::Input::KI_Z)
        return XK_a + (rmlKey - ::Rml::Input::KI_A);
    if (rmlKey >= ::Rml::Input::KI_0 && rmlKey <= ::Rml::Input::KI_9)
        return XK_0 + (rmlKey - ::Rml::Input::KI_0);
    if (rmlKey >= ::Rml::Input::KI_F1 && rmlKey <= ::Rml::Input::KI_F24)
        return XK_F1 + (rmlKey - ::Rml::Input::KI_F1);
    if (rmlKey >= ::Rml::Input::KI_NUMPAD0 && rmlKey <= ::Rml::Input::KI_NUMPAD9)
        return XK_KP_0 + (rmlKey - ::Rml::Input::KI_NUMPAD0);

    switch (rmlKey) {
    case ::Rml::Input::KI_MULTIPLY:
        return XK_KP_Multiply;
    case ::Rml::Input::KI_ADD:
        return XK_KP_Add;
    case ::Rml::Input::KI_SEPARATOR:
        return XK_KP_Separator;
    case ::Rml::Input::KI_SUBTRACT:
        return XK_KP_Subtract;
    case ::Rml::Input::KI_DECIMAL:
        return XK_KP_Decimal;
    case ::Rml::Input::KI_DIVIDE:
        return XK_KP_Divide;
    case ::Rml::Input::KI_BACK:
        return XK_BackSpace;
    case ::Rml::Input::KI_TAB:
        return XK_Tab;
    case ::Rml::Input::KI_RETURN:
        return XK_Return;
    case ::Rml::Input::KI_NUMPADENTER:
        return XK_KP_Enter;
    case ::Rml::Input::KI_SPACE:
        return XK_space;
    case ::Rml::Input::KI_ESCAPE:
        return XK_Escape;
    case ::Rml::Input::KI_DELETE:
        return XK_Delete;
    case ::Rml::Input::KI_INSERT:
        return XK_Insert;
    case ::Rml::Input::KI_HOME:
        return XK_Home;
    case ::Rml::Input::KI_END:
        return XK_End;
    case ::Rml::Input::KI_PRIOR:
        return XK_Page_Up;
    case ::Rml::Input::KI_NEXT:
        return XK_Page_Down;
    case ::Rml::Input::KI_UP:
        return XK_Up;
    case ::Rml::Input::KI_DOWN:
        return XK_Down;
    case ::Rml::Input::KI_LEFT:
        return XK_Left;
    case ::Rml::Input::KI_RIGHT:
        return XK_Right;
    case ::Rml::Input::KI_LCONTROL:
        return XK_Control_L;
    case ::Rml::Input::KI_RCONTROL:
        return XK_Control_R;
    case ::Rml::Input::KI_LSHIFT:
        return XK_Shift_L;
    case ::Rml::Input::KI_RSHIFT:
        return XK_Shift_R;
    case ::Rml::Input::KI_LMENU:
        return XK_Alt_L;
    case ::Rml::Input::KI_RMENU:
        return XK_Alt_R;
    case ::Rml::Input::KI_LMETA:
    case ::Rml::Input::KI_LWIN:
        return XK_Super_L;
    case ::Rml::Input::KI_RMETA:
    case ::Rml::Input::KI_RWIN:
        return XK_Super_R;
    case ::Rml::Input::KI_OEM_1:
        return XK_semicolon;
    case ::Rml::Input::KI_OEM_PLUS:
        return XK_equal;
    case ::Rml::Input::KI_OEM_COMMA:
        return XK_comma;
    case ::Rml::Input::KI_OEM_MINUS:
        return XK_minus;
    case ::Rml::Input::KI_OEM_PERIOD:
        return XK_period;
    case ::Rml::Input::KI_OEM_2:
        return XK_slash;
    case ::Rml::Input::KI_OEM_3:
        return XK_grave;
    case ::Rml::Input::KI_OEM_4:
        return XK_bracketleft;
    case ::Rml::Input::KI_OEM_5:
        return XK_backslash;
    case ::Rml::Input::KI_OEM_6:
        return XK_bracketright;
    case ::Rml::Input::KI_OEM_7:
        return XK_apostrophe;
    case ::Rml::Input::KI_OEM_102:
        return XK_less;
    default:
        return 0;
    }
#else
    (void)rmlKey;
    return 0;
#endif
}

} // namespace Styx::UI
