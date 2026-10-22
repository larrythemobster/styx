// Ported from ouwou/Acheron video-streams commit 71b4907 (GPL-3.0-or-later).
#include "Core/Audio/LoopbackCapture.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <iterator>
#include <optional>
#include <set>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#elif defined(__linux__) && defined(STYX_HAVE_PIPEWIRE)
#include <pipewire/pipewire.h>
#include <spa/debug/types.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/type-info.h>
#include <spa/utils/result.h>
#include <fstream>
#include <unistd.h>
#endif

namespace Styx::Core::Audio {
namespace {
struct CaptureTarget { std::uint32_t processId; LoopbackCapture::Scope scope; };
constexpr int kBytesPerSampleFrame = LOOPBACK_CHANNELS * LOOPBACK_SAMPLE_BYTES;
std::string errorText(const char *backend, const std::string &detail) { return std::string(backend) + " could not capture the sound (" + detail + ")."; }
} // namespace

#ifdef _WIN32
namespace {
constexpr DWORD kActivationTimeoutMs = 3000;
constexpr REFERENCE_TIME kBufferDuration = 200 * 10000;
constexpr DWORD kSoundStoppedMs = 50;

struct Released { void operator()(IUnknown *object) const { if (object) object->Release(); } };
template <typename T> using ComPointer = std::unique_ptr<T, Released>;

class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler {
public:
    explicit ActivationHandler(const CaptureTarget &target) {
        loopback.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        loopback.ProcessLoopbackParams.TargetProcessId = target.processId;
        loopback.ProcessLoopbackParams.ProcessLoopbackMode = target.scope == LoopbackCapture::Scope::ProcessTree
            ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
        asked.vt = VT_BLOB; asked.blob.cbSize = sizeof(loopback); asked.blob.pBlobData = reinterpret_cast<BYTE *>(&loopback);
    }
    PROPVARIANT *parameters() { return &asked; }
    HANDLE completed() const { return done; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **object) override {
        if (id != __uuidof(IUnknown) && id != __uuidof(IActivateAudioInterfaceCompletionHandler) && id != __uuidof(IAgileObject)) { *object = nullptr; return E_NOINTERFACE; }
        *object = static_cast<IActivateAudioInterfaceCompletionHandler *>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { const auto remaining = --references; if (!remaining) delete this; return remaining; }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation *) override { SetEvent(done); return S_OK; }
private:
    ~ActivationHandler() { CloseHandle(done); }
    std::atomic<ULONG> references{1};
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    AUDIOCLIENT_ACTIVATION_PARAMS loopback{};
    PROPVARIANT asked{};
};
struct PendingActivation { ComPointer<ActivationHandler> handler; ComPointer<IActivateAudioInterfaceAsyncOperation> operation; };
HRESULT beginActivation(const CaptureTarget &target, PendingActivation &pending) {
    using ActivateAsync = HRESULT(WINAPI *)(LPCWSTR, REFIID, PROPVARIANT *, IActivateAudioInterfaceCompletionHandler *, IActivateAudioInterfaceAsyncOperation **);
    static const auto activate = reinterpret_cast<ActivateAsync>(reinterpret_cast<void *>(GetProcAddress(LoadLibraryW(L"mmdevapi.dll"), "ActivateAudioInterfaceAsync")));
    if (!activate) return E_NOTIMPL;
    pending.handler.reset(new ActivationHandler(target));
    IActivateAudioInterfaceAsyncOperation *operation = nullptr;
    const auto result = activate(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), pending.handler->parameters(), pending.handler.get(), &operation);
    pending.operation.reset(operation); return result;
}
HRESULT activatedClient(const PendingActivation &pending, ComPointer<IAudioClient> &client) {
    HRESULT activated = E_FAIL; IUnknown *made = nullptr;
    HRESULT result = pending.operation->GetActivateResult(&activated, &made);
    if (FAILED(result)) return result;
    if (FAILED(activated) || !made) return FAILED(activated) ? activated : E_NOINTERFACE;
    IAudioClient *raw = nullptr; result = made->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void **>(&raw)); made->Release(); client.reset(raw); return result;
}
std::string winFailure(const char *step, HRESULT result) {
    char code[16]; std::snprintf(code, sizeof(code), "0x%08lx", static_cast<unsigned long>(result));
    return errorText("Windows", std::string(step) + ", error " + code);
}
} // namespace

