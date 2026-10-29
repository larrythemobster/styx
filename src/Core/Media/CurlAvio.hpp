#pragma once

#ifdef STYX_HAVE_FFMPEG

#include <string>

#include "Core/ProxyConfig.hpp"

extern "C" {
#include <libavformat/avio.h>
}

namespace Styx {
namespace Core {
namespace Media {

namespace CurlAvio {

AVIOContext *create(const std::string &url, const ProxyConfig &proxy, const AVIOInterruptCB &interrupt);
void destroy(AVIOContext *context);

[[nodiscard]] bool handlesScheme(const std::string &url);

} // namespace CurlAvio

} // namespace Media
} // namespace Core
} // namespace Styx

#endif // STYX_HAVE_FFMPEG
