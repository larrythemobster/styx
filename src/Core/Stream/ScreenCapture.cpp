#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>

#include <Windows.Graphics.Capture.Interop.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include "ScreenCapture.hpp"

#include "D3DDevice.hpp"

#include "Core/Logging.hpp"

#include "Platform/Utf8.hpp"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <stdexcept>

namespace Styx {
namespace Core {
namespace Stream {

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;
using Microsoft::WRL::ComPtr;

bool isScreenCaptureSupported()
{
    try {
        return wgc::GraphicsCaptureSession::IsSupported();
    } catch (...) {
        return false;
    }
}

namespace {

BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM param)
{
    auto *sources = reinterpret_cast<std::vector<CaptureSource> *>(param);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info))
        return TRUE;

    CaptureSource source;
    source.kind = CaptureSource::Kind::Monitor;
    source.handle = reinterpret_cast<uintptr_t>(monitor);
    source.geometry = {static_cast<float>(info.rcMonitor.left), static_cast<float>(info.rcMonitor.top),
                       static_cast<float>(info.rcMonitor.right - info.rcMonitor.left),
                       static_cast<float>(info.rcMonitor.bottom - info.rcMonitor.top)};
    source.primary = info.dwFlags & MONITORINFOF_PRIMARY;
    source.title = "Screen " + std::to_string(sources->size() + 1) + " (" +
                   std::to_string(static_cast<int>(source.geometry.width)) + " × " +
                   std::to_string(static_cast<int>(source.geometry.height)) + ")";
    sources->push_back(std::move(source));
    return TRUE;
}

bool isCapturableWindow(HWND hwnd)
{
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr)
        return false;
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW)
        return false;
    if (GetWindowTextLengthW(hwnd) == 0)
        return false;

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return false;

    wchar_t className[64]{};
    GetClassNameW(hwnd, className, 64);
    const std::wstring_view cls(className);
    if (cls == L"Progman" || cls == L"WorkerW" || cls == L"Shell_TrayWnd")
        return false;
    return true;
}

BOOL CALLBACK collectWindow(HWND hwnd, LPARAM param)
{
    if (!isCapturableWindow(hwnd))
        return TRUE;
    auto *sources = reinterpret_cast<std::vector<CaptureSource> *>(param);

    const int length = GetWindowTextLengthW(hwnd);
    std::wstring title(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(hwnd, title.data(), length + 1);

    RECT rect{};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect))))
        GetWindowRect(hwnd, &rect);

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    CaptureSource source;
    source.kind = CaptureSource::Kind::Window;
    source.handle = reinterpret_cast<uintptr_t>(hwnd);
    source.title = Platform::fromUtf16(std::wstring_view(title.data(), length));
    source.geometry = {static_cast<float>(rect.left), static_cast<float>(rect.top),
                       static_cast<float>(rect.right - rect.left), static_cast<float>(rect.bottom - rect.top)};
    source.processId = pid;
    if (source.geometry.width > 1 && source.geometry.height > 1)
        sources->push_back(std::move(source));
    return TRUE;
}

Platform::ImageBuffer imageFromDc(HDC sourceDc, int srcX, int srcY, int srcW, int srcH, const Platform::Size &target, bool stretch)
{
    HDC memDc = CreateCompatibleDC(sourceDc);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = target.width;
    bmi.bmiHeader.biHeight = -target.height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) {
        DeleteDC(memDc);
        return {};
    }
    HGDIOBJ old = SelectObject(memDc, bitmap);
    if (stretch) {
        SetStretchBltMode(memDc, HALFTONE);
        StretchBlt(memDc, 0, 0, target.width, target.height, sourceDc, srcX, srcY, srcW, srcH, SRCCOPY);
    } else {
        BitBlt(memDc, 0, 0, target.width, target.height, sourceDc, srcX, srcY, SRCCOPY);
    }
    Platform::ImageBuffer image{target.width, target.height, 4,
        std::vector<uint8_t>(static_cast<size_t>(target.width) * target.height * 4)};
    const auto *src = static_cast<const uint8_t *>(bits);
    for (size_t i = 0; i < image.pixels.size(); i += 4) {
        image.pixels[i] = src[i + 2];
        image.pixels[i + 1] = src[i + 1];
        image.pixels[i + 2] = src[i];
        image.pixels[i + 3] = 255;
    }
    SelectObject(memDc, old);
    DeleteObject(bitmap);
    DeleteDC(memDc);
    return image;
}

} // namespace

std::vector<CaptureSource> enumerateCaptureSources()
{
    std::vector<CaptureSource> sources;
    EnumDisplayMonitors(nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&sources));
    EnumWindows(collectWindow, reinterpret_cast<LPARAM>(&sources));
    return sources;
}

