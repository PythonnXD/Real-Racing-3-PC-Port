# Real Racing 3 Native Windows PC Port — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a standalone native Windows x64 executable (`d:\Real Racing 3\port\build\rr3_pc.exe`) that runs Real Racing 3 v14.0.1 (`arm64-v8a` `libRealRacing3.so` + `libc++_shared.so` + `libfmodex.so`) fully offline at 60+ FPS with dynamic crisp window rescaling, FMOD audio, native keyboard controls, and native Xbox/DualShock 4/DualSense/Switch Pro gamepad support.

**Architecture:** A C++20 Windows x64 host maps the unmodified `arm64-v8a` ELF working copies into flat 64-bit memory, executes guest ARM64 code via Dynarmic JIT with per-thread `TPIDR_EL0` TLS/stack-canary blocks and shared `ExclusiveMonitor`, and dispatches all imported C/POSIX/pthread/GLES/EGL/JNI/OpenSL calls through 8-byte `SVC #<id>; RET` trampolines directly to native Windows/ANGLE/SDL2/DirectWrite implementations.

**Tech Stack:** C++20, MSVC 14.51 (`D:\Visual Studio`), CMake, Ninja, Dynarmic (AArch64 JIT), SDL2 (windowing, DPI, WASAPI audio, keyboard, GameController HID/XInput), Google ANGLE (`libEGL.dll` + `libGLESv2.dll`), zlib, DirectWrite, `stb_image`, `rg_etc1`.

