# Styx

A native Discord client in C++ using SDL3 to host an RmlUi interface modelled
on the official client. It also supports Go Live screen sharing.

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
(HTML/CSS-style documents) using OpenGL 3.3 in an SDL3 window, in place of
Acheron's Qt widget layout. It has:

- a server rail with folders, unread and mention badges
- channel and DM sidebars, a floating user panel and a member list
- a chat view with grouped messages, embeds, replies and reactions
- a Friends page with Online and All tabs, search, activity cards, an Active
  Now panel and an Add Friend form
- a voice call view

Settings, profiles, account management, server settings, image viewing and
pickers also use RmlUi. User, server, channel and message context menus render
in the same view, with keyboard navigation, nested menus and viewport clamping.
Member moderation includes kick, ban, timeout, nickname and searchable role
forms. CAPTCHA prompts use RmlUi while the challenge still opens in your browser.

Native platform services provide file dialogs, the system tray, credential
storage and startup error dialogs.

**Go Live screen sharing (Windows 10+).**

- Shares a screen or a window through Windows Graphics Capture, with optional
  WASAPI loopback audio for the whole system or a single app.
- Encodes H.264 with AMF, NVENC, Quick Sync or Media Foundation and falls
  back to OpenH264 or x264. Presets: 720p30, 720p60, 1080p30 and 1080p60.
  Discord only allows 720p30 without Nitro.
- Sends media over WebRTC through libdatachannel with SRTP and DAVE end-to-end
  encryption.
- Can watch other people's streams in the call view.

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

- Visual Studio 2022 with the C++ workload, CMake 3.21 or newer, and Ninja
- prebuilt FFmpeg (shared) and
  [curl-impersonate](https://github.com/lexiforest/curl-impersonate)
- an OpenGL 3.3 driver

vcpkg is vendored as a submodule and installs SDL3 3.2 or newer for tray
support, RmlUi, FreeType, HarfBuzz, LunaSVG, libavif, utf8proc, PCRE2,
libdatachannel, libsodium, opus, OpenSSL and the remaining libraries.

```bat
git clone --recursive https://github.com/larrythemobster/styx.git
cd styx
set DEPS=C:\path\to\deps
scripts\build-windows.bat configure
```

`DEPS` should contain `ffmpeg` and `curl-impersonate`. If it isn't set, the
script looks for `..\deps`. Later builds only need
`scripts\build-windows.bat`. The script defaults to the `build` directory;
set `BUILD_DIR` to override it. CMake deploys runtime DLLs and resources beside
the executable.

### Tests

After a `configure` build, run the renderer and menu regression tests from an
MSVC prompt. Replace `build` with `BUILD_DIR` if you used an override:

```bat
cmake --build build --target TestRmlRender
ctest --test-dir build -R "^RmlRenderTests$" --output-on-failure
```

They need a desktop session with OpenGL 3.3. Coverage includes texture lifetimes,
background document updates, menu selection, disabled actions, dismissal,
nested navigation and viewport bounds.

## Dependencies

- SDL3 3.2+ and OpenGL 3.3
- RmlUi 6.1, FreeType, HarfBuzz, LunaSVG, libavif with dav1d, utf8proc and PCRE2
- libcurl-impersonate (plain libcurl works but isn't recommended)
- OpenSSL and zlib
- libsecret (Linux) or the system Security framework (macOS) for credential storage;
  KDE sessions use KWallet over D-Bus. Existing QtKeychain service/account identities
  are retained.
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
