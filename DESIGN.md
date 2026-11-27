# Real Racing 3 Native Windows PC Port — Design Document

## 1. Intent & Goals
Produce a standalone, native Windows x64 PC build (`rr3_pc.exe`) of **Real Racing 3 v14.0.1** from the user's local read-only Android APK (`d:\Real Racing 3\RR3 14.0.1.apk`) and unpacked data (`d:\Real Racing 3\com.ea.games.r3_row\`).

### Explicit Requirements
1. **Standalone native Windows x64 program** — NOT an Android emulator/VM (no BlueStacks, MEmu, Waydroid, WSA, or Android OS guest).
2. **100% Offline operation** — no EA servers, no login, no Google Play Services, no outbound network connections.
3. **Native keyboard controls** — full driving and menu navigation with smooth, controllable steering and remappable bindings.
4. **Native gamepad support** — Xbox (XInput), DualShock 4, DualSense, and Nintendo Switch Pro Controller with analog steering/throttle/brake and hot-plug detection.
5. **60 FPS and high-refresh-rate support** — smooth 60 FPS baseline, plus support for higher monitor refresh rates (>60 Hz) if the Mint3 engine's timestep/physics allows it safely.
6. **Dynamic window rescaling & crisp PC visuals** — DPI-aware resizable window (plus borderless/fullscreen toggle) that dynamically rescales the game viewport on resize, paired with crisp high-resolution rendering (`AndroidXtraHigh` / `AndroidUltraWide` tier, optional internal resolution scaling, anisotropic filtering, and MSAA where compatible).
7. **Read-only originals + customizable PC overlays** — keep original `RR3 14.0.1.apk` and `com.ea.games.r3_row\` intact, while allowing modifications/overrides on working copies and VFS overlays (including PC-adapted in-game text, button prompts, UI labels, `.plist` presets, and shaders); work only inside `d:\Real Racing 3\`.

### Non-Goals & Anti-Goals
- **Non-goals:** Online multiplayer, Time Shifted Multiplayer (TSM) cloud sync, EA/Facebook/Google Play login, IAP store, in-game ads (`Anzu`/`GluAds`/`Tapjoy`/`AppLovin`), ARCore camera mode, Android NFC.
- **Anti-goals:** Wrapping an Android emulator/VM; mutating the user's original APK or `com.ea.games.r3_row\` folder; blurry OS bitmap stretching on resize; claiming anything works without verified build/run/visual proof.

---

## 2. Verified Target & Host Constraints

### Target (`RR3 14.0.1.apk`, SHA-256 `724103c66b32c1c782a52eae686d5ae4ee85581b860be7015f6b45d84829c9c5`)
- **Primary engine binary:** `lib/arm64-v8a/libRealRacing3.so` (33,110,472 bytes; `.text` = 21.56 MB; stripped AArch64 ELF with intact C++ RTTI; `PT_TLS=[]`, uses `__emutls_get_address`; 181 `.init_array` constructors; 706 dynamic imports; 124 `Java_*` exports).
- **Shipped runtime libraries:** `lib/arm64-v8a/libc++_shared.so` (1,014,704 bytes) and `lib/arm64-v8a/libfmodex.so` (1,274,928 bytes; 94 libc/libm imports; 1,033 exports; `PT_TLS=[]`; exports `Java_org_fmod_FMODAudioDevice_fmodProcess` and `fmodGetInfo`).
- **Graphics surface:** Pure **OpenGL ES 2.0** (142 `gl*` + 3 `egl*` imports: `eglGetDisplay`, `eglGetProcAddress`, `eglQueryString`). Zero Vulkan imports. Plaintext GLSL shaders in `.depot\Shaders\`.
- **Engine lifecycle (`classes2.dex`):** Classic `GLSurfaceView` callback model on `com.firemint.realracing.MainActivity`: `onCreateJNI`, `onStartJNI`, `onResumeJNI`, `onViewCreatedJNI`, `onViewChangedJNI(IIII)`, `onViewRenderJNI(II)`, `onPauseJNI`, `onStopJNI`, `onDestroyJNI`, `onWindowFocusChangedJNI(Z)`, `SetRunningOnAndroidTv(Z)`, `setSafeArea(II)`.
- **Built-in native input exports:** `ControllerManager` (`ControllerConnectedJNI`, `ControllerDisconnectedJNI`, `SetButtonValueJNI`, `SetJoystickValueJNI`, with `ControllerAxis` and `ControllerButtons` enums) and `MainActivity` (`onKeyPressed`, `onKeyReleased`, `onTouchBeginJNI`, `onTouchMoveJNI`, `onTouchEndJNI`, `onTouchCancelJNI`).
- **Game data:** `com.ea.games.r3_row\` contains 39,520 files (7.31 GB), including `.depot\eds\AndroidXtraHigh.plist` & `AndroidUltraWide.plist`, plaintext shaders, audio banks, car/track depots, `apk\res.zip`, and `doc\` saves.

### Host Machine
- **OS/CPU/GPU:** Windows x64 (`AMD64`), **AMD Ryzen 7 5700X (8C/16T)**, **AMD Radeon RX 580 2048SP** (Driver `31.0.21925.1001`).
- **Existing toolchain:**
  - MSVC x64: `D:\Visual Studio\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\cl.exe` (`vcvars64.bat` at `D:\Visual Studio\VC\Auxiliary\Build\vcvars64.bat`)
  - CMake: `D:\Visual Studio\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
  - Ninja: `D:\Visual Studio\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe`
  - Windows SDK: `10.0.26100.0`
  - Ghidra: `C:\Users\Administrator\Desktop\ghidra_12.2_PUBLIC` (`analyzeHeadless.bat`, Java 26.0.1)
  - Python 3, Git, 7-Zip, Node.js, Rust 1.99.1.
