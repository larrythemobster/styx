#include "D3DDevice.hpp"

#ifdef _WIN32

#include "Core/Logging.hpp"

#include <d3d10.h>

namespace Acheron {
namespace Core {
namespace Stream {

std::shared_ptr<D3DDevice> D3DDevice::create()
{
    auto result = std::shared_ptr<D3DDevice>(new D3DDevice);

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION, &result->d3dDevice, nullptr, &result->d3dContext);
    if (FAILED(hr)) {
        qCWarning(LogStream) << "D3D11CreateDevice failed:" << Qt::hex << static_cast<quint32>(hr);
        return nullptr;
    }

    Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(result->d3dContext.As(&multithread)))
        multithread->SetMultithreadProtected(TRUE);

    return result;
}

} // namespace Stream
} // namespace Core
} // namespace Acheron

#endif