**Spec:** [DESIGN.md](file:///d:/Real%20Racing%203/DESIGN.md)

## Global Constraints
- Work strictly inside `d:\Real Racing 3\`; never modify original `d:\Real Racing 3\RR3 14.0.1.apk` or `d:\Real Racing 3\com.ea.games.r3_row\`.
- Working copies and build outputs live under `d:\Real Racing 3\port\`; writable save/cache files live in `d:\Real Racing 3\port\userdata\` (seeded once from `com.ea.games.r3_row\files\doc\`).
- Standalone native Windows x64 program — zero Android emulator/VM.
- 100% offline: `NetworkStatusMonitor.getNetworkConnectivity` returns `1` (`CONNECTION_TYPE_NONE`), `Platform.isNetworkAvailable` returns `false`, and POSIX `connect`/`getaddrinfo`/`gethostbyname` plus Java `Http`/`HttpRequest` fail immediately without opening outbound sockets.
- Pin any optional in-memory runtime patches (e.g., menu 24 FPS `usleep` unlock at `0x12af09c` / `0x12af100`) to `libRealRacing3.so` SHA-256 (`arm64-v8a` extracted from APK SHA-256 `724103c66b32c1c782a52eae686d5ae4ee85581b860be7015f6b45d84829c9c5`).
- Report only verified build/run/visual results; clearly label physical vs. synthetic hardware tests.
- Update `d:\Real Racing 3\PORT_NOTES.md` after every phase.

## Review Focus
1. **AArch64 `TPIDR_EL0` stack canary & Bionic ABI struct layout mismatch:** `libRealRacing3.so` reads `[TPIDR_EL0 + 0x28]` on function entry (`ControllerConnectedJNI`, `onCreateJNI`, `onViewRenderJNI`); a missing TLS block crashes immediately, and a wrong `struct stat`/`struct dirent`/`errno` layout silently corrupts `.depot` file loading. Pinned by unit tests in Task 2 and Task 3.
2. **Re-entrant Guest -> Host -> Guest callbacks & multi-threaded Dynarmic safety:** `qsort`, `pthread_create`, `dl_iterate_phdr`, FMOD OpenSL callbacks, and JNI callbacks re-enter ARM64 code while already inside a host SVC handler. Pinned by re-entrancy and multi-thread stress tests in Task 2 and Task 3.
3. **Variadic AAPCS64 formatting (`snprintf`, `vsnprintf`, `__android_log_print`, `sscanf`) & float args:** AArch64 splits integer (`X0–X7`), float (`Q0–Q7`), and stack arguments differently from Win64 x64; naive varargs forwarding crashes or prints garbage. Pinned by AAPCS64 printf/float formatting test in Task 3.
4. **Live window resize + portrait/degenerate window sizes:** `onViewChangedJNI` ignores `width <= height` (`"orientation is portrait, ignoring..."`) and `onViewRenderJNI` defers viewport rebuilds by 3 frames; resizing to portrait or minimizing (`0x0`) must not crash ANGLE or corrupt touch coordinates. Pinned by resize/minimize test in Task 5 and Task 8.
5. **Read-only original data isolation + save round-trip integrity:** Writes targeting `.depot`, `doc/`, or `apk/` paths must redirect strictly into `port\userdata\` without mutating `com.ea.games.r3_row\` and without corrupting binary `*.2.dat` saves. Pinned by VFS isolation test in Task 3 and save check in Task 7.

---

## File Structure

- `port/CMakeLists.txt` — Root CMake build (MSVC C++20, Dynarmic, SDL2, ANGLE, zlib, DirectWrite, test targets).
- `port/src/config.h`, `port/src/config.cpp` — INI config loader (`rr3_pc.ini`: resolution, fullscreen, `render_scale`, `anisotropy`, `msaa_samples`, `vsync`, `fps_cap`, `unlock_menu_fps`, `android_tv_mode`, keybinds).
- `port/src/elf_loader.h`, `port/src/elf_loader.cpp` — AArch64 ELF64 loader (`PT_LOAD`, `R_AARCH64_RELATIVE`/`ABS64`/`GLOB_DAT`/`JUMP_SLOT`), multi-library symbol table, `dl_iterate_phdr` registry, 8-byte `SVC #<id>; RET` trampoline allocator, SHA-256 verification.
- `port/src/cpu_dynarmic.h`, `port/src/cpu_dynarmic.cpp` — Per-thread Dynarmic `A64::Jit` + `TPIDR_EL0` block (`+0x28` stack canary) + shared `ExclusiveMonitor`, re-entrant `CallGuest(addr, args...)`, AAPCS64 integer/float/stack helpers, SVC dispatch table.
- `port/src/vfs.h`, `port/src/vfs.cpp` — Layered copy-on-write VFS mapping Android storage paths to read-only `com.ea.games.r3_row\files\` + writable `port\userdata\`.
- `port/src/bionic_libc.h`, `port/src/bionic_libc.cpp` — Bionic libc/libm/zlib/pthread/sem/`__emutls_get_address`/`setjmp`/`longjmp`/`dlopen`/`dlsym`/`syscall`/`ptrace`/`getauxval`/variadic printf/offline-gated sockets + AArch64 `stat`/`dirent`/`errno`/`__sF`.
- `port/src/gles_bridge.h`, `port/src/gles_bridge.cpp` — Dynamic loader for ANGLE `libEGL.dll` + `libGLESv2.dll`, all 142 `gl*` + 3 `egl*` + `eglGetProcAddress` thunks, forced anisotropy & render-scale viewport helpers.
- `port/src/jni_env.h`, `port/src/jni_env.cpp` — Full `JavaVM` + `JNIEnv` 232-slot guest-callable vtables (SVC-backed), global/local ref table, string/array/DirectByteBuffer storage, class/method/field dispatch registry.
- `port/src/java_firemint.h`, `port/src/java_firemint.cpp` — Native implementations of `MainActivity`, `Platform` (`loadTextureFromMemory` via `stb_image`), `Font` + `GlyphVector` (DirectWrite -> GL texture), `TextureInfo`, `CarLiveryBaker` (ETC1 via `rg_etc1`), `ControllerManager`, `AudioStreamManager`, `Input`, `TextField` (SDL text input), `MoviePlayer` (skip callback).
- `port/src/java_offline_stubs.h`, `port/src/java_offline_stubs.cpp` — Offline stubs for `NetworkStatusMonitor` (`CONNECTION_TYPE_NONE = 1`), `Http`/`HttpRequest`, `CC_Component`, `GetInfo`, `GooglePlayWorker`, `FacebookWorker`, `AndroidAccountManager`, `AndroidAssetManagerService`, `Nimble*`, `GoogleNativeAdManager`, `csdk.*`, `Ar*`, `Anzu_*`, `libfuelmetrics`.
- `port/src/audio_opensl.h`, `port/src/audio_opensl.cpp` — Lightweight HLE `libOpenSLES.so` (`slCreateEngine` + `SLAndroidSimpleBufferQueueItf`) bridging guest `libfmodex.so` PCM buffers directly to SDL2/WASAPI audio.
- `port/src/input_pc.h`, `port/src/input_pc.cpp` — SDL2 GameController (Xbox/XInput, DualShock 4, DualSense, Switch Pro) + smooth analog-ramped keyboard + mouse-to-touch + synthetic virtual-controller test driver.
- `port/src/main.cpp` — PerMonitorV2 DPI setup, resizable SDL2 window + ANGLE EGL context, exact `MainActivity`/`GLRenderer` boot & frame loop (`onViewRenderJNI(2, 0)`), dynamic resize handling, FPS telemetry, screenshot dump helper.
- `port/tests/` — Standalone CTest executables (`test_elf_cpu.cpp`, `test_bionic_vfs.cpp`, `test_input_resize.cpp`).

---

### Task 1: Workspace Build Scaffolding & Third-Party Dependencies (Phase 2a)

**Files:**
- Create: `port/CMakeLists.txt`
- Create: `port/scripts/fetch_deps.ps1`
- Create: `port/src/config.h`, `port/src/config.cpp`

**Interfaces:**
- Produces: `struct PcConfig` and `PcConfig LoadConfig(const std::filesystem::path& path)` in `port/src/config.h`; CMake targets `dynarmic`, `SDL2::SDL2`, `zlibstatic`, and colocated `libEGL.dll` / `libGLESv2.dll` / `SDL2.dll` in `port/build/`.

- [ ] **Step 1: Fetch pinned third-party dependencies inside `port/third_party/`**
  - Clone/fetch `dynarmic` (C++20 AArch64 JIT), `SDL2` (prebuilt MSVC x64 devel zip), `zlib`, `stb_image.h`, `rg_etc1.{h,cpp}`, and prebuilt Windows x64 ANGLE (`libEGL.dll`, `libGLESv2.dll`, headers). Document exact versions/sources in `PORT_NOTES.md`.
- [ ] **Step 2: Write `port/CMakeLists.txt` and `port/src/config.{h,cpp}`**
  - Configure C++20 (`/std:c++20 /EHsc /bigobj`), link `dynarmic`, `SDL2`, `zlib`, `dwrite`, `d2d1`, `windowscodecs`, `ws2_32`, `winmm`, `bcrypt`.
  - Implement `LoadConfig` parsing `rr3_pc.ini` with defaults (`width=1920`, `height=1080`, `fullscreen=false`, `render_scale=1.0`, `anisotropy=16`, `msaa_samples=0`, `vsync=true`, `fps_cap=0`, `unlock_menu_fps=true`, `android_tv_mode=false`).
- [ ] **Step 3: Build smoke target using MSVC 14.51 + CMake + Ninja**
  - Run: `cmd /c ""D:\Visual Studio\VC\Auxiliary\Build\vcvars64.bat" && "D:\Visual Studio\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S "d:\Real Racing 3\port" -B "d:\Real Racing 3\port\build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && "D:\Visual Studio\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build "d:\Real Racing 3\port\build" --target test_smoke"`
  - Expected: Exit code 0, `dynarmic` + `SDL2` + `config` link cleanly.

---

### Task 2: AArch64 ELF64 Loader & Dynarmic Execution Core (Phase 2b)

**Files:**
- Create: `port/src/elf_loader.h`, `port/src/elf_loader.cpp`
- Create: `port/src/cpu_dynarmic.h`, `port/src/cpu_dynarmic.cpp`
- Test: `port/tests/test_elf_cpu.cpp`

**Interfaces:**
- Produces:
  - `class ElfLoader { LoadedModule* Load(const std::filesystem::path& path); uint64_t FindExport(const std::string& name); void RegisterHostSymbol(const std::string& name, HostThunkFn fn); void ApplyRelocations(); void RunInitArrays(CpuRuntime& cpu); const std::vector<LoadedModule>& Modules() const; };`
  - `class CpuRuntime { void InitThread(); uint64_t CallGuest(uint64_t addr, std::initializer_list<uint64_t> args); double CallGuestF64(...); void SetX(int i, uint64_t v); uint64_t GetX(int i); ... };`

- [ ] **Step 1: Write failing unit tests in `port/tests/test_elf_cpu.cpp`**
  - Test 1 (`test_tpidr_canary_and_call`): execute assembled AArch64 machine code reading `mrs x0, tpidr_el0; ldr x0, [x0, #0x28]; ret` and assert non-zero canary matches thread TLS block.
  - Test 2 (`test_svc_thunk_and_reentrancy`): guest calls host SVC thunk, host thunk re-entrantly calls another guest AArch64 function, verify nested return value and restored registers `X19–X30`/`SP`.
  - Test 3 (`test_elf_load_real_libs`): load `libc++_shared.so`, `libfmodex.so`, `libRealRacing3.so` from `port\work_bins\lib\arm64-v8a\`; assert `FindExport("Java_com_firemint_realracing_MainActivity_onCreateJNI") != 0`, `FindExport("FMOD_System_Create") != 0`, and zero unresolved `R_AARCH64_*` relocation type errors.
- [ ] **Step 2: Run `test_elf_cpu` to verify failure before implementation**
- [ ] **Step 3: Implement `elf_loader.{h,cpp}` and `cpu_dynarmic.{h,cpp}`**
  - Map `PT_LOAD` via `VirtualAlloc`, resolve `.rela.dyn` (`1027` RELATIVE, `257` ABS64, `1025` GLOB_DAT) and `.rela.plt` (`1026` JUMP_SLOT) across loaded modules first, then host SVC trampoline table (`SVC #id; RET`), logging any unresolved symbol with a trap stub that prints the missing symbol name and caller PC.
  - Configure Dynarmic `A64::UserConfig` with flat direct memory reads/writes, shared `ExclusiveMonitor`, per-thread 64 KB aligned TLS block in `tpidr_el0` with random canary at `+0x28`, and a sentinel `SVC #0xFFFF` return trap for `CallGuest` with saved register frame stack for arbitrary nesting.
- [ ] **Step 4: Build and run `test_elf_cpu.exe`**
  - Expected: All 3 tests PASS.

---

### Task 3: Bionic libc/POSIX/pthread Layer, Layered VFS & `.init_array` Boot Proof (Phase 2c)

**Files:**
- Create: `port/src/vfs.h`, `port/src/vfs.cpp`
- Create: `port/src/bionic_libc.h`, `port/src/bionic_libc.cpp`
- Test: `port/tests/test_bionic_vfs.cpp`

**Interfaces:**
- Consumes: `ElfLoader`, `CpuRuntime`.
- Produces:
  - `class LayeredVfs { void Initialize(const path& readonly_files_dir, const path& userdata_dir); path ResolveRead(std::string_view android_path); path ResolveWrite(std::string_view android_path); };`
  - `void RegisterBionicSymbols(ElfLoader& loader, CpuRuntime& cpu, LayeredVfs& vfs);`

- [ ] **Step 1: Write failing unit tests in `port/tests/test_bionic_vfs.cpp`**
  - Test 1 (`test_vfs_read_only_isolation`): verify reads resolve `.depot/Shaders/2dimage.vsh` from `com.ea.games.r3_row\files`, while writes to `doc/test_write.tmp` redirect strictly into `port\userdata\doc\test_write.tmp` and never touch `com.ea.games.r3_row\`.
  - Test 2 (`test_aapcs64_vsnprintf_and_stat`): verify AAPCS64 integer + double variadic formatting (`"%s %d %.2f"`) and exact 128-byte AArch64 `struct stat` size/offsets (`st_size` at offset `0x30`, `st_mode` at `0x10`).
  - Test 3 (`test_pthread_and_emutls`): spawn two guest threads via `pthread_create` thunk, verify mutex mutual exclusion and distinct `__emutls_get_address` slots per thread.
  - Test 4 (`test_rr3_init_arrays`): load `libc++_shared.so` + `libfmodex.so` + `libRealRacing3.so` (with stubbed GL/AR/Anzu/Fuel/Nimble symbols) and execute all `.init_array` constructors cleanly without crashing!
- [ ] **Step 2: Run `test_bionic_vfs` to confirm failure**
- [ ] **Step 3: Implement `vfs.{h,cpp}` and `bionic_libc.{h,cpp}`**
  - Seed `port\userdata\doc\` once from `com.ea.games.r3_row\files\doc\` if absent.
  - Implement all ~229 libc/libm + 40 pthread/sem + zlib + `__emutls_get_address` + `setjmp`/`longjmp` + `dlopen`/`dlsym`/`dl_iterate_phdr` + `getauxval` + `ptrace` (return `0` so `memory_profiler.cpp` never raises `SIGINT`) + `syscall` (`0x11c` `SYS_mlock2` -> `ENOSYS`; `0x116` `SYS_getrandom` -> `BCryptGenRandom`) + offline-blocked sockets (`connect`/`getaddrinfo`/`gethostbyname` return `-1` / `EAI_NONAME` with `ENETUNREACH`).
  - Handle AAPCS64 `va_list` and direct variadic calls cleanly (`snprintf`, `__vsnprintf_chk`, `sprintf`, `fprintf`, `__android_log_print`, `sscanf`).
- [ ] **Step 4: Build and run `test_bionic_vfs.exe` (Phase 2 Gate)**
  - Expected: All 4 tests PASS; all 181 `libRealRacing3.so` `.init_array` constructors complete with exit code 0. Update `PORT_NOTES.md`.

---

### Task 4: ANGLE GLES2 Bridge & Fake JNI Infrastructure (Phase 3a)

**Files:**
- Create: `port/src/gles_bridge.h`, `port/src/gles_bridge.cpp`
- Create: `port/src/jni_env.h`, `port/src/jni_env.cpp`

**Interfaces:**
- Consumes: `ElfLoader`, `CpuRuntime`, `PcConfig`.
- Produces:
  - `class GlesBridge { bool Init(HWND hwnd, int width, int height, const PcConfig& cfg); void Resize(int w, int h); void SwapBuffers(); void RegisterSymbols(ElfLoader& loader); uint32_t UploadRgbaTexture(const void* pixels, int w, int h, int texW, int texH, bool flipY); };`
  - `class FakeJvm { uint64_t GetJavaVmGuestPtr(); uint64_t GetJniEnvGuestPtr(); uint64_t NewStringUtf(const std::string& s); std::string GetStringUtf(uint64_t jstr); void RegisterClass(const std::string& name, JniClassDef def); };`

- [ ] **Step 1: Implement `gles_bridge.{h,cpp}`**
  - Load `libEGL.dll` and `libGLESv2.dll` (D3D11 backend), create GLES 2.0 context (`RGB888`, `D24`, `S8`, optional `EGL_SAMPLES`), bind all 142 `gl*` + 3 `egl*` symbols + dynamic `eglGetProcAddress` (wrapping returned extension pointers in guest SVC stubs!).
  - Inject configurable `GL_TEXTURE_MAX_ANISOTROPY_EXT` when `cfg.anisotropy > 1` and `EXT_texture_filter_anisotropic` is present.
- [ ] **Step 2: Implement `jni_env.{h,cpp}`**
  - Build exact 232-slot `JNINativeInterface` and 8-slot `JNIInvokeInterface` structs in guest-readable memory populated with SVC stubs so ARM64 code calling `(*env)->FindClass(...)` or `(*vm)->GetEnv(...)` dispatches cleanly into C++.
  - Implement string, primitive array, object array, DirectByteBuffer (`GetDirectBufferAddress`), field get/set, and method call dispatch (`Call*Method`, `Call*MethodV`, `Call*MethodA`, `CallStatic*Method*`).
  - Note: log every unimplemented class/method/field lookup with exact name + signature + caller PC so zero silent failures hide during bring-up!
- [ ] **Step 3: Build & run unit test verifying ANGLE context creation + `FakeJvm` `FindClass`/`CallStaticIntMethod`/`NewStringUTF` round-trip from guest ARM64**
  - Expected: PASS, `glGetString(GL_RENDERER)` prints ANGLE D3D11 AMD Radeon RX 580.

---

### Task 5: Firemint Java Shims, Offline Stubs & First Rendered Frame (Phase 3b)

**Files:**
- Create: `port/src/java_firemint.h`, `port/src/java_firemint.cpp`
- Create: `port/src/java_offline_stubs.h`, `port/src/java_offline_stubs.cpp`
- Create: `port/src/main.cpp`

**Interfaces:**
- Consumes: `ElfLoader`, `CpuRuntime`, `LayeredVfs`, `GlesBridge`, `FakeJvm`, `PcConfig`.
- Produces: runnable `port/build/rr3_pc.exe` that boots `MainActivity` + `GLRenderer` to real rendered RR3 frames.

- [ ] **Step 1: Implement `java_firemint.{h,cpp}`**
  - `MainActivity`, `AppProxy`: `finishActivity`, `getApiLevel() -> 31`, `setSkipOnDestroyJNI`, `getScreenOrientation() -> 2`, `getScreenRotation() -> 0`, `getIsAndroidTv()`, `getIsAmazon() -> false`, `HasDisplayCutout() -> false`, `GetCutoutLeft/Right() -> 0`, `showMessage` (log + auto/interactive callback), `showKeyboardInput`/`TextField` (SDL text input).
  - `Platform`: `getExternalStorageDir() -> "/storage/rr3"`, `getExternalStorageState() -> 2`, `getPackageName() -> "com.ea.games.r3_row"`, `getAppVersion() -> "14.0.1"`, `getScreenWidth/Height` (tracks live scaled viewport), `getScreenDPI() -> 320`, `getCpuCount() -> 8`, `getTotalMemory() -> 8192`, `getModelID/BuildDevice/Manufacturer` (tuned for `AndroidXtraHigh` / `AndroidUltraWide`), `isNetworkAvailable(int) -> false`, `loadTextureFromMemory(byte[], int)` via `stb_image` -> POT GL texture -> `TextureInfo`.
  - `Font` & `GlyphVector`: DirectWrite font loader from VFS `.ttf` files implementing `init`, `loadGlyph`, `loadBitmap`, `loadBitmapDynamic`, `GlyphVector.init`, `initWithParagraph`, `createTexture`, `renderToTexture`.
  - `CarLiveryBaker`: `compressTextureToETC1` via `rg_etc1` returning DirectByteBuffer.
  - `AudioStreamManager` (`staticIsUserMusicPlaying() -> false`), `Input` (accel/gyro unavailable), `MoviePlayer` (`play` -> immediately invokes `OnCompletionNative`).
- [ ] **Step 2: Implement `java_offline_stubs.{h,cpp}`**
  - `NetworkStatusMonitor` (`getNetworkConnectivity -> 1`), `CC_Component`, `GetInfo`, `Http` / `cloudcellapi.HttpRequest` (invoke `errorCallback` cleanly, zero sockets), `GooglePlayWorker`, `FacebookWorker`, `AndroidAccountManager`, `AndroidAssetManagerService`, `Nimble*` + `libNimble.so` (`EA::Nimble::getEnv` / `findClass`), `GoogleNativeAdManager`, `csdk.*`, `Ar*`, `Anzu_*`, `libfuelmetrics`.
- [ ] **Step 3: Implement `main.cpp` and iteratively resolve bring-up logs until first real frame renders (Phase 3 Gate)**
  - Set `PerMonitorV2` DPI awareness, open resizable SDL2 window + ANGLE EGL context, drive exact Android lifecycle: `SetRunningOnAndroidTv` -> `onCreateJNI` -> `onStartJNI` -> `onResumeJNI` -> `onWindowFocusChangedJNI(true)` -> `onViewCreatedJNI` -> `onViewChangedJNI(w, h, 2, 0)` -> loop `onViewRenderJNI(2, 0)` + `eglSwapBuffers`.
  - Support live resize (`SDL_WINDOWEVENT_SIZE_CHANGED`, clamped landscape `w > h` when needed, ignore `0x0` minimized) and `--dump-frame N path.png` for automated visual verification.
  - **Verification:** Run `rr3_pc.exe`, inspect logs for zero unhandled SVCs/JNI crashes, capture & visually inspect rendered splash/menu screenshot, and test live window resize. Update `PORT_NOTES.md`.

---

### Task 6: FMOD Ex Audio via HLE `libOpenSLES.so` & Offline Gameplay Verification (Phase 4)

**Files:**
- Create: `port/src/audio_opensl.h`, `port/src/audio_opensl.cpp`
- Modify: `port/src/bionic_libc.cpp` (register `dlopen("libOpenSLES.so")` -> HLE OpenSL module)
- Modify: `port/src/java_offline_stubs.cpp` / `port/src/java_firemint.cpp` (any missing JNI/offline methods encountered transitioning from menu -> garage -> live 3D race)

**Interfaces:**
- Consumes: `ElfLoader`, `CpuRuntime`.
- Produces: `class OpenSlBridge { bool Init(); void Shutdown(); uint64_t ResolveSymbol(const std::string& name); uint64_t FramesPlayed() const; };`

- [ ] **Step 1: Implement `audio_opensl.{h,cpp}`**
  - Provide `slCreateEngine` and interface IIDs (`SL_IID_ENGINE`, `SL_IID_PLAY`, `SL_IID_ANDROIDSIMPLEBUFFERQUEUE`, `SL_IID_ANDROIDCONFIGURATION`) backed by guest-callable SVC vtables.
  - Bridge `SLAndroidSimpleBufferQueueItf::Enqueue` PCM buffers into an SDL2 WASAPI audio device (`SDL_QueueAudio` or lock-free ring buffer callback triggering the registered OpenSL buffer-queue completion callback on the FMOD mixer thread).
- [ ] **Step 2: Boot into offline garage and start a live 3D race (Phase 4 Gate)**
  - Run `rr3_pc.exe`, navigate via mouse-to-touch from title -> offline garage -> event -> 3D track.
  - Resolve any runtime missing libc/JNI/offline calls observed in logs.
  - **Verification:** Capture in-race screenshot, confirm `OpenSlBridge::FramesPlayed() > 0` in runtime log, confirm zero outbound sockets, confirm `port\userdata\doc\` save writes without mutating `com.ea.games.r3_row\`. Update `PORT_NOTES.md`.

---

### Task 7: Native Keyboard, Multi-Gamepad, High-FPS & Crisp Upscaling Polish (Phase 5)

**Files:**
- Create: `port/src/input_pc.h`, `port/src/input_pc.cpp`
- Modify: `port/src/main.cpp`, `port/src/gles_bridge.cpp`, `port/src/config.cpp`
- Test: `port/tests/test_input_resize.cpp`

**Interfaces:**
- Consumes: `CpuRuntime`, `ElfLoader`, `FakeJvm`, `PcConfig`.
- Produces: `class PcInput { void Init(bool android_tv_mode); void ProcessEvent(const SDL_Event& e); void UpdateFrame(float dt_sec); void DispatchToGuest(CpuRuntime& cpu, ElfLoader& loader, FakeJvm& jvm); };`

- [ ] **Step 1: Implement `input_pc.{h,cpp}` using verified `ControllerManager.java` ordinals**
  - **Gamepads (SDL2 GameController):** Auto-detect hot-plug for Xbox (XInput), DualShock 4, DualSense, Switch Pro; emit `ControllerConnectedJNI(name, guid, devId, 0x3FF)` / `ControllerDisconnectedJNI(devId)`. Map sticks -> `AXIS_LTHUMB_X/Y` (`0,1`), `AXIS_RTHUMB_X/Y` (`2,3`), triggers -> `AXIS_LTRIGGER/RTRIGGER` (`4,5`), buttons -> exact `ControllerButtons` ordinals (`BTN_A=0..BTN_START=13`).
  - **Keyboard:** Virtual controller device (`devId = 1000`, `flags = 0x3FF`) + `MainActivity_onKeyPressed`/`Released`: remappable `W`/`Up` throttle (`AXIS_RTRIGGER`), `S`/`Down` brake (`AXIS_LTRIGGER`), `A`/`D` & `Left`/`Right` smooth analog-ramped steering (`AXIS_LTHUMB_X` with configurable ramp/return rate), `Space` handbrake, `C` camera, `R` rear view, `Enter` (`BTN_A`), `Esc` (`BTN_B`/Back keycode `4`), `F11`/`Alt+Enter` fullscreen toggle.
  - **Mouse-to-touch:** Map left click/drag to `onTouchBeginJNI`/`onTouchMoveJNI`/`onTouchEndJNI` scaled cleanly to current render resolution.
- [ ] **Step 2: Wire crisp upscaling, anisotropy/MSAA, and high-refresh pacing (>60 FPS)**
  - Verify `AndroidXtraHigh` / `AndroidUltraWide` preset selection in logs.
  - Expose `unlock_menu_fps=true` (skips the verified `0x12af100` 41 ms menu-only `usleep` in `onViewRenderJNI`) and `vsync` / `fps_cap` / `render_scale` / `anisotropy` in `rr3_pc.ini`.
  - Verify dynamic window resize cleanly calls `onViewChangedJNI(scaled_w, scaled_h, 2, 0)` and stays sharp at 1080p/1440p/custom aspect ratios.
- [ ] **Step 3: Write & run automated verification (`test_input_resize.exe`) + live end-to-end run (Phase 5 Gate)**
  - Run automated tests injecting SDL virtual gamepads (Xbox/DS4/DualSense/Switch Pro profiles), keyboard ramp curves, and resize/minimize sequences, plus live in-race benchmark logging frame times (1% low, avg FPS, menu FPS, race FPS) and visual screenshots.
  - Update `PORT_NOTES.md` with exact build/run commands, measured FPS, and explicit physical vs. synthetic verification matrix.