class LoopbackCapture::Worker {
public:
    explicit Worker(LoopbackCapture *owner) : owner(owner), asked(CreateEventW(nullptr, FALSE, FALSE, nullptr)), soundArrived(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
    ~Worker() { { std::lock_guard lock(mutex); quitting = true; } SetEvent(asked); if (thread.joinable()) thread.join(); CloseHandle(asked); CloseHandle(soundArrived); }
    void request(std::optional<CaptureTarget> target) { { std::lock_guard lock(mutex); wanted = target; ++requests; if (target && !thread.joinable()) thread = std::thread([this] { run(); }); } SetEvent(asked); }
private:
    void run() {
        const bool initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        std::uint64_t served = 0;
        for (;;) {
            std::optional<CaptureTarget> target; std::uint64_t request = 0;
            { std::lock_guard lock(mutex); if (quitting) break; request = requests; if (request != served) target = wanted; }
            served = request;
            if (!target) { WaitForSingleObject(asked, INFINITE); continue; }
            const auto failure = capture(*target, request);
            if (!failure.empty() && !superseded(request)) owner->failed.publish(failure);
        }
        if (initialized) CoUninitialize();
    }
    std::string capture(const CaptureTarget &target, std::uint64_t request) {
        PendingActivation activation; HRESULT result = beginActivation(target, activation);
        if (FAILED(result)) return winFailure("ActivateAudioInterfaceAsync", result);
        for (;;) { const HANDLE wait[] = {activation.handler->completed(), asked}; const auto wake = WaitForMultipleObjects(2, wait, FALSE, kActivationTimeoutMs);
            if (wake == WAIT_OBJECT_0) break;
            if (wake != WAIT_OBJECT_0 + 1) return winFailure("ActivateAudioInterfaceAsync", HRESULT_FROM_WIN32(wake == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError()));
            if (superseded(request)) return {};
        }
        ComPointer<IAudioClient> client; result = activatedClient(activation, client); if (FAILED(result)) return winFailure("GetActivateResult", result);
        WAVEFORMATEX format{}; format.wFormatTag = WAVE_FORMAT_PCM; format.nChannels = LOOPBACK_CHANNELS; format.nSamplesPerSec = LOOPBACK_SAMPLE_RATE; format.wBitsPerSample = LOOPBACK_SAMPLE_BYTES * 8; format.nBlockAlign = kBytesPerSampleFrame; format.nAvgBytesPerSec = LOOPBACK_SAMPLE_RATE * kBytesPerSampleFrame;
        result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, kBufferDuration, 0, &format, nullptr);
        if (FAILED(result)) return winFailure("IAudioClient::Initialize", result);
        ResetEvent(soundArrived); result = client->SetEventHandle(soundArrived); if (FAILED(result)) return winFailure("IAudioClient::SetEventHandle", result);
        IAudioCaptureClient *raw = nullptr; result = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(&raw)); if (FAILED(result)) return winFailure("IAudioClient::GetService", result); ComPointer<IAudioCaptureClient> reader(raw);
        result = client->Start(); if (FAILED(result)) return winFailure("IAudioClient::Start", result);
        std::vector<std::uint8_t> unsent; unsent.reserve(LOOPBACK_FRAME_SIZE * 2); std::string failure;
        for (;;) { const HANDLE wait[] = {asked, soundArrived}; const auto wake = WaitForMultipleObjects(2, wait, FALSE, unsent.empty() ? INFINITE : kSoundStoppedMs);
            if (wake == WAIT_OBJECT_0) { if (superseded(request)) break; continue; }
            if (wake == WAIT_TIMEOUT) { unsent.resize(LOOPBACK_FRAME_SIZE); owner->frameCaptured.publish(std::move(unsent)); unsent.clear(); continue; }
            if (wake != WAIT_OBJECT_0 + 1) { failure = winFailure("WaitForMultipleObjects", HRESULT_FROM_WIN32(GetLastError())); break; }
            result = read(reader.get(), unsent); if (FAILED(result)) { failure = winFailure("IAudioCaptureClient::GetBuffer", result); break; }
        }
        client->Stop(); return failure;
    }
    HRESULT read(IAudioCaptureClient *reader, std::vector<std::uint8_t> &unsent) {
        for (;;) { BYTE *samples = nullptr; UINT32 frames = 0; DWORD flags = 0; const HRESULT result = reader->GetBuffer(&samples, &frames, &flags, nullptr, nullptr); if (FAILED(result)) return result; if (result == AUDCLNT_S_BUFFER_EMPTY || !frames) return S_OK;
            const size_t bytes = static_cast<size_t>(frames) * kBytesPerSampleFrame; const auto old = unsent.size(); unsent.resize(old + bytes); if (flags & AUDCLNT_BUFFERFLAGS_SILENT) std::memset(unsent.data() + old, 0, bytes); else std::memcpy(unsent.data() + old, samples, bytes); reader->ReleaseBuffer(frames);
            while (unsent.size() >= LOOPBACK_FRAME_SIZE) { std::vector<std::uint8_t> frame(unsent.begin(), unsent.begin() + LOOPBACK_FRAME_SIZE); unsent.erase(unsent.begin(), unsent.begin() + LOOPBACK_FRAME_SIZE); owner->frameCaptured.publish(std::move(frame)); }
        }
    }
    bool superseded(std::uint64_t request) { std::lock_guard lock(mutex); return quitting || requests != request; }
    LoopbackCapture *owner; HANDLE asked; HANDLE soundArrived; std::mutex mutex; std::optional<CaptureTarget> wanted; std::uint64_t requests = 0; bool quitting = false; std::thread thread;
};