Platform::ImageBuffer captureThumbnail(const CaptureSource &source, const Platform::Size &maxSize)
{
    auto scaled = Platform::fitWithin({static_cast<int>(source.geometry.width), static_cast<int>(source.geometry.height)}, maxSize);
    scaled.width = std::max(1, scaled.width);
    scaled.height = std::max(1, scaled.height);

    if (source.kind == CaptureSource::Kind::Monitor) {
        HDC screen = GetDC(nullptr);
        auto image = imageFromDc(screen, static_cast<int>(source.geometry.x), static_cast<int>(source.geometry.y),
                                 static_cast<int>(source.geometry.width), static_cast<int>(source.geometry.height), scaled, true);
        ReleaseDC(nullptr, screen);
        return image;
    }

    // PrintWindow with PW_RENDERFULLCONTENT also renders DirectComposition/GPU content
    auto hwnd = reinterpret_cast<HWND>(source.handle);
    RECT client{};
    GetWindowRect(hwnd, &client);
    const Platform::Size full{client.right - client.left, client.bottom - client.top};
    if (full.empty())
        return {};

    HDC windowDc = GetDC(hwnd);
    HDC fullDc = CreateCompatibleDC(windowDc);
    HBITMAP fullBitmap = CreateCompatibleBitmap(windowDc, full.width, full.height);
    HGDIOBJ old = SelectObject(fullDc, fullBitmap);
    constexpr UINT PW_RENDERFULLCONTENT_FLAG = 0x00000002;
    PrintWindow(hwnd, fullDc, PW_RENDERFULLCONTENT_FLAG);
    auto fitted = Platform::fitWithin(full, maxSize);
    fitted.width = std::max(1, fitted.width);
    fitted.height = std::max(1, fitted.height);
    auto image = imageFromDc(fullDc, 0, 0, full.width, full.height, fitted, true);
    SelectObject(fullDc, old);
    DeleteObject(fullBitmap);
    DeleteDC(fullDc);
    ReleaseDC(hwnd, windowDc);
    return image;
}

struct ScreenCapture::Impl
{
    std::shared_ptr<D3DDevice> device;
    wdx::Direct3D11::IDirect3DDevice rtDevice{ nullptr };
    wgc::GraphicsCaptureItem item{ nullptr };
    wgc::Direct3D11CaptureFramePool pool{ nullptr };
    wgc::GraphicsCaptureSession session{ nullptr };
    wgc::Direct3D11CaptureFramePool::FrameArrived_revoker frameRevoker;
    wgc::GraphicsCaptureItem::Closed_revoker closedRevoker;
    winrt::Windows::Graphics::SizeInt32 poolSize{};

    std::mutex frameMutex;
    ComPtr<ID3D11Texture2D> latest;
    Platform::Size latestSize;
    uint64_t sequence = 0;
    std::atomic<bool> closed{ false };

    void onFrameArrived(const wgc::Direct3D11CaptureFramePool &sender);
};

void ScreenCapture::Impl::onFrameArrived(const wgc::Direct3D11CaptureFramePool &sender)
{
    auto frame = sender.TryGetNextFrame();
    if (!frame)
        return;

    const auto contentSize = frame.ContentSize();
    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    ComPtr<ID3D11Texture2D> surface;
    if (FAILED(access->GetInterface(IID_PPV_ARGS(&surface))))
        return;

    D3D11_TEXTURE2D_DESC surfaceDesc{};
    surface->GetDesc(&surfaceDesc);
    const int width = std::min<int>(contentSize.Width, static_cast<int>(surfaceDesc.Width));
    const int height = std::min<int>(contentSize.Height, static_cast<int>(surfaceDesc.Height));
    if (width <= 0 || height <= 0)
        return;

    {
        std::lock_guard deviceLock(device->mutex());
        std::lock_guard lock(frameMutex);
        if (!latest || latestSize != Platform::Size{width, height}) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = static_cast<UINT>(width);
            desc.Height = static_cast<UINT>(height);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            latest.Reset();
            if (FAILED(device->device()->CreateTexture2D(&desc, nullptr, &latest)))
                return;
            latestSize = {width, height};
        }
        D3D11_BOX box{ 0, 0, 0, static_cast<UINT>(width), static_cast<UINT>(height), 1 };
        device->context()->CopySubresourceRegion(latest.Get(), 0, 0, 0, 0, surface.Get(), 0, &box);
        sequence++;
    }

    // Content larger than the pool surfaces (a window grown past the whole desktop): grow
    // the pool. Never shrink it; a smaller window is copied out of the corner.
    if (contentSize.Width > poolSize.Width || contentSize.Height > poolSize.Height) {
        poolSize = { std::max(contentSize.Width, poolSize.Width), std::max(contentSize.Height, poolSize.Height) };
        sender.Recreate(rtDevice, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, poolSize);
    }
}

