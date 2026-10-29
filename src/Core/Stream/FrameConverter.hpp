#pragma once

#ifdef _WIN32

#include "Platform/Native.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <memory>

namespace Styx {
namespace Core {
namespace Stream {

class D3DDevice;

// BGRA capture texture -> NV12 encoder surface on the GPU's video engine
// (ID3D11VideoProcessor): scales to the stream resolution, keeps the aspect ratio with
// black bars, and converts to BT.709 limited range, which is what every H264 decoder
// assumes when the bitstream does not say otherwise.
class FrameConverter
{
public:
    explicit FrameConverter(std::shared_ptr<D3DDevice> device);
    ~FrameConverter();

    // Caller holds the device mutex. target is an NV12 texture (array slice targetIndex)
    // created with D3D11_BIND_RENDER_TARGET.
    bool convert(ID3D11Texture2D *source, const Platform::Size &sourceSize, ID3D11Texture2D *target, UINT targetIndex,
                 const Platform::Size &targetSize);

private:
    bool ensureProcessor(const Platform::Size &sourceSize, const Platform::Size &targetSize);

    std::shared_ptr<D3DDevice> device;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> videoContext;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor;
    Platform::Size processorSource;
    Platform::Size processorTarget;

    // views are cached per texture: the encoder pool cycles through a handful of slices
    struct OutputView
    {
        ID3D11Texture2D *texture = nullptr;
        UINT index = 0;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> view;
    };
    OutputView outputViews[16];
    int nextOutputView = 0;
    ID3D11Texture2D *inputTexture = nullptr;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inputView;
};

} // namespace Stream
} // namespace Core
} // namespace Styx

#endif
