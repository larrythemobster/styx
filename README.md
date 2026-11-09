# Styx

A Discord client written in C++. The interface is built with RmlUi in an SDL3
window and laid out like the official client. Styx supports voice calls with
DAVE end-to-end encryption, multiple accounts and Go Live screen sharing.

> [!WARNING]
> Third-party Discord clients break Discord's Terms of Service. Your account
> could be actioned for using one. Use Styx at your own risk.

## Features

**Interface.** The main window is drawn with [RmlUi](https://github.com/mikke89/RmlUi)
(HTML/CSS-style documents) using OpenGL 3.3 in an SDL3 window. It has:

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

Voice & Video settings offer Voice Activity or Push to Talk. Record a shortcut and
adjust its release delay from 0 to 2000 ms. Windows and X11 with XInput2 2.2 support
passive global input that never consumes keys, so other applications still receive
them. On Wayland and macOS, push to talk works only while Styx has focus.

The Keybinds page records and removes saved Push to Talk, Toggle Mute and Toggle
Deafen shortcuts, and rejects conflicts with other bindings or fixed shortcuts.
Mute and deafen work while Styx is focused. Their default keys are Ctrl+Shift+M
and Ctrl+Shift+D, or Cmd+Shift+M and Cmd+Shift+D on macOS. Rebinding or removing
them disables the old keys. The shortcut help dialog shows the current bindings.

The smile button in the user panel edits custom status text, Unicode or server
emoji, and expiry. Save, remove the emoji, or clear the whole status. Updates keep
the current presence and activity-visibility settings.

Account settings edits read the latest server protobuf, keep untouched fields
and send only changed groups with a data-version check. Custom status uses this
write path, which also supports appearance, privacy and guild-folder edits.
Conflicts report an error rather than overwriting another client's changes.
Styx's custom theme generator and local notification preferences stay local.

My Account's Edit Profile dialog changes your display name, pronouns, bio, avatar
and banner. Images use the same crop and preview pipeline as server settings.
Reset discards unsaved edits. If only part of a save succeeds, retry sends the
remaining changes without uploading the saved avatar again. Banner changes
require an eligible Nitro subscription.

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

## Status

Styx is experimental and mainly tested on Windows 11. Go Live capture is
Windows-only.

The native SDL3/RmlUi build, login window and test suite have been verified on
Ubuntu/WSL under Xvfb with software OpenGL, with `ENABLE_VOICE=OFF`,
`ENABLE_RNNOISE=OFF` and `ENABLE_FFMPEG=OFF`. Linux voice and FFmpeg playback,
and the native macOS UI, have not been verified.

The executable is `styx.exe` on Windows or `styx` on Linux. If you used
Acheron, Styx imports its settings, database and cache on first launch.

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

After a `configure` build, run the renderer, menu and animation regression tests
from an MSVC prompt. Replace `build` with `BUILD_DIR` if you used an override:

```bat
cmake --build build --target TestRmlRender TestClips
ctest --test-dir build -R "^(RmlRenderTests|ClipTests)$" --output-on-failure
```

The renderer and menu tests need a desktop session with OpenGL 3.3. They cover
texture lifetimes, background document updates, menu selection, disabled actions,
dismissal, nested navigation and viewport bounds. The background-refresh test reads
pixels from an owned framebuffer, independent of front-buffer behavior after a swap.

`ClipTests` also checks GIF and animated WebP playback when `ENABLE_FFMPEG=OFF`.
MP4 cases run when FFmpeg is enabled.

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

Linux builds also need libX11 and libXi development headers, along with Autoconf,
Automake, Libtool, Gettext, M4 and NASM. The Linux CI jobs install these build tools
explicitly.

## Credits

- Styx started from the source of [Acheron](https://github.com/ouwou/acheron)
  by [ouwou](https://github.com/ouwou), imported at commit
  [`e5337a5`](https://github.com/ouwou/acheron/commit/e5337a5f3b2ca2978dbf48bbbe124e3a62926591).
  Acheron wrote the gateway and REST clients, storage, markdown parsing, voice
  with DAVE, multi-account support and much of the client logic. Go Live
  viewing and the video engine were ported from Acheron's `video-streams`
  branch (commit `71b4907`), and the ported source files say so. Thanks to
  ouwou and Acheron's other contributors.
- [RmlUi](https://github.com/mikke89/RmlUi) provides the UI library and the
  OpenGL 3 renderer adapted in `vendor/rmlui-backend`.
- Icons come from [Lucide](https://lucide.dev).

## License

GPL-3.0. See [LICENSE](LICENSE). Styx is a modified version of Acheron, which
is also GPL-3.0. The first commit in this repository is Acheron's source as of
9 October 2026, and every change since then is in the commit history.
