#include "FrameConverter.hpp"

#ifdef _WIN32

#include "D3DDevice.hpp"

#include "Core/Logging.hpp"

namespace Acheron {
namespace Core {
namespace Stream {

using Microsoft::WRL::ComPtr;

FrameConverter::FrameConverter(std::shared_ptr<D3DDevice> dev) : device(std::move(dev))
{
    device->device()->QueryInterface(IID_PPV_ARGS(&videoDevice));
    device->context()->QueryInterface(IID_PPV_ARGS(&videoContext));
}

FrameConverter::~FrameConverter() = default;

bool FrameConverter::ensureProcessor(const QSize &sourceSize, const QSize &targetSize)
{
    if (processor && processorSource == sourceSize && processorTarget == targetSize)
        return true;
    if (!videoDevice || !videoContext)
        return false;

    processor.Reset();
    enumerator.Reset();
    inputView.Reset();
    inputTexture = nullptr;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputWidth = static_cast<UINT>(sourceSize.width());
    desc.InputHeight = static_cast<UINT>(sourceSize.height());
    desc.OutputWidth = static_cast<UINT>(targetSize.width());
    desc.OutputHeight = static_cast<UINT>(targetSize.height());
    desc.InputFrameRate = { 60, 1 };
    desc.OutputFrameRate = { 60, 1 };
    desc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;

    if (FAILED(videoDevice->CreateVideoProcessorEnumerator(&desc, &enumerator))
        || FAILED(videoDevice->CreateVideoProcessor(enumerator.Get(), 0, &processor))) {
        qCWarning(LogStream) << "Could not create a D3D11 video processor for" << sourceSize << "->" << targetSize;
        processor.Reset();
        enumerator.Reset();
        return false;
    }

    // RGB full range in, BT.709 limited range out
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputSpace{};
    inputSpace.RGB_Range = 0;
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputSpace{};
    outputSpace.YCbCr_Matrix = 1;
    outputSpace.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    videoContext->VideoProcessorSetStreamColorSpace(processor.Get(), 0, &inputSpace);
    videoContext->VideoProcessorSetOutputColorSpace(processor.Get(), &outputSpace);
    videoContext->VideoProcessorSetStreamAutoProcessingMode(processor.Get(), 0, FALSE);

    // letterbox: fit the source inside the target, centred, black around it
    const QSize fitted = sourceSize.scaled(targetSize, Qt::KeepAspectRatio);
    const int x = (targetSize.width() - fitted.width()) / 2;
    const int y = (targetSize.height() - fitted.height()) / 2;
    RECT dest{ x & ~1, y & ~1, (x + fitted.width()) & ~1, (y + fitted.height()) & ~1 };
    RECT src{ 0, 0, sourceSize.width(), sourceSize.height() };
    RECT full{ 0, 0, targetSize.width(), targetSize.height() };
    videoContext->VideoProcessorSetStreamSourceRect(processor.Get(), 0, TRUE, &src);
    videoContext->VideoProcessorSetStreamDestRect(processor.Get(), 0, TRUE, &dest);
    videoContext->VideoProcessorSetOutputTargetRect(processor.Get(), TRUE, &full);
    // Black in RGB: the background colour is in the output space only on paper; AMD's
    // driver reads a YCbCr value as RGB, so (16, 128, 128) came out teal.
    D3D11_VIDEO_COLOR black{};
    black.RGBA = { 0.0f, 0.0f, 0.0f, 1.0f };
    videoContext->VideoProcessorSetOutputBackgroundColor(processor.Get(), FALSE, &black);

    processorSource = sourceSize;
    processorTarget = targetSize;
    for (auto &view : outputViews)
        view = {};
    return true;
}

bool FrameConverter::convert(ID3D11Texture2D *source, const QSize &sourceSize, ID3D11Texture2D *target, UINT targetIndex,
                             const QSize &targetSize)
{
    if (!source || !target || sourceSize.isEmpty() || !ensureProcessor(sourceSize, targetSize))
        return false;

    if (source != inputTexture || !inputView) {
        inputView.Reset();
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
        inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        if (FAILED(videoDevice->CreateVideoProcessorInputView(source, enumerator.Get(), &inputDesc, &inputView)))
            return false;
        inputTexture = source;
    }

    OutputView *output = nullptr;
    for (auto &view : outputViews) {
        if (view.texture == target && view.index == targetIndex && view.view) {
            output = &view;
            break;
        }
    }
    if (!output) {
        OutputView &slot = outputViews[nextOutputView];
        nextOutputView = (nextOutputView + 1) % static_cast<int>(std::size(outputViews));
        slot = {};
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
        D3D11_TEXTURE2D_DESC targetDesc{};
        target->GetDesc(&targetDesc);
        if (targetDesc.ArraySize > 1) {
            outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
            outputDesc.Texture2DArray.FirstArraySlice = targetIndex;
            outputDesc.Texture2DArray.ArraySize = 1;
        } else {
            outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        }
        if (FAILED(videoDevice->CreateVideoProcessorOutputView(target, enumerator.Get(), &outputDesc, &slot.view)))
            return false;
        slot.texture = target;
        slot.index = targetIndex;
        output = &slot;
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.Get();
    return SUCCEEDED(videoContext->VideoProcessorBlt(processor.Get(), output->view.Get(), 0, 1, &stream));
}

} // namespace Stream
} // namespace Core
} // namespace Acheron

#endif
