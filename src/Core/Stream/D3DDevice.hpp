#pragma once

#ifdef _WIN32

#include <d3d11.h>
#include <wrl/client.h>

#include <memory>
#include <mutex>

namespace Styx {
namespace Core {
namespace Stream {

// One D3D11 device shared by capture, colour conversion and the hardware encoder, so a
// captured frame never leaves the GPU. Created with BGRA (Windows Graphics Capture) and
// video (ID3D11VideoProcessor) support, and multithread-protected because WGC, the
// converter and FFmpeg all touch the immediate context from different threads.
class D3DDevice
{
public:
    static std::shared_ptr<D3DDevice> create();

    [[nodiscard]] ID3D11Device *device() const { return d3dDevice.Get(); }
    [[nodiscard]] ID3D11DeviceContext *context() const { return d3dContext.Get(); }

    // Serialises immediate-context use between our threads (FFmpeg takes the same lock
    // through AVD3D11VADeviceContext::lock).
    std::recursive_mutex &mutex() { return contextMutex; }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext;
    std::recursive_mutex contextMutex;
};

} // namespace Stream
} // namespace Core
} // namespace Styx

#endif
