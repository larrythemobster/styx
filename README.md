<div align="center">
  <img src="resources/app/styx.svg" alt="Styx logo" width="88" height="88">
  <h1>Styx</h1>
  <p>A native Discord client, written in C++.</p>
  <p>
    <a href="#features">Features</a> ·
    <a href="#roadmap">Roadmap</a> ·
    <a href="#build-from-source">Build</a> ·
    <a href="https://github.com/larrythemobster/styx/issues">Issues</a>
  </p>
  <p>
    <a href="#status"><img src="https://img.shields.io/badge/status-experimental-d29922?style=flat-square" alt="Status: experimental"></a>
    <a href="CMakeLists.txt"><img src="https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square&amp;logo=cplusplus&amp;logoColor=white" alt="C++20"></a>
    <a href="LICENSE.txt"><img src="https://img.shields.io/badge/license-GPLv3-555555?style=flat-square" alt="License: GPLv3"></a>
  </p>
</div>

## Status

Experimental. Windows is the primary development platform; Linux and macOS
support is incomplete. Calls and streaming still need more live testing.

> [!WARNING]
> Third-party clients violate Discord's Terms of Service and may put your account
> at risk. Styx is not affiliated with Discord.

## Download

The first pre-release is [v0.1.0-alpha.1](https://github.com/larrythemobster/styx/releases/tag/v0.1.0-alpha.1).
It is an alpha build, not a stable release.

On Windows x64, download `styx-v0.1.0-alpha.1-windows-x64.zip` from the release,
extract the entire archive, and run `bin/styx.exe`. Keep the DLLs, resources and
licenses beside the executable. The [Microsoft Visual C++ x64 runtime](https://aka.ms/vs/17/release/vc_redist.x64.exe)
is required.

Linux and macOS users need to build from source; this release has no binaries for
those platforms.

## Local credentials and logs

Discord tokens and proxy usernames and passwords are stored in the operating
system's credential store. SQLite keeps proxy endpoints and credential references,
not proxy passwords. Unlock the system credential store before starting Styx.

Existing proxy credentials migrate automatically. Migration removes plaintext
credentials from Styx's active account database and WAL, but cannot clean copies
in backups or snapshots.

Older versions logged complete gateway payloads, including private messages and
voice-session credentials. Treat existing `styx.log` files and rotated copies as
private. Do not attach them to public issues. Remove old copies you no longer need
and replace any credentials exposed through shared logs or database backups.

## Features

- Multiple accounts with account switching.
- Server chats, DMs and group DMs, with replies, reactions, embeds, GIFs and stickers.
- Message search, threads, and polls with voting and poll creation.
- Voice channels and outgoing DM calls with DAVE end-to-end encryption.
- Windows screen and window sharing, hardware encoding, and optional app or system audio.
- Stream viewing, full profiles, and clickable mutual friends and servers.
- Global and per-server profile editing, subject to Discord's account restrictions.
- Custom accent colors, light and dark themes, font controls and keybinds.
- Desktop notifications, server muting, and per-server privacy and notification settings.

## Roadmap

Current priorities, not a complete feature list:

- Settings and menus closer to Discord, with more account and privacy controls.
- Call stability, incoming call controls, camera calls and microphone diagnostics.
- Smoother streaming and screen sharing on Linux.
- Better chat tools: autocomplete, pins, inline media and attachment controls.
- Message requests, forwarding, server events and soundboard support.
- Accessibility, translations and better Linux/macOS support.

## Built with

[SDL3](https://www.libsdl.org/) · [RmlUi](https://github.com/mikke89/RmlUi) ·
[OpenGL](https://www.opengl.org/) · [FFmpeg](https://ffmpeg.org/) ·
[libdatachannel](https://github.com/paullouisageneau/libdatachannel) ·
[DAVE](https://github.com/discord/libdave)

## Build from source

GitHub Actions builds are manual. Choose a platform; Windows is the default.
Artifact uploads are optional and expire after three days.

<details>
<summary>Windows build instructions</summary>

Requires Visual Studio 2022 Community with C++ tools, CMake 3.21+, Ninja and an
OpenGL 3.3 driver. Put prebuilt [FFmpeg](https://ffmpeg.org/download.html) and
[curl-impersonate](https://github.com/lexiforest/curl-impersonate) in the
`ffmpeg` and `curl-impersonate` subfolders of your dependency directory.
The remaining dependencies are installed through the bundled vcpkg submodule.

```bat
git clone --recursive https://github.com/larrythemobster/styx.git
cd styx
set DEPS=C:\path\to\deps
scripts\build-windows.bat configure
```

Run `build\styx.exe`, keeping its DLLs and resources beside it. For later builds,
run `scripts\build-windows.bat` without `configure`. Set `BUILD_DIR` to override
`build`. Linux and macOS build configurations are in the [CI workflow](.github/workflows/build.yml).

The credential tests use the native credential store with disposable entries.

```bat
cmake --build build --target TestAccountRepository TestGatewayLogging TestProxy
ctest --test-dir build -R "^(AccountRepositoryTests|GatewayLoggingTests|ProxyTests)$" --output-on-failure
```

To check protobuf decoding and patches, including sensitive-media preferences:

```bat
cmake --build build --target TestProto TestPreloadedSettings
ctest --test-dir build -R "^(ProtoTests|PreloadedSettingsTests)$" --output-on-failure
```

To run the renderer and animation tests from an MSVC prompt:

```bat
cmake --build build --target TestRmlRender TestClips
ctest --test-dir build -R "^(RmlRenderTests|ClipTests)$" --output-on-failure
```

</details>

## Credits and license

Based on [Acheron](https://github.com/ouwou/acheron) by [ouwou](https://github.com/ouwou).

Icons by [Lucide](https://lucide.dev).

Licensed under [GPL-3.0](LICENSE.txt). [Third-party notices](NOTICE.txt).
