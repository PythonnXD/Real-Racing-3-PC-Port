# Real Racing 3 PC Port

A native Windows x64 runtime and launcher for **Real Racing 3 (Android)**. It runs offline with Direct3D 11 rendering through ANGLE, WASAPI audio, keyboard and gamepad support, and a native Win32 launcher.

> **Game files notice:** This repository does not ship with game data, you have to provide the game's APK and Data to able to run.

## What works

- **Native Win64 runtime:** Runs the ARM64 binary with Dynarmic JIT, 64-bit fastmem, a custom ELF64 loader, and Bionic libc, JNI, and OpenSL ES to WASAPI shims.
- **Direct3D 11 rendering:** Translates OpenGL ES 2.0/3.0 to D3D11 via ANGLE, decodes ATC and ETC1 textures, renders fonts with DirectWrite, and supports up to 16x anisotropic filtering.
- **PC display and graphics:**
  - Configurable V-Sync, frame rate cap, menu frame rate limit removal, native RR3 Quality presets (`Low`, `Medium`, `High`, `Ultra`), render scale (`75%` to `200%`), anisotropic filtering (`Off` to `16x`), and verbose console logging.
- **Keyboard and gamepad controls:**
  - Smooth keyboard steering ramp with remappable keys (`W`/`Up` throttle, `S`/`Space` brake, `A`/`D`/Arrows steer, `C` camera, `V` look back, `Esc` pause).
  - Automatic Xbox, PlayStation DualShock 4, DualSense, and Nintendo Switch Pro support via SDL2 HIDAPI (`A`/`RT` throttle, `X`/`LT` brake, `LB`/`Y` camera, `RB`/`B` look back, Left Stick or D-Pad steer).
- **Built-in launcher (`rr3_pc.exe`):**
  - Configure display, graphics, controls, and verbose logs from one window.
  - Built-in APK extractor that pulls the required ARM64 `.so` libraries and checks the v14.0.1 SHA-256 hash.
  - Save import and export with automatic timestamped backups before any overwrite.

## Quick start (prebuilt release)

1. Download `RealRacing3-PC-Port.zip` from Releases and extract it.
2. Open `rr3_pc.exe` and switch to **Files & saves**.
3. Pick the APK of Real Racing 3 you downloaded.
4. Pick the Data of Real Racing 3 you downloaded.
5. Click **Play**.

## Building from source (Windows x64)

### Prerequisites
- Visual Studio 2022 / MSVC x64 C++20 toolchain, CMake, and Ninja
- Third-party dependencies in `port/third_party/`:
  - `dynarmic` (A64 frontend with bundled externals)
  - `boost` (headers required by Dynarmic)
  - `zlib`
  - `SDL2` (VC x64 prebuilt)
  - `angle/angle-x64` (`libEGL.dll`, `libGLESv2.dll`, `d3dcompiler_47.dll`, headers, and `.lib`)
  - `single_header/rg_etc1.{h,cpp}` (public-domain ETC1 decoder)

### Build and package
```bat
port\build.cmd rr3_pc test_input_resize
port\package_dist.cmd
```

## License

The runtime and launcher source code is provided for educational and preservation use. Real Racing 3 and all associated assets and trademarks belong to Electronic Arts Inc. and Firemonkeys.
