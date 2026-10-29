#include "IAudioBackend.hpp"
#include "MiniaudioAudioBackend.hpp"

namespace Styx {
namespace Core {
namespace Audio {

std::unique_ptr<IAudioBackend> IAudioBackend::create()
{
    return std::make_unique<MiniaudioAudioBackend>();
}

} // namespace Audio
} // namespace Core
} // namespace Styx
