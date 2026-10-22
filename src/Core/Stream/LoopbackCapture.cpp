#include "LoopbackCapture.hpp"

#ifdef _WIN32

#include "Core/Logging.hpp"

#include <QOperatingSystemVersion>

#include <Audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <wrl/implements.h>

#include <chrono>
#include <vector>

namespace Styx {
namespace Core {
namespace Stream {

using Microsoft::WRL::ComPtr;

namespace {

class ActivationHandler
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler>
{
public:
    ActivationHandler() { done = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    ~ActivationHandler() override { CloseHandle(done); }

    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation *operation) override
    {
        HRESULT activateResult = E_FAIL;
        ComPtr<IUnknown> unknown;
        result = operation->GetActivateResult(&activateResult, &unknown);
        if (SUCCEEDED(result))
            result = activateResult;
        if (SUCCEEDED(result))
            result = unknown.As(&client);
        SetEvent(done);
        return S_OK;
    }

    HANDLE done = nullptr;
    HRESULT result = E_PENDING;
    ComPtr<IAudioClient> client;
};

QString hresultText(HRESULT hr)
{
    return QStringLiteral("0x%1").arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
}

} // namespace

LoopbackCapture::~LoopbackCapture()
{
    stop();
}

bool LoopbackCapture::isSupported()
{
    // AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK arrived in Windows 10 2004 (build 19041)
    return QOperatingSystemVersion::current() >= QOperatingSystemVersion(QOperatingSystemVersion::Windows, 10, 0, 19041);
}

bool LoopbackCapture::start(Mode mode, quint32 processId, Callback cb, QString *error)
{
    stop();
    callback = std::move(cb);
    running = true;
    startState = 0;
    thread = std::thread(&LoopbackCapture::run, this, mode, processId);

    // activation is asynchronous; wait for the thread to report either way
    for (int i = 0; i < 300 && startState == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (startState != 1) {
        if (error)
            *error = startError.isEmpty() ? QStringLiteral("audio capture did not start") : startError;
        stop();
        return false;
    }
    return true;
}

void LoopbackCapture::stop()
{
    running = false;
    if (thread.joinable())
        thread.join();
}

void LoopbackCapture::run(Mode mode, quint32 processId)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    auto fail = [this](const QString &message) {
        startError = message;
        qCWarning(LogStream) << "Stream audio capture:" << message;
        startState = -1;
    };

    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = processId;
    params.ProcessLoopbackParams.ProcessLoopbackMode = mode == Mode::IncludeProcess
                                                               ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
                                                               : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT activateParams{};
    activateParams.vt = VT_BLOB;
    activateParams.blob.cbSize = sizeof(params);
    activateParams.blob.pBlobData = reinterpret_cast<BYTE *>(&params);

    auto handler = Microsoft::WRL::Make<ActivationHandler>();
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                                             &activateParams, handler.Get(), &operation);
    if (FAILED(hr)) {
        fail(QStringLiteral("ActivateAudioInterfaceAsync failed ") + hresultText(hr));
        CoUninitialize();
        return;
    }
    WaitForSingleObject(handler->done, 3000);
    if (FAILED(handler->result) || !handler->client) {
        fail(QStringLiteral("process loopback activation failed ") + hresultText(handler->result));
        CoUninitialize();
        return;
    }
    ComPtr<IAudioClient> client = handler->client;

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = CHANNELS;
    format.nSamplesPerSec = SAMPLE_RATE;
    format.wBitsPerSample = 16;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                                    | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                            200 * 10000, 0, &format, nullptr);
    ComPtr<IAudioCaptureClient> capture;
    if (SUCCEEDED(hr))
        hr = client->SetEventHandle(ready);
    if (SUCCEEDED(hr))
        hr = client->GetService(IID_PPV_ARGS(&capture));
    if (SUCCEEDED(hr))
        hr = client->Start();
    if (FAILED(hr)) {
        fail(QStringLiteral("audio client setup failed ") + hresultText(hr));
        CloseHandle(ready);
        CoUninitialize();
        return;
    }

    startState = 1;
    std::vector<int16_t> silence;

    while (running) {
        if (WaitForSingleObject(ready, 100) != WAIT_OBJECT_0)
            continue;
        UINT32 available = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&available)) && available > 0) {
            BYTE *data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                break;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                silence.assign(static_cast<size_t>(frames) * CHANNELS, 0);
                callback(silence.data(), static_cast<int>(frames));
            } else {
                callback(reinterpret_cast<const int16_t *>(data), static_cast<int>(frames));
            }
            capture->ReleaseBuffer(frames);
        }
    }

    client->Stop();
    CloseHandle(ready);
    CoUninitialize();
}

} // namespace Stream
} // namespace Core
} // namespace Styx

#endif
