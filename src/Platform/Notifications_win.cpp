#include "Notifications.hpp"

#include "Dispatcher.hpp"
#include "Native.hpp"
#include "Utf8.hpp"

#include <windows.h>
#include <shobjidl.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>

#include <deque>
#include <filesystem>
#include <string>
#include <utility>

namespace Styx::Platform::Notifications {

namespace {

namespace toast = winrt::Windows::UI::Notifications;

// The same id SDL_SetAppMetadata uses in main.cpp, so toasts and the taskbar group together.
constexpr wchar_t APP_ID[] = L"io.github.larrythemobster.styx";

// A toast only raises Activated while its object is alive, so the recent ones are kept.
constexpr std::size_t KEPT_TOASTS = 32;

std::deque<toast::ToastNotification> &keptToasts()
{
    static std::deque<toast::ToastNotification> toasts;
    return toasts;
}

// Windows only shows toasts from an unpackaged app whose id is registered.
bool registerApp()
{
    static const bool registered = [] {
        try {
            winrt::init_apartment(winrt::apartment_type::single_threaded);
        } catch (const winrt::hresult_error &error) {
            if (error.code() != RPC_E_CHANGED_MODE)
                return false;
        }

        const std::wstring key = std::wstring(L"Software\\Classes\\AppUserModelId\\") + APP_ID;
        HKEY handle = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr) != ERROR_SUCCESS)
            return false;

        auto setString = [handle](const wchar_t *name, const std::wstring &value) {
            RegSetValueExW(handle, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        };
        setString(L"DisplayName", L"Styx");
        const std::filesystem::path icon = Resources::path("app/styx.ico");
        std::error_code error;
        if (std::filesystem::exists(icon, error))
            setString(L"IconUri", icon.wstring());
        RegCloseKey(handle);

        return SUCCEEDED(SetCurrentProcessExplicitAppUserModelID(APP_ID));
    }();
    return registered;
}

toast::ToastNotifier notifier()
{
    static toast::ToastNotifier instance = toast::ToastNotificationManager::CreateToastNotifier(APP_ID);
    return instance;
}

} // namespace

bool available()
{
    if (!registerApp())
        return false;
    try {
        return notifier().Setting() == toast::NotificationSetting::Enabled;
    } catch (const winrt::hresult_error &error) {
        // An unpackaged app has no notification setting until its first toast, and Windows
        // reports that as "element not found" even though toasts show.
        return error.code() == HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
}

bool show(const Notification &notification, ClickHandler onClick)
{
    if (!registerApp())
        return false;
    try {
        winrt::Windows::Data::Xml::Dom::XmlDocument document;
        document.LoadXml(toUtf16(toastXml(notification)));
        toast::ToastNotification shown(document);
        // Activated arrives on a system thread.
        shown.Activated([onClick = std::move(onClick)](const auto &, const auto &) {
            if (onClick)
                Dispatcher::main().post(onClick);
        });
        notifier().Show(shown);
        auto &toasts = keptToasts();
        toasts.push_back(shown);
        if (toasts.size() > KEPT_TOASTS)
            toasts.pop_front();
        return true;
    } catch (const winrt::hresult_error &) {
        return false;
    }
}

} // namespace Styx::Platform::Notifications