bool processLoopbackAvailable() {
    static const bool available = [] { bool result = false; std::thread([&] { const bool initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); PendingActivation activation; HRESULT status = beginActivation({GetCurrentProcessId(), LoopbackCapture::Scope::ProcessTree}, activation); if (SUCCEEDED(status) && WaitForSingleObject(activation.handler->completed(), kActivationTimeoutMs) != WAIT_OBJECT_0) status = HRESULT_FROM_WIN32(ERROR_TIMEOUT); ComPointer<IAudioClient> client; if (SUCCEEDED(status)) status = activatedClient(activation, client); result = SUCCEEDED(status); if (initialized) CoUninitialize(); }).join(); return result; }();
    return available;
}

#elif defined(__linux__) && defined(STYX_HAVE_PIPEWIRE)
namespace {
constexpr int kAnswerTimeoutMs = 200;
constexpr int kAskedAgainAfterMs = 1000;
constexpr spa_audio_channel kCaptured[] = {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_LFE, SPA_AUDIO_CHANNEL_SL, SPA_AUDIO_CHANNEL_SR, SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR};
enum Channel { FL, FR, FC, LFE, SL, SR, RL, RR, CapturedChannels };

bool initPipeWire() { static std::once_flag once; std::call_once(once, [] { pw_init(nullptr, nullptr); }); return true; }
std::uint32_t parentOf(std::uint32_t processId) { std::ifstream file("/proc/" + std::to_string(processId) + "/stat"); std::string line; std::getline(file, line); const auto close = line.rfind(')'); if (close == std::string::npos) return 0; const auto start = close + 2; const auto first = line.find(' ', start); if (first == std::string::npos) return 0; const auto second = line.find(' ', first + 1); try { return static_cast<std::uint32_t>(std::stoul(line.substr(first + 1, second - first - 1))); } catch (...) { return 0; } }
bool isInTree(std::uint32_t processId, std::uint32_t root) { for (; processId; processId = parentOf(processId)) if (processId == root) return true; return false; }

class Graph {
public:
    struct Client { pw_proxy *proxy = nullptr; spa_hook listener{}; bool known = false; std::uint32_t processId = 0; std::string name; std::optional<bool> inTree; };
    struct Node { Graph *graph = nullptr; pw_proxy *proxy = nullptr; spa_hook listener{}; std::optional<std::uint32_t> client; pw_node_state state = PW_NODE_STATE_CREATING; bool known = false; bool chained = false; std::vector<std::string> ownChannels; };
    struct Port { std::uint32_t node = 0; bool output = false; std::string channel; };
    explicit Graph(pw_context *context) : core(context ? pw_context_connect(context, nullptr, 0) : nullptr) {
        if (!core) return;
        static const pw_core_events coreEvents = {.version = PW_VERSION_CORE_EVENTS, .done = [](void *g, uint32_t, int seq) { static_cast<Graph *>(g)->answered(seq); }, .error = [](void *g, uint32_t id, int, int result, const char *) { static_cast<Graph *>(g)->failed(id, result); }};
        pw_core_add_listener(core, &coreListener, &coreEvents, this);
        static const pw_registry_events registryEvents = {.version = PW_VERSION_REGISTRY_EVENTS, .global = [](void *g, uint32_t id, uint32_t, const char *type, uint32_t, const spa_dict *props) { static_cast<Graph *>(g)->add(id, type, props); }, .global_remove = [](void *g, uint32_t id) { static_cast<Graph *>(g)->remove(id); }};
        registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0); if (!registry) { pw_core_disconnect(std::exchange(core, nullptr)); return; } pw_registry_add_listener(registry, &registryListener, &registryEvents, this);
    }
    ~Graph() { if (!core) return; for (auto &[id, c] : clients) pw_proxy_destroy(c.proxy); for (auto &[id, n] : nodes) pw_proxy_destroy(n.proxy); pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry)); pw_core_disconnect(core); }
    void sync() { pending = pw_core_sync(core, PW_ID_CORE, 0); }
    bool isPlaying() const { return std::any_of(nodes.begin(), nodes.end(), [](const auto &node) { return node.second.state == PW_NODE_STATE_RUNNING; }); }
    pw_core *core = nullptr; std::map<std::uint32_t, Client> clients; std::map<std::uint32_t, Node> nodes; std::map<std::uint32_t, Port> ports; bool sinkSeen = false; int answers = 0; std::function<void()> changed; std::function<void(int)> lost;
