#pragma once

#include <string>
#include <vector>

struct ma_context;
struct ma_context_config;
struct ma_device;
struct ma_device_config;

namespace Styx {
namespace Core {
namespace Audio {

std::vector<std::string> supportedAudioBackends();

std::string configuredAudioBackend();
void setConfiguredAudioBackend(const std::string &name);

bool initAudioContext(ma_context *context, const ma_context_config *config);
bool initAudioDevice(const ma_device_config *config, ma_device *device);

} // namespace Audio
} // namespace Core
} // namespace Styx
