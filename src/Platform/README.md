# Styx Platform Foundation (`Styx::Platform`)

Qt-free, lightweight platform foundation layer for Styx. Provides operating system abstractions, thread scheduling, event handling, storage, networking, image decoding, and configuration.

Target: `styx_platform` (static library)  
Namespace: `Styx::Platform`

## Interfaces

### 1. Dispatcher (`Dispatcher.hpp`)
Main-thread event loop dispatcher and timer scheduler.
- Thread-safe non-blocking task queue (`post`, `drainTasks`).
- Wake hook callback (`setWakeHook`) to wake host event loop (SDL / Win32 / Qt pump).
- High-resolution timers (`setTimer`, `cancelTimer`) with single-shot or repeating mode.
- Host integration via `runPending()` and `nextTimeout()` for efficient loop sleeping.

### 2. Signal (`Signal.hpp`)
Type-safe multicast signals and slots with RAII connection management.
- RAII `Connection` handle auto-disconnects on destruction.
- Owner-based bulk disconnect (`disconnect(owner)`).
- Reentrant, safe disconnect-during-emit without invalidating iterators.
- Optional queued delivery via `Dispatcher::main().post` (via `ConnectionType::Queued` or `emitQueued`).

### 3. Settings (`Settings.hpp`)
Typed persistent configuration store backed by JSON (nlohmann-json).
- Key-value accessors (`getBool`, `getInt`, `getString`, `getJson`, template `get`/`set`).
- Atomic file writes via temporary file and atomic replace.
- Change notifications through `Signal<std::string, nlohmann::json> changed`.
- Built-in one-time Win32 registry importer (`importFromRegistry`) to read legacy QSettings from `HKCU\Software\larrythemobster\Styx` without Qt.

### 4. SecureStore (`SecureStore.hpp`)
Hardware-backed credential management.
- Windows implementation uses Windows Credential Manager (`CredWriteW`, `CredReadW`, `CredDeleteW`) under `CRED_TYPE_GENERIC`.
- Uses identical target-name formatting and attribute layout as QtKeychain on Windows so existing Styx account tokens are seamlessly readable.
- Portable seam for other platforms.

### 5. Sqlite (`Sqlite.hpp`)
Modern RAII wrapper for SQLite3 databases.
- `Database`: connection lifecycle, execution, and prepared statement factory.
- `Statement`: RAII prepared statements with typed 1-based and named parameter binding (`bind`), stepwise row traversal (`step`), and typed column getters (`getInt`, `getText`, `getBlob`).
- `Transaction`: RAII transaction scope with explicit `commit()` and automatic rollback on scope exit or exception.
- Rich typed exceptions (`SqliteError`) reporting SQLite error codes and messages.

### 6. Paths (`Paths.hpp`)
Operating system directory resolution matching Styx conventions:
- `configDir()`: AppData roaming directory (`%APPDATA%\larrythemobster\Styx` on Windows).
- `dataDir()`: AppData data directory (`%APPDATA%\larrythemobster\Styx` on Windows).
- `cacheDir()`: LocalAppData cache directory (`%LOCALAPPDATA%\larrythemobster\Styx` on Windows).
- `executablePath()`: Path of running binary.

### 7. Image (`Image.hpp`)
Raw pixel buffer and multi-format image decoder:
- `decode`: Decodes PNG, JPEG, GIF (first frame), WebP, and BMP from memory buffers into raw RGBA8 (`ImageBuffer`). Backed by `stb_image` and `libwebp`.
- `decodeFile`: Reads and decodes image from disk.
- `scale`: Bilinear/linear image resizing backed by `stb_image_resize2`.

### 8. Http (`Http.hpp`)
Asynchronous multi-transfer HTTP client:
- Dedicated worker thread driving non-blocking `curl_multi_poll`.
- Supports arbitrary methods (`GET`, `POST`, custom), headers, request bodies, and per-request proxies (HTTP/SOCKS5).
- Cancellation support (`cancel`).
- Response callbacks dispatched onto the main thread via `Dispatcher`.

### 9. Utf8 (`Utf8.hpp`)
String utilities to replace common QString operations:
- `toUtf16` / `fromUtf16`: UTF-8 <-> UTF-16 conversions for Win32 API interop.
- `trim`, `trimLeft`, `trimRight`: whitespace stripping.
- `split`: string splitting by character or delimiter string with optional empty token skipping.
- `toLowerAscii`, `toUpperAscii`, `equalsIgnoreCaseAscii`: fast ASCII case folding.
- `startsWith`, `endsWith`, `replace`: standard string operations.