private:
    void answered(int seq) { if (seq == pending) ++answers; } void failed(std::uint32_t id, int result) { if (id == PW_ID_CORE && result == -EPIPE && lost) lost(result); } void tell() { if (changed) changed(); }
    pw_proxy *bind(std::uint32_t id, const char *type, std::uint32_t version) { auto *proxy = static_cast<pw_proxy *>(pw_registry_bind(registry, id, type, version, 0)); if (!proxy && lost) lost(-errno); return proxy; }
    void add(std::uint32_t id, const char *type, const spa_dict *props) {
        if (!props) return;
        if (spa_streq(type, PW_TYPE_INTERFACE_Client)) { static const pw_client_events events = {.version = PW_VERSION_CLIENT_EVENTS, .info = [](void *g, const pw_client_info *info) { static_cast<Graph *>(g)->identify(info); }}; auto *proxy = bind(id, type, PW_VERSION_CLIENT); if (!proxy) return; auto &client = clients[id]; client.proxy = proxy; pw_proxy_add_object_listener(proxy, &client.listener, &events, this); }
        else if (spa_streq(type, PW_TYPE_INTERFACE_Node)) { const char *klass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS); sinkSeen = sinkSeen || spa_streq(klass, "Audio/Sink"); if (!spa_streq(klass, "Stream/Output/Audio")) return; static const pw_node_events events = {.version = PW_VERSION_NODE_EVENTS, .info = &Graph::follow, .param = &Graph::shape}; auto *proxy = bind(id, type, PW_VERSION_NODE); if (!proxy) return; auto &node = nodes[id]; node.graph = this; node.proxy = proxy; if (const char *client = spa_dict_lookup(props, PW_KEY_CLIENT_ID)) node.client = static_cast<std::uint32_t>(std::strtoul(client, nullptr, 10)); pw_proxy_add_object_listener(proxy, &node.listener, &events, &node); std::uint32_t wanted[] = {SPA_PARAM_Format}; pw_node_subscribe_params(reinterpret_cast<pw_node *>(proxy), wanted, 1); }
        else if (spa_streq(type, PW_TYPE_INTERFACE_Port)) { const char *channel = spa_dict_lookup(props, PW_KEY_AUDIO_CHANNEL); const char *node = spa_dict_lookup(props, PW_KEY_NODE_ID); const char *direction = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION); if (!channel || !node || !direction) return; ports[id] = {static_cast<std::uint32_t>(std::strtoul(node, nullptr, 10)), spa_streq(direction, "out"), channel}; } else return; tell();
    }
    void remove(std::uint32_t id) { if (auto it = clients.find(id); it != clients.end()) { pw_proxy_destroy(it->second.proxy); clients.erase(it); } else if (auto it = nodes.find(id); it != nodes.end()) { pw_proxy_destroy(it->second.proxy); nodes.erase(it); } else if (!ports.erase(id)) return; tell(); }
    static void follow(void *data, const pw_node_info *info) { auto *node = static_cast<Node *>(data); node->state = info->state; if (!info->props || !(info->change_mask & PW_NODE_CHANGE_MASK_PROPS)) return; node->chained = spa_dict_lookup(info->props, PW_KEY_NODE_LINK_GROUP) != nullptr; node->known = true; node->graph->tell(); }
    static void shape(void *data, int, std::uint32_t, std::uint32_t, std::uint32_t, const spa_pod *format) { auto *node = static_cast<Node *>(data); spa_audio_info_raw raw{}; if (!format || spa_format_audio_raw_parse(format, &raw) < 0) return; node->ownChannels.clear(); for (std::uint32_t i = 0; i < std::min<std::uint32_t>(raw.channels, SPA_AUDIO_MAX_CHANNELS); ++i) { const char *name = spa_debug_type_find_short_name(spa_type_audio_channel, raw.position[i]); if (name) node->ownChannels.emplace_back(name); } node->graph->tell(); }
    void identify(const pw_client_info *info) { auto it = clients.find(info->id); if (it == clients.end() || !info->props) return; const bool pulse = spa_streq(spa_dict_lookup(info->props, PW_KEY_CLIENT_API), "pipewire-pulse"); const char *pid = spa_dict_lookup(info->props, pulse ? PW_KEY_APP_PROCESS_ID : PW_KEY_SEC_PID); it->second.processId = pid ? static_cast<std::uint32_t>(std::strtoul(pid, nullptr, 10)) : 0; const char *name = spa_dict_lookup(info->props, PW_KEY_APP_NAME); if (!name) name = spa_dict_lookup(info->props, PW_KEY_APP_PROCESS_BINARY); it->second.name = name ? name : ""; it->second.known = true; it->second.inTree.reset(); tell(); }
    pw_registry *registry = nullptr; spa_hook coreListener{}; spa_hook registryListener{}; int pending = 0;
};

