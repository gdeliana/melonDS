<p align="center"><img src="https://raw.githubusercontent.com/melonDS-emu/melonDS/master/res/icon/melon_128x128.png"></p>
<h2 align="center"><b>melonDS</b></h2>
<p align="center">
<a href="http://melonds.kuribo64.net/" alt="melonDS website"><img src="https://img.shields.io/badge/website-melonds.kuribo64.net-%2331352e.svg"></a>
<a href="http://melonds.kuribo64.net/downloads.php" alt="Release: 1.1"><img src="https://img.shields.io/badge/release-1.1-%235c913b.svg"></a>
<a href="https://www.gnu.org/licenses/gpl-3.0" alt="License: GPLv3"><img src="https://img.shields.io/badge/License-GPL%20v3-%23ff554d.svg"></a>
<a href="https://kiwiirc.com/client/irc.badnik.net/?nick=IRC-Source_?#melonds" alt="IRC channel: #melonds"><img src="https://img.shields.io/badge/IRC%20chat-%23melonds-%23dd2e44.svg"></a>
<a href="https://discord.gg/pAMAtExcqV" alt="Discord"><img src="https://img.shields.io/badge/Discord-Kuribo64-7289da?logo=discord&logoColor=white"></a>
<br>
<a href="https://github.com/melonDS-emu/melonDS/actions/workflows/build-windows.yml?query=event%3Apush"><img src="https://github.com/melonDS-emu/melonDS/actions/workflows/build-windows.yml/badge.svg" /></a>
<a href="https://github.com/melonDS-emu/melonDS/actions/workflows/build-ubuntu.yml?query=event%3Apush"><img src="https://github.com/melonDS-emu/melonDS/actions/workflows/build-ubuntu.yml/badge.svg" /></a>
<a href="https://github.com/melonDS-emu/melonDS/actions/workflows/build-macos.yml?query=event%3Apush"><img src="https://github.com/melonDS-emu/melonDS/actions/workflows/build-macos.yml/badge.svg" /></a>
<a href="https://github.com/melonDS-emu/melonDS/actions/workflows/build-bsd.yml?query=event%3Apush"><img src="https://github.com/melonDS-emu/melonDS/actions/workflows/build-bsd.yml/badge.svg" /></a>
</p>
DS emulator, sorta

The goal is to do things right and fast, akin to blargSNES (but hopefully better). But also to, you know, have a fun challenge :)
<hr>

## About this fork

