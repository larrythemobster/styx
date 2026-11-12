#pragma once

#include "Native.hpp"
#include "Signal.hpp"

#include <functional>
#include <string>
#include <vector>

namespace Styx::Platform::Desktop {

struct FileFilter {
    std::string name;
    std::string pattern;
};
using FileCallback = std::function<void(std::vector<std::string>)>;

std::string clipboardText();
void setClipboardText(const std::string &text);
bool openUrl(const std::string &url);
bool applicationActive();
Signal<bool> &activationChanged();
void setApplicationActive(bool active);
Point cursorPosition();
void openFiles(std::string title, std::vector<FileFilter> filters, FileCallback callback, bool multiple = true);
void saveFile(std::string title, std::string defaultName, FileCallback callback);

bool launchAtLoginSupported();
bool launchAtLoginEnabled();
bool configureLaunchAtLogin(bool enabled, bool minimized);

}