void consult(int roundTrips, const std::function<void(const Graph &)> &read) { pw_main_loop *loop = initPipeWire() ? pw_main_loop_new(nullptr) : nullptr; pw_context *context = loop ? pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0) : nullptr; { Graph graph(context); if (graph.core) { pw_loop *events = pw_main_loop_get_loop(loop); pw_loop_enter(events); for (int round = 1; round <= roundTrips; ++round) { graph.sync(); for (int left = kAnswerTimeoutMs; graph.answers < round && left > 0; left = 0) pw_loop_iterate(events, left); } pw_loop_leave(events); } read(graph); } if (context) pw_context_destroy(context); if (loop) pw_main_loop_destroy(loop); }
} // namespace

class LoopbackCapture::Worker {
public:
    explicit Worker(LoopbackCapture *owner) : owner(owner) {}
    ~Worker() { if (!loop) return; pw_thread_loop_stop(loop); close(); if (context) pw_context_destroy(context); pw_thread_loop_destroy(loop); }
    void request(std::optional<CaptureTarget> wantedTarget) { std::lock_guard lock(mutex); wanted = wantedTarget; ++requests; if (wantedTarget && !loop) { loop = initPipeWire() ? pw_thread_loop_new("styx-loopback", nullptr) : nullptr; asked = loop ? pw_loop_add_event(pw_thread_loop_get_loop(loop), [](void *worker, std::uint64_t) { static_cast<Worker *>(worker)->serve(); }, this) : nullptr; const int started = asked ? pw_thread_loop_start(loop) : -ENOENT; if (started < 0) { if (loop) pw_thread_loop_destroy(std::exchange(loop, nullptr)); owner->failed.publish(errorText("PipeWire", spa_strerror(started))); return; } } if (loop) pw_loop_signal_event(pw_thread_loop_get_loop(loop), asked); }
private:
    void serve() { std::optional<CaptureTarget> next; bool replaced; { std::lock_guard lock(mutex); replaced = served != requests; served = requests; next = wanted; } auto failure = std::exchange(broken, std::string()); if (!replaced && failure.empty()) return; close(); if (!replaced) owner->failed.publish(failure); else if (next) { target = *next; open(); } }
    void open() { pw_loop *events = pw_thread_loop_get_loop(loop); if (!context) context = pw_context_new(events, nullptr, 0); graph = std::make_unique<Graph>(context); pace = graph->core ? pw_loop_add_timer(events, [](void *worker, std::uint64_t) { static_cast<Worker *>(worker)->sendNext(); }, this) : nullptr; stream = pace ? pw_stream_new(graph->core, "Shared sound", pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_NODE_AUTOCONNECT, "false", nullptr)) : nullptr; if (!stream) { fail(std::strerror(errno)); return; }
        static const pw_stream_events eventsDesc = {.version = PW_VERSION_STREAM_EVENTS, .state_changed = [](void *w, pw_stream_state, pw_stream_state state, const char *error) { static_cast<Worker *>(w)->onState(state, error); }, .process = [](void *w) { static_cast<Worker *>(w)->read(); }}; pw_stream_add_listener(stream, &streamListener, &eventsDesc, this);
        spa_audio_info_raw format{}; format.format = SPA_AUDIO_FORMAT_S16; format.rate = LOOPBACK_SAMPLE_RATE; format.channels = CapturedChannels; std::copy(std::begin(kCaptured), std::end(kCaptured), format.position); std::uint8_t buffer[1024]; spa_pod_builder builder; spa_pod_builder_init(&builder, buffer, sizeof(buffer)); const spa_pod *parameters[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)}; const int connected = pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY, PW_STREAM_FLAG_MAP_BUFFERS, parameters, 1); if (connected < 0) { fail(spa_strerror(connected)); return; } graph->changed = [this] { relink(); }; graph->lost = [this](int result) { fail(spa_strerror(result)); }; keepPace(); }
    void close() { if (!graph) return; if (pace) pw_loop_destroy_source(pw_thread_loop_get_loop(loop), std::exchange(pace, nullptr)); if (stream) pw_stream_destroy(std::exchange(stream, nullptr)); for (auto &[ports, link] : links) pw_proxy_destroy(link); links.clear(); graph.reset(); unsent.clear(); streaming = false; }
    void fail(const char *detail) { broken = errorText("PipeWire", detail ? detail : "unknown error"); pw_loop_signal_event(pw_thread_loop_get_loop(loop), asked); }
    void onState(pw_stream_state state, const char *error) { if (state == PW_STREAM_STATE_ERROR) fail(error); else streaming = state == PW_STREAM_STATE_STREAMING; }
    void keepPace() { timespec period{0, LOOPBACK_FRAME_DURATION_MS * 1000000}; pw_loop_update_timer(pw_thread_loop_get_loop(loop), pace, &period, &period, false); }
    void read() { while (sendFrame()) {} while (pw_buffer *buffer = pw_stream_dequeue_buffer(stream)) { const spa_data &data = buffer->buffer->datas[0]; if (data.data && data.chunk) { const auto offset = std::min(data.chunk->offset, data.maxsize); mixDown(reinterpret_cast<const std::int16_t *>(static_cast<const char *>(data.data) + offset), std::min(data.chunk->size, data.maxsize - offset) / (CapturedChannels * LOOPBACK_SAMPLE_BYTES)); } pw_stream_queue_buffer(stream, buffer); } sendFrame(); keepPace(); }
    void mixDown(const std::int16_t *captured, std::uint32_t count) { constexpr float side = .707106781f, bass = .353553391f; const size_t old = unsent.size(); unsent.resize(old + static_cast<size_t>(count) * kBytesPerSampleFrame); auto *stereo = reinterpret_cast<std::int16_t *>(unsent.data() + old); for (std::uint32_t i = 0; i < count; ++i, captured += CapturedChannels, stereo += 2) { const float both = side * captured[FC] + bass * captured[LFE]; stereo[0] = static_cast<std::int16_t>(std::clamp(captured[FL] + both + side * (captured[SL] + captured[RL]), -32768.f, 32767.f)); stereo[1] = static_cast<std::int16_t>(std::clamp(captured[FR] + both + side * (captured[SR] + captured[RR]), -32768.f, 32767.f)); } }
    bool sendFrame() { if (unsent.size() < LOOPBACK_FRAME_SIZE) return false; std::vector<std::uint8_t> frame(unsent.begin(), unsent.begin() + LOOPBACK_FRAME_SIZE); unsent.erase(unsent.begin(), unsent.begin() + LOOPBACK_FRAME_SIZE); owner->frameCaptured.publish(std::move(frame)); return true; }
    void sendNext() { if (sendFrame() || streaming) return; if (!unsent.empty()) { unsent.resize(LOOPBACK_FRAME_SIZE); sendFrame(); } else if (graph->isPlaying()) owner->frameCaptured.publish(std::vector<std::uint8_t>(LOOPBACK_FRAME_SIZE)); }
    bool inScope(const Graph::Node &node) { if (!node.known || node.chained) return false; bool tree = false; if (node.client) { auto client = graph->clients.find(*node.client); if (client == graph->clients.end() || !client->second.known) return false; if (!client->second.inTree) client->second.inTree = isInTree(client->second.processId, target.processId); tree = *client->second.inTree; } return tree == (target.scope == LoopbackCapture::Scope::ProcessTree); }
    static bool carries(const Graph::Node &node, const std::string &played, const std::string &captured) { if (captured == "FL" || captured == "FR") return played == captured || played == "MONO"; return played == captured && std::find(node.ownChannels.begin(), node.ownChannels.end(), played) != node.ownChannels.end(); }
    void relink() { const auto self = pw_stream_get_node_id(stream); std::set<std::pair<std::uint32_t, std::uint32_t>> missing; for (const auto &[outId, output] : graph->ports) { auto node = graph->nodes.find(output.node); if (!output.output || node == graph->nodes.end() || !inScope(node->second)) continue; for (const auto &[inId, input] : graph->ports) if (input.node == self && !input.output && carries(node->second, output.channel, input.channel)) missing.emplace(outId, inId); } for (auto it = links.begin(); it != links.end();) { if (missing.erase(it->first)) { ++it; continue; } pw_proxy_destroy(it->second); it = links.erase(it); } for (const auto &[output, input] : missing) { const auto from = std::to_string(output); const auto to = std::to_string(input); const spa_dict_item items[] = {{PW_KEY_LINK_OUTPUT_PORT, from.c_str()}, {PW_KEY_LINK_INPUT_PORT, to.c_str()}}; const spa_dict props{0, 2, items}; auto *link = static_cast<pw_proxy *>(pw_core_create_object(graph->core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props, 0)); if (!link) { fail(std::strerror(errno)); return; } links[{output, input}] = link; } }
    LoopbackCapture *owner; std::mutex mutex; std::optional<CaptureTarget> wanted; std::uint64_t requests = 0; pw_thread_loop *loop = nullptr; spa_source *asked = nullptr; std::uint64_t served = 0; pw_context *context = nullptr; CaptureTarget target{}; std::unique_ptr<Graph> graph; pw_stream *stream = nullptr; spa_hook streamListener{}; spa_source *pace = nullptr; std::map<std::pair<std::uint32_t, std::uint32_t>, pw_proxy *> links; std::vector<std::uint8_t> unsent; std::string broken; bool streaming = false;
};

