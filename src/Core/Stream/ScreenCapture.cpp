// WinRT headers first: Qt's keyword macros (emit, signals, slots) must not reach them.
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

#include <QCoreApplication>
#include <QGuiApplication>
#include <QScreen>

#include <atomic>
#include <mutex>

namespace Acheron {
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
    auto *sources = reinterpret_cast<QList<CaptureSource> *>(param);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info))
        return TRUE;

    CaptureSource source;
    source.kind = CaptureSource::Kind::Monitor;
    source.handle = reinterpret_cast<quintptr>(monitor);
    source.geometry = QRect(info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right - info.rcMonitor.left,
                            info.rcMonitor.bottom - info.rcMonitor.top);
    source.primary = info.dwFlags & MONITORINFOF_PRIMARY;
    source.title = QCoreApplication::translate("ScreenCapture", "Screen %1 (%2 × %3)")
                           .arg(sources->size() + 1)
                           .arg(source.geometry.width())
                           .arg(source.geometry.height());
    sources->append(source);
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
    const QString cls = QString::fromWCharArray(className);
    if (cls == QLatin1String("Progman") || cls == QLatin1String("WorkerW") || cls == QLatin1String("Shell_TrayWnd"))
        return false;
    return true;
}

BOOL CALLBACK collectWindow(HWND hwnd, LPARAM param)
{
    if (!isCapturableWindow(hwnd))
        return TRUE;
    auto *sources = reinterpret_cast<QList<CaptureSource> *>(param);

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
    source.handle = reinterpret_cast<quintptr>(hwnd);
    source.title = QString::fromWCharArray(title.c_str(), length);
    source.geometry = QRect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
    source.processId = pid;
    if (source.geometry.width() > 1 && source.geometry.height() > 1)
        sources->append(source);
    return TRUE;
}

QImage imageFromDc(HDC sourceDc, int srcX, int srcY, int srcW, int srcH, const QSize &target, bool stretch)
{
    HDC memDc = CreateCompatibleDC(sourceDc);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = target.width();
    bmi.bmiHeader.biHeight = -target.height();
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
        StretchBlt(memDc, 0, 0, target.width(), target.height(), sourceDc, srcX, srcY, srcW, srcH, SRCCOPY);
    } else {
        BitBlt(memDc, 0, 0, target.width(), target.height(), sourceDc, srcX, srcY, SRCCOPY);
    }
    QImage image(static_cast<const uchar *>(bits), target.width(), target.height(), QImage::Format_RGB32);
    image = image.copy();
    SelectObject(memDc, old);
    DeleteObject(bitmap);
    DeleteDC(memDc);
    return image;
}

} // namespace

QList<CaptureSource> enumerateCaptureSources()
{
    QList<CaptureSource> monitors;
    EnumDisplayMonitors(nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&monitors));

    QList<CaptureSource> windows;
    EnumWindows(collectWindow, reinterpret_cast<LPARAM>(&windows));

    return monitors + windows;
}

QImage captureThumbnail(const CaptureSource &source, const QSize &maxSize)
{
    const QSize scaled = source.geometry.size().scaled(maxSize, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));

    if (source.kind == CaptureSource::Kind::Monitor) {
        HDC screen = GetDC(nullptr);
        QImage image = imageFromDc(screen, source.geometry.x(), source.geometry.y(), source.geometry.width(),
                                   source.geometry.height(), scaled, true);
        ReleaseDC(nullptr, screen);
        return image;
    }

    // PrintWindow with PW_RENDERFULLCONTENT also renders DirectComposition/GPU content
    auto hwnd = reinterpret_cast<HWND>(source.handle);
    RECT client{};
    GetWindowRect(hwnd, &client);
    const QSize full(client.right - client.left, client.bottom - client.top);
    if (full.isEmpty())
        return {};

    HDC windowDc = GetDC(hwnd);
    HDC fullDc = CreateCompatibleDC(windowDc);
    HBITMAP fullBitmap = CreateCompatibleBitmap(windowDc, full.width(), full.height());
    HGDIOBJ old = SelectObject(fullDc, fullBitmap);
    constexpr UINT PW_RENDERFULLCONTENT_FLAG = 0x00000002;
    PrintWindow(hwnd, fullDc, PW_RENDERFULLCONTENT_FLAG);
    QImage image = imageFromDc(fullDc, 0, 0, full.width(), full.height(), full.scaled(maxSize, Qt::KeepAspectRatio).expandedTo(QSize(1, 1)), true);
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
    QSize latestSize;
    quint64 sequence = 0;
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
        if (!latest || latestSize != QSize(width, height)) {
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
            latestSize = QSize(width, height);
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

bool ScreenCapture::start(const CaptureSource &source, bool captureCursor, QString *error)
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
                qCWarning(LogStream) << "Capture frame failed:" << QString::fromWCharArray(e.message().c_str());
            }
        });
        impl->closedRevoker = impl->item.Closed(winrt::auto_revoke, [raw](auto &&, auto &&) {
            qCInfo(LogStream) << "Capture source closed";
            raw->closed = true;
        });

        impl->session.StartCapture();
        qCInfo(LogStream) << "Capturing" << source.title << impl->poolSize.Width << "x" << impl->poolSize.Height;
        return true;
    } catch (const winrt::hresult_error &e) {
        if (error)
            *error = QString::fromWCharArray(e.message().c_str());
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
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
} // namespace Acheron