This fork builds on [upstream melonDS](https://github.com/melonDS-emu/melonDS)
with additional filtering, performance improvements and loading fixes. It aims
to reduce avoidable CPU/GPU work while preserving emulation timing and audio
quality.

- **GPU xBRZ filtering:** edge-aware upscaling for software and OpenGL renderer
  output, including fractional scaling and enhanced resolutions. The Filtering
  submenu offers one saved choice per window: nearest neighbor, bilinear or xBRZ.
- **More robust in-memory archive loading:** ZIP ROMs decompress directly into
  the ROM buffer in chunks, with no temporary ROM files. Missing, corrupt,
  truncated, encrypted and oversized entries fail cleanly. Relative archive
  paths now resolve their default save location correctly. Archive support was
  already present upstream; this fork strengthens its streaming and validation.
- **Faster texture handling:** reuse converted palettes for compressed,
  alpha-indexed and ordinary indexed textures, avoid repeated invalidation
  hashing, and correct dirty-map indexing for wrapped textures.
- **Less redundant graphics work:** improve regular OpenGL 3D buffer uploads,
  skip unchanged 2D and compute metadata uploads, and reuse texture uploads and
  xBRZ analysis when presenting the same emulated frame. Partial capture
  conversion avoids unnecessary rows, and wrapped readbacks respect their
  requested length.
- **Audio improvements:** bulk ring-buffer copies, fewer redundant rate updates
  and less mixing work for disabled channels. Callback fixes fill the entire
  output buffer and bound reads to its capacity.
- **Regression checks and measurements:** standalone CPU, archive, audio and
  real-OpenGL tests, with reproduction commands in [PERFORMANCE.md](./PERFORMANCE.md).

Performance figures in that document are isolated benchmarks. Whole-game FPS,
CPU/GPU usage and power savings have not been measured, and gains depend on the
game, renderer, settings and hardware.

## Platform status

| Platform | Status of this fork |
| --- | --- |
| Linux | Built successfully in the `melonds-gpu` development container. Automated graphics checks passed on an NVIDIA GPU with OpenGL 3.2, 3.3 and 4.3 contexts. |
| Windows and macOS | Upstream build instructions are retained; these fork changes have not been built or tested on those platforms here. |
| Android | This repository has no Android frontend or APK build target. Bringing these changes to Android requires a separate porting and testing pass. |

The separate [melonDS Android port](https://github.com/rafaelvcaetano/melonDS-android)
provides an Android frontend and SDK/NDK build setup. Core improvements would
need integration there; the desktop filtering UI, archive frontend and OpenGL
paths require adaptation to its Android APIs and graphics backend.

## How to use

Firmware boot (not direct boot) requires a BIOS/firmware dump from an original DS or DS Lite.
DS firmwares dumped from a DSi or 3DS aren't bootable and only contain configuration data, thus they are only suitable when booting games directly.

### Possible firmware sizes

 * 128KB: DSi/3DS DS-mode firmware (reduced size due to lacking bootcode)
 * 256KB: regular DS firmware
 * 512KB: iQue DS firmware

DS BIOS dumps from a DSi or 3DS can be used with no compatibility issues. DSi BIOS dumps (in DSi mode) are not compatible. Or maybe they are. I don't know.

As for the rest, the interface should be pretty straightforward. If you have a question, don't hesitate to ask, though!

### ROMs in ZIP files

Open a `.zip` using **File > Open ROM**, drag it onto the window, or pass it on
the command line. A ZIP containing one supported ROM loads automatically; if
there are several, select the ROM to play. DS and GBA ROMs can be in subfolders.

```sh
./build/melonDS games.zip
./build/melonDS -a "folder/game.nds" games.zip
```

The selected ROM decompresses in chunks directly into memory, with no temporary
ROM file written to disk. The complete uncompressed ROM stays in memory while
playing. Corrupt, truncated, encrypted and oversized entries are rejected.
Saves use the selected ROM's name and the configured save location, defaulting
to the archive's folder.

### xBRZ filtering

With OpenGL display enabled in **Config > Video settings**, enable
**View > Filtering > xBRZ filtering** for edge-aware pixel-art upscaling. This GPU-based
freescale filter supports fractional window sizes and both 3D renderers. Choose
one option in the submenu: **None (nearest neighbor)**, **Bilinear filtering**, or
**xBRZ filtering**. The choice is saved per window. xBRZ leaves
native-size images unchanged. It does not increase the internal 3D resolution.
Edge analysis runs once per source pixel on the GPU for each new frame;
repeated presentations reuse the analysis. The window-resolution pass only
blends the classified edges. Both software and OpenGL renderer outputs are
supported, including enhanced OpenGL resolutions.

See [PERFORMANCE.md](./PERFORMANCE.md) for optimization details, benchmarks and
regression-check commands.

### Portable settings

On Linux, run `./run-melonds.sh` beside your AppImage or native executable. It
creates the portable settings directory automatically and imports the system
`melonDS.toml` (normally `~/.config/melonDS/melonDS.toml`, respecting
`XDG_CONFIG_HOME`) only if portable settings do not already exist. Later launches
preserve those settings.

```sh
./run-melonds.sh
./run-melonds.sh --binary /path/to/melonDS-x86_64.AppImage
./run-melonds.sh --fresh
```

AppImage settings are stored in `<AppImage filename>.config/melonDS/`; native
builds use `portable/` beside the executable. The launcher also finds the default
build outputs when run from this checkout. ROM arguments are forwarded unchanged.
Only the settings file is imported; existing BIOS, firmware and ROM paths are
kept as configured. `--fresh` skips import without deleting existing settings.
AppImage builds place a copy of the launcher in `build-appimage/dist/`.

## How to build
See [BUILD.md](./BUILD.md) for build instructions.

To build an AppImage with local Linux dependencies, run `make appimage`.
With the existing Distrobox environment, use `make appimage CONTAINER=melonds-gpu`.
The package is written to `build-appimage/dist/`; see
[AppImage build details](./BUILD.md#appimage) for prerequisites and overrides.

## TODO LIST

 * better DSi emulation
 * better OpenGL rendering
 * netplay
 * the impossible quest of pixel-perfect 3D graphics
 * support for rendering screens to separate windows
 * emulating some fancy addons
 * other non-core shit (debugger, graphics viewers, etc)

### TODO LIST FOR LATER (low priority)

 * big-endian compatibility (Wii, etc)
 * LCD refresh time (used by some games for blending effects)
 * any feature you can eventually ask for that isn't outright stupid

## Credits

 * Martin for GBAtek, a good piece of documentation
 * Cydrak for the extra 3D GPU research
 * limittox for the icon
 * All of you comrades who have been testing melonDS, reporting issues, suggesting shit, etc

## Licenses

[![GNU GPLv3 Image](https://www.gnu.org/graphics/gplv3-127x51.png)](http://www.gnu.org/licenses/gpl-3.0.en.html)

melonDS is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

### External
* Images used in the Input Config Dialog - see `src/frontend/qt_sdl/InputConfig/resources/LICENSE.md`
* xBRZ freescale shader, adapted from libretro/glsl-shaders - see `src/frontend/qt_sdl/xbrz_shader.h` for Hyllian and Zenju's license notices.
