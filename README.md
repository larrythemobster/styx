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
    <a href="https://github.com/larrythemobster/styx/stargazers"><img src="https://img.shields.io/badge/GitHub-Star%20Styx-181717?style=flat-square&amp;logo=github" alt="Star Styx on GitHub"></a>
  </p>
</div>

## Status

Experimental. Windows is the primary development platform; Linux and macOS
support is incomplete. Calls and streaming still need more live testing.

> [!WARNING]
> Third-party clients violate Discord's Terms of Service and may put your account
> at risk. Styx is not affiliated with Discord.

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

To run the renderer and animation tests from an MSVC prompt:

```bat
cmake --build build --target TestRmlRender TestClips
ctest --test-dir build -R "^(RmlRenderTests|ClipTests)$" --output-on-failure
```

</details>

## Credits and license

Styx is a modified version of [Acheron](https://github.com/ouwou/acheron) by
[ouwou](https://github.com/ouwou), forked on 9 October 2026. Acheron provides much
of the client, networking, storage and voice code; its `video-streams` branch
also provided the video engine and stream viewing.

Icons by [Lucide](https://lucide.dev). The application mark uses the Discord
symbol from [Simple Icons](https://github.com/simple-icons/simple-icons), under CC0.

Licensed under [GPL-3.0](LICENSE.txt).