ScreenCapture::ScreenCapture(std::shared_ptr<D3DDevice> device) : impl(std::make_unique<Impl>())
{
    impl->device = std::move(device);
}

ScreenCapture::~ScreenCapture()
{
    stop();
}

bool ScreenCapture::start(const CaptureSource &source, bool captureCursor, int framerate, std::string *error)
{
    try {
        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(impl->device->device()->QueryInterface(IID_PPV_ARGS(&dxgiDevice))))
            throw std::runtime_error("device has no IDXGIDevice");
        winrt::com_ptr<::IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put()));
        impl->rtDevice = inspectable.as<wdx::Direct3D11::IDirect3DDevice>();

        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        if (source.kind == CaptureSource::Kind::Monitor) {
            winrt::check_hresult(interop->CreateForMonitor(reinterpret_cast<HMONITOR>(source.handle),
                                                           winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                           winrt::put_abi(impl->item)));
        } else {
            winrt::check_hresult(interop->CreateForWindow(reinterpret_cast<HWND>(source.handle),
                                                          winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                          winrt::put_abi(impl->item)));
        }

        // Size the pool for the whole virtual desktop up front. WGC only delivers frames when
        // the source repaints, so a pool recreated after a resize may never get the frame
        // that shows the new size; with room to spare, every resize lands in the next frame.
        const auto itemSize = impl->item.Size();
        impl->poolSize = { std::max<int32_t>(itemSize.Width, GetSystemMetrics(SM_CXVIRTUALSCREEN)),
                           std::max<int32_t>(itemSize.Height, GetSystemMetrics(SM_CYVIRTUALSCREEN)) };
        impl->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
                impl->rtDevice, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, impl->poolSize);
        impl->session = impl->pool.CreateCaptureSession(impl->item);

        using winrt::Windows::Foundation::Metadata::ApiInformation;
        if (ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsCursorCaptureEnabled"))
            impl->session.IsCursorCaptureEnabled(captureCursor);
        [framerate]<typename Session>(const Session &session) {
            if constexpr (requires { session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{}); }) {
                if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
                        L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval"))
                    session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{10'000'000 / framerate});
            }
        }(impl->session);
        // the yellow "being captured" border on Windows 11; best effort, needs no consent from us
        if (ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
            try {
                impl->session.IsBorderRequired(false);
            } catch (...) {
            }
        }

        Impl *raw = impl.get();
        impl->frameRevoker = impl->pool.FrameArrived(winrt::auto_revoke, [raw](const wgc::Direct3D11CaptureFramePool &sender, auto &&) {
            try {
                raw->onFrameArrived(sender);
            } catch (const winrt::hresult_error &e) {
                log(LogLevel::Warning, LogStream) << "Capture frame failed:" << winrt::to_string(e.message());
            }
        });
        impl->closedRevoker = impl->item.Closed(winrt::auto_revoke, [raw](auto &&, auto &&) {
            log(LogLevel::Info, LogStream) << "Capture source closed";
            raw->closed = true;
        });

        impl->session.StartCapture();
        log(LogLevel::Info, LogStream) << "Capturing" << source.title << impl->poolSize.Width << "x" << impl->poolSize.Height;
        return true;
    } catch (const winrt::hresult_error &e) {
        if (error)
            *error = winrt::to_string(e.message());
    } catch (const std::exception &e) {
        if (error)
            *error = e.what();
    }
    stop();
    return false;
}

void ScreenCapture::stop()
{
    impl->frameRevoker.revoke();
    impl->closedRevoker.revoke();
    if (impl->session) {
        impl->session.Close();
        impl->session = nullptr;
    }
    if (impl->pool) {
        impl->pool.Close();
        impl->pool = nullptr;
    }
    impl->item = nullptr;
    impl->rtDevice = nullptr;

    std::lock_guard lock(impl->frameMutex);
    impl->latest.Reset();
    impl->latestSize = {};
}

ScreenCapture::Frame ScreenCapture::latestFrame()
{
    std::lock_guard lock(impl->frameMutex);
    return Frame{ impl->latest.Get(), impl->latestSize, impl->sequence };
}

bool ScreenCapture::isClosed() const
{
    return impl->closed;
}

} // namespace Stream
} // namespace Core
} // namespace Styx