- **Additional third-party libraries to fetch/build inside workspace (`d:\Real Racing 3\port\third_party\`):**
  - **Dynarmic** (ARM64->x86_64 JIT, C++20 CMake subproject + its small header/build deps: `fmt`, `mcl`, `xbyak`, `zycore`/`zydis` or via Dynarmic's CMake/vcpkg)
  - **SDL2** (or **SDL3**) — windowing, DPI/resize, WASAPI audio, keyboard, native XInput + HID DualShock 4 / DualSense / Switch Pro controller support
  - **Google ANGLE** (`libEGL.dll` + `libGLESv2.dll`, prebuilt x64 Windows binaries or built from source) — hardware-accelerated D3D11/Vulkan OpenGL ES 2.0/3.0
  - **zlib**, **stb_image** (single-header image decoding for `Platform.loadTextureFromMemory`), **rg_etc1** (single-file ETC1 compression for `CarLiveryBaker.compressTextureToETC1` if needed), and **DirectWrite** (already in Windows SDK) for `Font`/`GlyphVector` text rendering
  - **JADX CLI** (optional portable ZIP inside `port\tools\` for clean Java decompilation of `classes2.dex` in Phase 1).

---

## 3. Approaches Considered

| Approach | Summary | Tradeoffs | Decision |
|---|---|---|---|
| **A. Native Windows ARM64 JIT Loader (Dynarmic) + HLE Android/JNI/ANGLE/SDL2 Host** | Standalone C++20 x64 `.exe` loads `arm64-v8a` `libRealRacing3.so`, `libc++_shared.so`, and `libfmodex.so` into flat 64-bit guest memory, executes ARM64 code via Dynarmic JIT, and thunks ~450 external imports to native Windows APIs (ANGLE GLES2, SDL2/WASAPI, Win32 threads, layered VFS, Fake JNI). | ~2–4x CPU JIT overhead (easily hits 60–144+ FPS for a 2013 mobile game on desktop x64 + native RX 580 GPU). Zero brittle static CFG/vtable/EH recovery; handles stripped C++ RTTI/exceptions/`setjmp`/FMOD cleanly; flat 64-bit pointers = zero-copy GL/audio/file transfers. | **Selected** |
| **B. Static ARM64->C/LLVM Recompiler (AOT) + Same Host Runtime** | Same host shim layer, but statically lifts 21.56 MB stripped ARM64 `.text` + `libc++_shared.so` + `libfmodex.so` to x64 C/LLVM IR. | Native AOT CPU speed, but high upfront risk recovering ~50k+ stripped C++ functions, indirect virtual calls, jump tables, `_Unwind_Resume`, and `setjmp`/`longjmp` before seeing frame 1. Remains a possible future drop-in backend once Approach A's host runtime boots the game. | Deferred |
| **C. Clean-Room Mint3 Engine Rewrite** | Reimplement rendering, physics, AI, UI, and binary formats from scratch on Windows. | Multi-year effort; cannot realistically preserve RR3 14.0.1 physics/content parity. | Rejected |

---

## 4. Detailed Architecture (Approach A)

### 4.1 Workspace Directory Layout (`d:\Real Racing 3\`)
- `RR3 14.0.1.apk` — **read-only**
- `com.ea.games.r3_row\` — **read-only**
- `PORT_NOTES.md` — phase log, findings, exact build/run commands
- `DESIGN.md` — this design document
- `port\` — all generated code, tools, working copies, and build outputs:
  - `port\work_bins\arm64-v8a\` — extracted copies of `libRealRacing3.so`, `libc++_shared.so`, `libfmodex.so`, `libNimble.so`
  - `port\analysis\` — Ghidra project & decompiled Java references (`classes2.dex` Firemint/Cloudcell classes)
  - `port\src\` — C++20 native host implementation:
    - `elf_loader.*` — AArch64 ELF mapper, relocator, symbol resolver, SVC thunk stub generator
    - `cpu_dynarmic.*` — Dynarmic `A64::Jit` wrapper, per-thread CPU instances, shared `ExclusiveMonitor`, Guest<->Host call bridge & re-entrant callback stack, AAPCS64 argument/return marshalling (including variadic `va_list` / float registers)
    - `bionic_libc.*` — Bionic POSIX/libc/libm/zlib/pthread/sem/`__emutls_get_address`/`setjmp`/`longjmp`/`dlopen`/`dlsym` shims + exact AArch64 Linux `struct stat`, `struct dirent`, `errno`, `__sF` ABI translations
    - `vfs.*` — Layered copy-on-write virtual filesystem (maps Android paths like `/data/...` & external storage to read-only `com.ea.games.r3_row\files\` plus writable overlay `port\userdata\`, pre-seeded with a one-time copy of `doc\`)
    - `gles_bridge.*` — Direct thunking of all 142 `gl*` + 3 `egl*` + `eglGetProcAddress` dynamic extensions to ANGLE (`libEGL.dll` / `libGLESv2.dll`), plus optional forced 16x anisotropic filtering, MSAA, and internal resolution scaling hooks
    - `jni_env.*` — Full `JavaVM` + `JNIEnv` vtable, handle table (local/global refs, strings, primitive/object arrays, DirectByteBuffer), reflection registry
    - `java_firemint.*` — Native C++ implementations of `MainActivity`, `Platform`, `Font` + `GlyphVector` (DirectWrite -> GL texture), `TextureInfo` (`stb_image` -> GL texture), `CarLiveryBaker` (ETC1 compression / fallback), `ControllerManager`, `AudioStreamManager`, `Input`, `TextField` (minimal SDL `SDL_StartTextInput` / `SDL_TEXTINPUT` bridge, zero extra deps), and `MoviePlayer` (clean skip stub immediately invoking `OnCompletionNative`; no FFmpeg/Media Foundation dependency in initial milestones)
    - `java_offline_stubs.*` — Clean offline/no-op stubs for `NetworkStatusMonitor` (always offline), `Http`/`HttpRequest` (immediate error callback, zero sockets), `GooglePlayWorker`, `FacebookWorker`, `AndroidAccountManager`, `AndroidAssetManagerService`, `Nimble*`, `GoogleNativeAdManager`, `csdk.*`, `Ar*`, `Anzu_*`, `libfuelmetrics`
    - `audio_fmod.*` — Runs guest `libfmodex.so` and drives `FMODAudioDevice_fmodGetInfo` / `fmodProcess` PCM output into SDL2/WASAPI audio stream
    - `input_pc.*` — SDL2 GameController (Xbox/XInput, DualShock 4, DualSense, Switch Pro) + remappable keyboard + mouse-to-touch bridge into `ControllerManager_*JNI` & `MainActivity`
    - `main.cpp` — DPI-aware resizable SDL2 window, ANGLE EGL context setup, refresh-rate / frame-pacing loop, resize handler, config loader (`rr3_pc.ini`)
  - `port\third_party\` — Dynarmic, SDL2, ANGLE binaries, zlib, stb_image, etc.
  - `port\userdata\` — Writable save/cache overlay (copied from `com.ea.games.r3_row\files\doc\`)
  - `port\build\` — CMake + Ninja output (`rr3_pc.exe` + colocated runtime DLLs)

### 4.2 Guest-Host ABI & Execution Model
- **Flat 64-bit shared memory:** Guest ELFs are mapped via `VirtualAlloc` with exact segment alignments. Guest virtual addresses equal host pointers, avoiding copy marshalling across GLES, file I/O, JNI buffers, and FMOD PCM.
- **Import dispatch:** Each unresolved imported function gets an 8-byte executable stub (`SVC #<id>; RET`) in a dedicated trampoline page. Dynarmic's `UserCallbacks::CallSVC(id)` dispatches directly to the corresponding C++ host thunk.
- **Guest threading:** `pthread_create` creates a real Windows thread with its own `Dynarmic::A64::Jit` instance sharing one `Dynarmic::A64::ExclusiveMonitor` and guest memory. Each thread holds its own Bionic `errno`, `__emutls` map, and nested re-entry stack for Host->Guest callbacks (`onViewRenderJNI`, `qsort` comparators, FMOD DSP callbacks).
- **Zero network guarantee:** `connect`, `getaddrinfo`, `gethostbyname`, and Java `Http`/`HttpRequest` are gated offline (returning `ENETUNREACH` / immediate failure callback) so the binary cannot phone home.

### 4.3 Display, Dynamic Rescaling, Crispness & High Refresh Rate
- **DPI awareness & live resize:** Process declares `PerMonitorV2` DPI awareness. On window resize (`SDL_WINDOWEVENT_SIZE_CHANGED`), the host updates ANGLE's swapchain/surface to exact physical pixel dimensions, updates `Platform.getScreenWidth()/Height()`, and calls `MainActivity_onViewChangedJNI` on the render thread so the engine rebuilds its projection/viewport dynamically without blurry scaling.
- **Crisp PC rendering:**
  - `Platform` reports high-spec hardware (`getTotalMemory()`, `getCpuCount()`, `getBuildDevice()`, high DPI) so the engine loads its top preset (`AndroidXtraHigh.plist` / `AndroidUltraWide.plist`, verified in Phase 1).
  - `rr3_pc.ini` exposes: `render_scale` (default `1.0`, up to `2.0` supersampling), `anisotropy` (default `16`, injected in `glTexParameteri` when `GL_EXT_texture_filter_anisotropic` is supported by ANGLE), `msaa_samples` (`0`/`4`), and `fullscreen` / `borderless` mode (`Alt+Enter` / `F11`).
- **60 FPS & High Refresh Rate:**
  - Host render loop queries monitor refresh rate and supports `vsync` (matching 60 / 120 / 144 / 165+ Hz), `fps_cap`, or uncapped modes via `rr3_pc.ini`.
  - **Phase 1 Ghidra Gate:** Disassemble `onViewRenderJNI` and Mint3's clock/timestep loop to verify whether `libRealRacing3.so` has an internal 30/60 FPS cap, fixed vs variable physics `dt`, or `.plist` frame cap. If an engine-side cap exists and physics is delta-time safe, document and expose a clean high-refresh option; if physics ties to 60 Hz, cap simulation safely and report the exact finding.

### 4.4 Input Architecture
- **Gamepads (SDL2 GameController):** Native out-of-the-box support for Xbox (XInput), DualShock 4 (USB/BT), DualSense (USB/BT), and Switch Pro (USB/BT), using SDL's built-in GameControllerDB mappings. Hot-plug events invoke `ControllerConnectedJNI` / `ControllerDisconnectedJNI`; sticks & analog triggers drive `SetJoystickValueJNI`; face/shoulder/D-pad/Start/Select drive `SetButtonValueJNI`. Exact button/axis enum ordinals will be extracted verbatim from `classes2.dex` in Phase 1.
- **Keyboard:** Configurable via `rr3_pc.ini` (defaults: `W`/`Up` throttle, `S`/`Down` brake/reverse, `A`/`D` & `Left`/`Right` smooth analog-ramped steering mapped onto virtual controller `AXIS_LTHUMB_X` with speed-sensitive ramp/return rates, `Space` handbrake, `C` camera, `R` rear view, `Enter` select, `Esc` pause/back). Also forwards key events to `onKeyPressed`/`onKeyReleased` when appropriate.
- **Mouse-to-Touch & TV Navigation:** Left mouse click/drag maps directly to `onTouchBeginJNI`/`onTouchMoveJNI`/`onTouchEndJNI` in current viewport coordinates so all mobile-style menus work effortlessly with mouse, while `SetRunningOnAndroidTv(true)` (configurable) enables full controller/keyboard menu focus navigation.

### 4.5 Explicit Policy Calls (Resolving Design Ambiguities)
The following calls are proposed defaults; the user can override any of them before implementation starts:
1. **Offline scope (Vanilla Offline First):** Target clean offline execution using the user's existing `doc\` save (copied to `port\userdata\doc\`) with zero outbound network traffic and offline stubs for Nimble/Cloudcell/Google Play. Do **not** implement IAP/economy cheats or car unlocks. If Phase 1 discovers a dead-server boot blocker that rejects pure offline stubs, report the exact mechanism to the user before expanding scope into a local response synthesizer.
2. **Runtime patching vs. read-only inputs:** Original `RR3 14.0.1.apk` and `com.ea.games.r3_row\` remain strictly read-only. Layered VFS config overrides (in `port\userdata\`) and surgical, SHA-256-guarded in-memory runtime patches in `elf_loader` (for example, lifting an internal 30/60 FPS cap or fixing mobile-only DPI/viewport assumptions) are permitted only on our loaded working copy, preferring HLE shims first.
3. **Save format & compatibility:** Preserve RR3's native `doc\` save format untouched in `port\userdata\doc\` so saves remain clean and portable. Phase 1 will verify whether the existing `doc\` saves (`character.2.dat`, `TempSaveGame.dat`, `player_profile.2.dat`) load cleanly without missing Android `/data/data/.../shared_prefs` or `ANDROID_ID` bindings; if a device-ID mismatch occurs, report it and decide between supplying the ID or starting a fresh local profile.
4. **Performance priority (Ryzen 7 5700X + RX 580):** Target 60+ FPS at `AndroidXtraHigh` under Approach A (Dynarmic JIT + ANGLE). If profiling in Phase 4–5 shows a CPU bottleneck in 22-car races, optimize/replace hot subsystems natively (starting with `audio_fmod` / hot math paths) **before** stepping down visual quality or considering partial AOT.
5. **Verification bar for absent hardware:** Physically verify keyboard, mouse-to-touch, GPU rendering, resize crispness, and audio on this host. For any of the 4 gamepad families (Xbox/XInput, DualShock 4, DualSense, Switch Pro) or >60 Hz refresh rates not physically connected during a test run, verify end-to-end via an automated **SDL virtual gamepad + JNI injection harness** and **uncapped frame-time profiling**, and explicitly label what was physically vs. synthetically verified.
6. **`MoviePlayer` and `TextField` depth:** `MoviePlayer` uses a clean skip stub (`OnCompletionNative` callback; no video decoder dependency) and `TextField` uses a lightweight SDL `SDL_TEXTINPUT` bridge. Full video playback can be added later only if needed.

---

## 5. Phased Execution & Verification Gates

- **Phase 1 — Static Contract Extraction (JADX + Ghidra 12.2):**
  - Extract `arm64-v8a` `.so` copies into `port\work_bins\`.
  - Decompile `com.firemint.realracing.*` and `com.firemonkeys.cloudcellapi.*` from `classes2.dex` to get exact boot order, `onViewChangedJNI(IIII)` & `onViewRenderJNI(II)` arguments, `ControllerManager` button/axis IDs, `Platform`/`Font`/`GlyphVector`/`TextureInfo`/`CarLiveryBaker` contracts.
  - Decode `.depot\eds\AndroidXtraHigh.plist` and inspect `libRealRacing3.so` in Ghidra for: `ptrace`, `syscall`, `csdk.starlightsecurity`, `dlopen`/`dlsym`, graphics tier selection, and FPS/timestep handling.
  - **Gate:** Complete verified contract table documented in `PORT_NOTES.md`; no guessed symbols or offsets.
- **Phase 2 — Minimal Loader + Dynarmic Execution Proof:**
  - Set up CMake + Ninja + MSVC build in `port\`, integrate Dynarmic, map `libc++_shared.so` + `libRealRacing3.so`, implement SVC trampoline + core Bionic/pthread/emutls shims.
  - **Gate:** `rr3_pc.exe` builds cleanly, runs all 181 `.init_array` constructors and `JNI_OnLoad` (if present) / initial entry without unhandled SVC or segfault, verified by console run log.
- **Phase 3 — Graphics Bring-up (SDL2 + ANGLE) + First Frame:**
  - Integrate SDL2 + ANGLE + `gles_bridge` + `FakeJVM` + `Platform`/`Font`/`GlyphVector`/`TextureInfo` + offline stubs.
  - **Gate:** `rr3_pc.exe` opens a resizable DPI-aware window, executes `onCreateJNI` -> `onViewCreatedJNI` -> `onViewChangedJNI` -> `onViewRenderJNI`, renders real RR3 splash/menu frames (verified by captured screenshot + GL/FPS log), and survives live window resizing without crash or distortion.
- **Phase 4 — Offline Playability + FMOD Ex Audio:**
  - Integrate `libfmodex.so` + SDL2/WASAPI audio pump; resolve any missing JNI/POSIX/offline-startup behaviors until offline garage, car selection, and a live 3D race run cleanly from `port\userdata\` copy of `doc\` saves with zero network traffic.
  - **Gate:** Verified in-race screenshot, zero crashes, active FMOD audio output log, save round-trip verified in `port\userdata\`.
- **Phase 5 — Native Keyboard, Multi-Gamepad, High-FPS & Crisp Rendering Polish:**
  - Complete SDL2 GameController (Xbox/DS4/DualSense/Switch Pro) + smooth remappable keyboard + mouse-to-touch + `rr3_pc.ini` (`render_scale`, `anisotropy`, `msaa`, `vsync`/`fps_cap`, keybinds).
  - **Gate:** Verified steering/throttle/brake/menu navigation via keyboard and gamepad, verified dynamic resize crispness, verified 60+ FPS pacing (with exact measured frame times).

---

## 6. Left to the Builder
- Choice of SDL2 vs SDL3 (whichever links cleanest with MSVC 14.51 + ANGLE).
- Exact Dynarmic fetch/build wiring (CMake `FetchContent` vs local submodule + `vcpkg`/vendored headers).
- Internal implementation details of `Font`/`GlyphVector` (DirectWrite vs FreeType) and `CarLiveryBaker` ETC1 encoder, provided the rendered text and car liveries match Android output.
- Default keybindings and keyboard steering ramp curve constants (exposed in `rr3_pc.ini` for easy tuning).
