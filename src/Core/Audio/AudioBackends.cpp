#include "Core/Audio/AudioBackends.hpp"
#include "Core/Audio/Miniaudio.hpp"
#include "Core/Logging.hpp"

#include "Platform/Settings.hpp"

namespace Styx {
namespace Core {
namespace Audio {

namespace {

constexpr auto BackendKey = "audio/backend";

std::vector<ma_backend> enabledBackends()
{
    ma_backend backends[MA_BACKEND_COUNT];
    size_t count = 0;
    if (ma_get_enabled_backends(backends, MA_BACKEND_COUNT, &count) != MA_SUCCESS)
        return {};

    std::vector<ma_backend> result;
    for (size_t i = 0; i < count; i++) {
        if (backends[i] == ma_backend_custom || backends[i] == ma_backend_null)
            continue;
        result.push_back(backends[i]);
    }
    return result;
}

bool resolveBackend(const std::string &name, ma_backend &out)
{
    if (name.empty())
        return false;

    for (ma_backend backend : enabledBackends()) {
        if (name == ma_get_backend_name(backend)) {
            out = backend;
            return true;
        }
    }

    return false;
}

} // namespace

std::vector<std::string> supportedAudioBackends()
{
    static const std::vector<std::string> cached = []() {
        std::vector<std::string> names;
        for (ma_backend backend : enabledBackends()) {
            ma_context context;
            if (ma_context_init(&backend, 1, nullptr, &context) != MA_SUCCESS)
                continue;
            ma_context_uninit(&context);
            names.emplace_back(ma_get_backend_name(backend));
        }
        return names;
    }();

    return cached;
}

std::string configuredAudioBackend()
{
    return Platform::Settings::instance().getString(BackendKey);
}

void setConfiguredAudioBackend(const std::string &name)
{
    auto &settings = Platform::Settings::instance();
    if (name.empty())
        settings.remove(BackendKey);
    else
        settings.setString(BackendKey, name);
}

bool initAudioContext(ma_context *context, const ma_context_config *config)
{
    const std::string name = configuredAudioBackend();

    ma_backend backend;
    if (resolveBackend(name, backend)) {
        if (ma_context_init(&backend, 1, config, context) == MA_SUCCESS)
            return true;
        log(LogLevel::Warning, LogMiniaudio) << "Audio backend" << name << "failed to initialize, falling back to the default";
    }

    return ma_context_init(nullptr, 0, config, context) == MA_SUCCESS;
}

bool initAudioDevice(const ma_device_config *config, ma_device *device)
{
    const std::string name = configuredAudioBackend();

    ma_backend backend;
    if (resolveBackend(name, backend)) {
        if (ma_device_init_ex(&backend, 1, nullptr, config, device) == MA_SUCCESS)
            return true;
        log(LogLevel::Warning, LogMiniaudio) << "Audio backend" << name << "failed to initialize, falling back to the default";
    }

    return ma_device_init_ex(nullptr, 0, nullptr, config, device) == MA_SUCCESS;
}

} // namespace Audio
} // namespace Core
} // namespace Styx
