#include "Notifications.hpp"

#include "Native.hpp"

#include <string>
#include <string_view>

namespace Styx::Platform::Notifications {

namespace {

std::string xmlText(std::string_view text)
{
    std::string filtered;
    filtered.reserve(text.size());
    for (char c : text) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x20 && uc != 0x09 && uc != 0x0A && uc != 0x0D)
            continue;
        filtered.push_back(c);
    }
    return htmlEscape(filtered);
}

} // namespace

std::string toastXml(const Notification &notification)
{
    std::string xml = "<toast><visual><binding template=\"ToastGeneric\"><text>";
    xml += xmlText(notification.title);
    xml += "</text><text>";
    xml += xmlText(notification.body);
    xml += "</text>";
    if (!notification.image.empty()) {
        xml += "<image placement=\"appLogoOverride\" hint-crop=\"circle\" src=\"";
        xml += htmlEscape(fileUrl(notification.image));
        xml += "\"/>";
    }
    xml += "</binding></visual></toast>";
    return xml;
}

} // namespace Styx::Platform::Notifications