bool processLoopbackAvailable() { static std::mutex mutex; static bool available = false; static auto asked = std::chrono::steady_clock::time_point{}; std::lock_guard lock(mutex); const auto now = std::chrono::steady_clock::now(); if (available || (asked.time_since_epoch().count() && now - asked < std::chrono::milliseconds(kAskedAgainAfterMs))) return available; consult(1, [](const Graph &graph) { available = graph.sinkSeen; }); asked = now; return available; }
std::vector<PlayingProgram> playingPrograms() { if (!processLoopbackAvailable()) return {}; std::map<std::uint32_t, std::string> names; consult(2, [&] (const Graph &graph) { const auto self = static_cast<std::uint32_t>(getpid()); for (const auto &[id, node] : graph.nodes) { auto client = node.client ? graph.clients.find(*node.client) : graph.clients.end(); if (!node.known || node.chained || client == graph.clients.end() || !client->second.processId || isInTree(client->second.processId, self)) continue; auto known = names.find(client->second.processId); if (known == names.end() || client->second.name < known->second) names[client->second.processId] = client->second.name; } }); std::vector<PlayingProgram> programs; for (const auto &[id, name] : names) programs.push_back({id, name}); std::stable_sort(programs.begin(), programs.end(), [](const auto &a, const auto &b) { return a.name < b.name; }); return programs; }

#else
class LoopbackCapture::Worker { public: explicit Worker(LoopbackCapture *owner) : owner(owner) {} void request(std::optional<CaptureTarget> target) { if (target) owner->failed.publish("This build cannot capture what a program is playing."); } private: LoopbackCapture *owner; };
bool processLoopbackAvailable() { return false; }
#endif

#if defined(_WIN32) || !(defined(__linux__) && defined(STYX_HAVE_PIPEWIRE))
std::vector<PlayingProgram> playingPrograms() { return {}; }
#endif

LoopbackCapture::LoopbackCapture() : worker(std::make_unique<Worker>(this)) {}
LoopbackCapture::~LoopbackCapture() = default;
void LoopbackCapture::start(std::uint32_t processId, Scope scope) { worker->request(CaptureTarget{processId, scope}); }
void LoopbackCapture::stop() { worker->request(std::nullopt); }
} // namespace Styx::Core::Audio
