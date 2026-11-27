# Real Racing 3 PC Port

[![Release](https://img.shields.io/github/v/release/PythonnXD/Real-Racing-3-PC-Port)](https://github.com/PythonnXD/Real-Racing-3-PC-Port/releases)
[![Downloads](https://img.shields.io/github/downloads/PythonnXD/Real-Racing-3-PC-Port/total)](https://github.com/PythonnXD/Real-Racing-3-PC-Port/releases)
[![License](https://img.shields.io/github/license/PythonnXD/Real-Racing-3-PC-Port)](LICENSE)
![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078D6)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C)

A native Windows x64 port of Real Racing 3 (Android). It runs offline through Direct3D 11 (via ANGLE) and WASAPI audio, with keyboard, gamepad, and a built-in launcher packed into one executable.

> [!WARNING]
> This repo and its releases do not include any EA or Firemonkeys game files. You need to provide your own Real Racing 3 APK and `com.ea.games.r3_row` data folder.

## What's in it

Opening `rr3_pc.exe` brings up the launcher. Hit **Play** and the game starts right in the same process (or run `rr3_pc.exe --play` to skip the launcher).

Under the hood, Dynarmic translates the game's ARM64 binary into native x86-64 code on the fly, backed by a custom ELF64 loader, Bionic/JNI shims, ANGLE (OpenGL ES over Direct3D 11), DirectWrite text rendering, and WASAPI audio.

From the launcher you can tweak:
- Windowed, Borderless, and Fullscreen modes (`F10` cycles modes, `F11` or `Alt+Enter` toggles fullscreen) with automatic monitor fit and no black bars on landscape displays
- V-Sync, frame rate cap, and removing the 30 FPS menu cap
- Native RR3 quality presets (`Low`, `Medium`, `High`, `Ultra`), render scale (`75%` to `200%`), and anisotropic filtering (`Off` to `16x`)
- Optional verbose console logs
- Remappable keyboard controls (`W`/`Up` throttle, `S`/`Space` brake, `A`/`D` or Arrows steer, `C` camera, `V` look back, `Esc` pause)
- Plug-and-play Xbox, DualShock 4, DualSense, and Switch Pro controllers (`A`/`RT` throttle, `X`/`LT` brake, `LB`/`Y` camera, `RB`/`B` look back, Left Stick or D-Pad steer)
- Save import and export (`character.2.dat` or `.zip` backups)

## Installing

> [!CAUTION]
> Don't extract the game into `C:\Program Files`, since Windows can block save writes there. Use a normal folder like `C:\Games\RealRacing3PC`. The launcher backs up your existing `userdata\doc` folder before importing a save, though keeping your own copy is still smart.

1. Grab `RealRacing3-PC-Port.zip` from [Releases](https://github.com/PythonnXD/Real-Racing-3-PC-Port/releases) and extract it somewhere writable like `C:\Games\RealRacing3PC`.
2. Make sure you have your **Real Racing 3 APK** (`arm64-v8a`) and **`com.ea.games.r3_row`** data folder (the one with `files\.depot` inside).
3. Run `rr3_pc.exe` and go to **Files & saves**.
4. Pick your **Real Racing 3 APK** and click **Extract**. This copies `libRealRacing3.so`, `libc++_shared.so`, and `libfmodex.so` into `work_bins`.
5. Pick your `com.ea.games.r3_row` folder (or its `files` subfolder) under **Data folder**. Once both check out, the bottom-left status switches to **Ready to launch**.
6. If you have an existing Android save, click **Import save** and pick your `character.2.dat` or `.zip` backup.
7. Set up Display, Graphics, and Controls however you like, then hit **Play**.

Your original APK and `com.ea.games.r3_row` folder are never modified. Everything the game writes goes into the local `userdata` folder.

## System requirements

> [!IMPORTANT]
> Both 64-bit Windows and a 64-bit (`arm64-v8a`) Android APK are required. 32-bit Windows and `armeabi-v7a` APKs are not supported.

| | Minimum | Recommended |
| --- | --- | --- |
| OS | Windows 10 and up | Windows 10 and up |
| CPU | Intel Core i3-6100 / AMD Ryzen 3 1200 | Intel Core i5-10400 / AMD Ryzen 5 3600 |
| RAM | 4 GB | 8 GB |
| Graphics | Intel UHD 620 / NVIDIA GTX 750 / AMD R7 260 | NVIDIA GTX 1050 / AMD RX 560 |
| Storage | 5 GB free space | 8 GB free space (SSD recommended) |

## FAQ

### Is this an emulator?
No. There's no Android OS, Linux kernel, or VM running. `rr3_pc.exe` translates ARM64 instructions straight into native x86-64 code with Dynarmic and calls Windows APIs (Direct3D 11 via ANGLE, WASAPI, DirectWrite, SDL2) directly.

### Can you provide the game data?
No. All tracks, cars, audio, UI assets, and game binaries belong to EA and Firemonkeys, so you have to supply your own copy.

### Why does it need the APK?
The `com.ea.games.r3_row` folder only has assets inside `.depot`. The compiled engine libraries (`libRealRacing3.so`, `libc++_shared.so`, and `libfmodex.so`) live inside the APK, so the launcher extracts them once to run the game.

### What version does it work with right now?
Tested on Real Racing 3 v14.0.1 (`arm64-v8a`).

### Can it work with other versions of Real Racing 3?
Untested. Other `arm64-v8a` builds may boot, though some PC patches are locked to the tested binary so unknown builds don't break. 32-bit `armeabi-v7a` APKs won't work.

### Is it done?
Playable offline from menus through full races. Online modes stay disabled, and fixes are still ongoing.

### Will you make the port for Linux and macOS?
Windows is the focus right now, though a Linux port is possible later since most of the core runtime (`Dynarmic`, `SDL2`, `zlib`) is already cross-platform and only the Windows-specific layers (Win32 launcher, DirectWrite, ANGLE D3D11, and Win32 memory calls) need Linux equivalents. Community PRs are welcome.

If you're on an Apple Silicon Mac, someone has already put together a macOS port you can check out [here on Reddit](https://www.reddit.com/r/RealRacing3/s/UUhAgimbK8).

## Building from source

You'll need Visual Studio 2022 (MSVC x64 C++20), CMake, and Ninja, plus these dependencies under `port/third_party/`:

- `dynarmic` (A64 frontend with bundled externals)
- `boost` (headers for Dynarmic)
- `zlib`
- `SDL2` (VC x64 prebuilt)
- `angle/angle-x64` (`libEGL.dll`, `libGLESv2.dll`, `d3dcompiler_47.dll`, headers, and `.lib`)
- `single_header/rg_etc1.{h,cpp}`

Then build and package:

```bat
port\build.cmd rr3_pc test_input_resize
port\package_dist.cmd
```

## AI usage

I used AI to help write parts of the codebase, including the runtime shims, graphics and audio bridges, and the Win32 launcher. Every build and fix gets compiled and tested on real Windows hardware against the Android version so the visuals, audio, UI, quality presets, and controls match the original game.

## Credits

Built with help from these open-source projects:

- [azahar-emu/dynarmic](https://github.com/azahar-emu/dynarmic) (fork of [merryhime/dynarmic](https://github.com/merryhime/dynarmic)): ARM64-to-x64 JIT
- [azahar-emu/ext-boost](https://github.com/azahar-emu/ext-boost): Boost headers for Dynarmic
- [google/angle](https://github.com/google/angle) (Win64 builds from [mmozeiko/build-angle](https://github.com/mmozeiko/build-angle)): OpenGL ES to Direct3D 11
- [libsdl-org/SDL](https://github.com/libsdl-org/SDL): windowing, WASAPI audio, and keyboard/gamepad input
- [madler/zlib](https://github.com/madler/zlib): ZIP inflation, PNG dumps, and save backups
- [richgel999/rg-etc1](https://github.com/richgel999/rg-etc1): ETC1 texture decoding
- [nothings/stb](https://github.com/nothings/stb): `stb_image.h`

## License

The runtime and launcher source code is licensed under the [GNU General Public License v3.0](LICENSE). You can use, modify, and redistribute it under GPL-3.0 terms.

Real Racing 3 and all associated trademarks, binaries, and game assets belong to Electronic Arts Inc. and Firemonkeys.
