# Styx

A native Discord client in C++ with Qt 6, using an RmlUi interface modelled on
the official client. It also supports Go Live screen sharing.

Styx is a fork of [Acheron](https://github.com/ouwou/acheron) by
[ouwou](https://github.com/ouwou). Acheron provides the gateway, REST client,
storage, markdown, voice with DAVE end-to-end encryption, multi-account
support, and most of the client logic. This fork builds on that work, and
Acheron deserves the credit for it. If you want the stable, cross-platform
client, use upstream Acheron and
[join its Discord](https://discord.gg/wkCU3vuzG5).

> [!WARNING]
> Third-party Discord clients break Discord's Terms of Service. Your account
> could be actioned for using one. Use Styx at your own risk.

## What this fork adds

**RmlUi main window.** The main window is drawn with [RmlUi](https://github.com/mikke89/RmlUi)
(HTML/CSS-style documents) on an OpenGL 3.3 `QOpenGLWidget`, in place of
Acheron's Qt widget layout. It has:

- a server rail with folders, unread and mention badges
- channel and DM sidebars, a floating user panel and a member list
- a chat view with grouped messages, embeds, replies and reactions
- a Friends page with Online and All tabs, search, activity cards, an Active
  Now panel and an Add Friend form
- a voice call view

Settings, profiles, account management and other dialogs still use Acheron's Qt
widgets.

**Go Live screen sharing (Windows 10+).**

- Shares a screen or a window through Windows Graphics Capture, with optional
  WASAPI loopback audio for the whole system or a single app.
- Encodes H.264 with AMF, NVENC, Quick Sync or Media Foundation and falls
  back to OpenH264 or x264. Presets: 720p30, 720p60, 1080p30 and 1080p60.
  Discord only allows 720p30 without Nitro.
- Sends media over WebRTC through libdatachannel with SRTP and DAVE end-to-end
  encryption.
- Can watch other people's streams in a separate viewer window.

**Fixes.** Repaints triggered by timers and network events no longer free GPU
resources without the window's OpenGL context current. The renderer queues
them and frees them on the next frame.

## Status

Styx is experimental and mainly tested on Windows 11. The RmlUi window and Go
Live are Windows-first. Upstream Acheron's Linux and macOS support has not been
re-tested with the new UI. The executable is named `styx.exe` and automatically
migrates existing Acheron data on first launch.

## Building on Windows

Requirements:

- Visual Studio 2022 with the C++ workload, plus CMake and Ninja
- Qt 6.12 for MSVC 2022 x64, including OpenGL, OpenGLWidgets, Svg, Sql and Test
- prebuilt FFmpeg (shared) and
  [curl-impersonate](https://github.com/lexiforest/curl-impersonate)
- an OpenGL 3.3 driver

vcpkg is vendored as a submodule and installs the remaining libraries,
including RmlUi, libdatachannel, libsodium, opus and OpenSSL.

```bat
git clone --recursive https://github.com/larrythemobster/styx.git
cd styx
set DEPS=C:\path\to\deps
scripts\build-windows.bat configure
```

`DEPS` should contain `Qt\6.12.0\msvc2022_64`, `ffmpeg` and `curl-impersonate`.
If it isn't set, the script looks for `..\deps`. Later builds only need
`scripts\build-windows.bat`. The script builds `build\styx.exe` and runs
`windeployqt`. FFmpeg and curl-impersonate DLLs must be beside the executable
or on `PATH`.

### Tests

After a `configure` build, run the renderer regression tests from an MSVC
prompt with Qt's `bin` directory on `PATH`:

```bat
cmake --build build --target TestRmlRender
ctest --test-dir build -R "^RmlRenderTests$" --output-on-failure
```

They need a desktop session with OpenGL 3.3.

## Dependencies

- Qt 6.7+ with OpenGL and OpenGLWidgets
- RmlUi 6.1
- libcurl-impersonate (plain libcurl works but isn't recommended)
- OpenSSL, zlib, QtKeychain
- nayuki/QR-Code-generator and emoji-segmenter (vendored)
- rlottie (optional, Lottie stickers)
- libsodium, libopus, libdave, mlspp, miniaudio (optional, voice; the last
  three are vendored)
- rnnoise (optional, noise suppression, vendored)
- libdatachannel with libsrtp and FFmpeg (optional, Go Live, Windows only)

## Credits

- [ouwou](https://github.com/ouwou) wrote [Acheron](https://github.com/ouwou/acheron),
  which this fork is based on. Thanks also to Acheron's other contributors.
- [RmlUi](https://github.com/mikke89/RmlUi) provides the UI library and the
  OpenGL 3 renderer adapted in `vendor/rmlui-backend`.
- Icons come from [Lucide](https://lucide.dev).

## License

GPL-3.0, the same as Acheron. See [LICENSE](LICENSE). Styx is a modified version
of Acheron. The commit history keeps Acheron's original commits, and this
fork's changes start after them.
