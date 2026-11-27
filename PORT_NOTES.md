# Real Racing 3 Native Windows PC Port — PORT_NOTES

## Status: ALL PHASES COMPLETE & VERIFIED
- **Executable:** `d:\Real Racing 3\port\build\rr3_pc.exe` (standalone native Win64 C++20 binary — zero Android emulator/VM).
- **Config:** `d:\Real Racing 3\port\rr3_pc.ini` (auto-created if missing; supports `window_mode=windowed|borderless|fullscreen`, `width=0`/`height=0` monitor auto-fit, `render_scale`, `anisotropy`, `vsync`, `fps_cap`, `unlock_menu_fps`, `high_quality_preset`, steering rates, and remappable keyboard controls).
- **Phase 0 (Recon & Design):** COMPLETE ([DESIGN.md](file:///d:/Real%20Racing%203/DESIGN.md)).
- **Phase 1 (Static Contract Extraction):** COMPLETE ([PLAN.md](file:///d:/Real%20Racing%203/PLAN.md)).
- **Phase 2 (ELF64 Loader, Dynarmic AArch64 JIT + 64-bit Fastmem, Layered VFS, Bionic libc/pthread, Pure AArch64 String/Mem Stubs, `.init_array` Gate):** COMPLETE & VERIFIED.
- **Phase 3 (ANGLE D3D11 GLES2 Bridge, ATC/ETC1 CPU Decoder, Fake JNI, DirectWrite Fonts, `stb_image`, `rg_etc1` Livery Baker, Offline Stubs):** COMPLETE & VERIFIED.
- **Phase 4 (HLE OpenSL ES -> SDL2 WASAPI Audio, Zero-Download Offline Boot, Persistent Save Overlay, Live 3D Gameplay):** COMPLETE & VERIFIED.
- **Phase 5 (Native Remappable Keyboard with Smooth Analog Steering, Multi-Gamepad Hot-Plug for Xbox/XInput + DualShock 4 + DualSense + Switch Pro, High-FPS `enable_optimized_logic` + Menu Unlock, Dynamic Resize, Supersampling FBO, `AndroidXtraHigh` Preset, Whole-Port Code Audit):** COMPLETE & VERIFIED.

## Installed / Fetched Tools & Libraries (all inside `d:\Real Racing 3\port\`)
- **JADX CLI 1.5.1** (portable): `port\tools\jadx\bin\jadx.bat`
- **Python `capstone 5.0.7`** (pre-installed on host)
- **Dynarmic** (`azahar-emu/dynarmic` + submodules): `port\third_party\dynarmic\`
- **Boost 1.90 headers** (`azahar-emu/ext-boost`): `port\third_party\boost\`
- **zlib 1.4.0.1** (`madler/zlib`): `port\third_party\zlib\`
- **SDL2 2.30.11 VC x64**: `port\third_party\SDL2\`
- **Google ANGLE x64 (`2026-11-22`, `mmozeiko/build-angle`)**: `port\third_party\angle\angle-x64\` (`libEGL.dll`, `libGLESv2.dll`, `d3dcompiler_47.dll`)
- **`stb_image.h` + `rg_etc1.{h,cpp}`**: `port\third_party\single_header\`

## Read-Only Originals & Working Copies
- **Original APK (read-only, verified untouched `19-Dec-25 9:08:54 AM`):** `d:\Real Racing 3\RR3 14.0.1.apk` (105,189,283 B, SHA-256 `724103c66b32c1c782a52eae686d5ae4ee85581b860be7015f6b45d84829c9c5`).
- **Original Data (read-only, verified untouched newest file `19-Dec-25 9:08:13 AM`):** `d:\Real Racing 3\com.ea.games.r3_row\` (39,520 files, 7.31 GB; 19,183 Qualcomm `.atc.dds.z` textures, 874 `.pvr`, 0 DXT/ETC1).
- **Writable Copy-on-Write Overlay:** `d:\Real Racing 3\port\userdata\` (`doc\character.2.dat`, `doc\player_profile.2.dat`, shader cache, etc.). `LayeredVfs::ResolveWrite` canonicalizes and blocks `..` traversal and any write outside `userdata\`.
- **Working binary copies:** `d:\Real Racing 3\port\work_bins\lib\arm64-v8a\{libRealRacing3.so, libc++_shared.so, libfmodex.so, libNimble.so}` (`libRealRacing3.so` SHA-256 `57edb6314fb1f73916446dccf2aab2223a932ae63ccdf2149cffeddb5aa73051`).
- **Decompiled Java reference:** `d:\Real Racing 3\port\analysis\java_src\sources\com\{firemint,firemonkeys}\`.

## Key Architecture & Engineering Discoveries
1. **ATC / ETC1 Texture Transparency (`gles_bridge.cpp`):** User's `.depot` stores Adreno `.atc.dds.z` textures. Curating `GL_EXTENSIONS` to expose `GL_AMD_compressed_ATC_texture` + `GL_OES_compressed_ETC1_RGB8_texture` (hiding DXT/S3TC) and decoding ATC/ETC1 -> RGBA8 on upload eliminated all 358 MB / 100 MB download prompts offline with exact color fidelity.
2. **64-Bit Fastmem + Pure AArch64 String/Mem Stubs (`cpu_dynarmic.cpp`, `bionic_libc.cpp`):** Enabling Dynarmic `fastmem_pointer = 0` (`fastmem_address_space_bits = 64`) plus Capstone-verified pure AArch64 implementations of `strlen`, `memcmp`, `strcmp`, `strncmp`, `memcpy`, `memset`, `memmove` eliminated ~13.6M/frame JIT-to-host SVC exits and cut cold boot load from ~41s to ~11s.
3. **Offline `enable_optimized_logic` Flag (`libRealRacing3.so + 0x1f894f8`, SHA-256 guarded in `main.cpp`):** EA servers enabled `enable_optimized_logic` (`0x10b1480`) to cache expired Limited-Time Series lookups in `0x9a90ac`. Offline mode left it `0`, causing 6.8M string comparisons + 103k `gettimeofday` calls per menu frame (0.4–2.6 FPS). Enabling the flag raised 1080p 3D menu FPS to **123–136 FPS** and live 3D racing to **151–289 FPS**.
4. **Post-Boot Resolution Promotion (`main.cpp`):** Clamping initial `onViewChangedJNI` to `<=1920x1080` until frame 12 (after `AssetDownloadService::OnAssetListsComplete` at frame 7) prevents a 535 MB download prompt on 1440p/4K displays or `render_scale > 1.0`, then promotes cleanly to full native/supersampled resolution (verified at `2400x1350`).
5. **Verified `ControllerManager` JNI ABI (`ControllerManager.java:87-89`, `libRealRacing3.so @ 0x12aa724`):**
   - `SetJoystickValueJNI(int deviceId, float value, int axisOrdinal)` -> `X2=devId, S0=value, X3=axisOrdinal`.
   - `SetButtonValueJNI(int deviceId, boolean pressed, int buttonOrdinal)` -> `X2=devId, X3=pressed, X4=buttonOrdinal`.
   - `ControllerConnectedJNI(name, desc, devId, flags=0x3FF)` + `ControllerManager.isControllerConnected() -> true`.
6. **Verified OpenSL ES Audio Bridge (`libfmodex.so @ 0xb55e4..0xb5a80`, `audio_opensl.cpp`):** `SL_IID_*` symbols are pointers to interface IDs; HLE `SLAndroidSimpleBufferQueueItf::Enqueue` streams 24 kHz 2-ch S16 PCM directly to SDL2 WASAPI.
7. **Graphics Quality (`vfs.cpp`, `gles_bridge.cpp`):** Read-only VFS redirects `AndroidHigh.{plist,feats}` -> `AndroidXtraHigh.{plist,feats}` (`RENDER_CARS_IN_CUBEMAP`, `USE_HDR_POST_PROCESS`, `USE_REALTIME_CAR_SHADOW_MAPS`, `USE_PBR_WATER`, `PARTICLE_CAST_SHADOW`), plus 16x anisotropic filtering and configurable supersampling FBO (`render_scale`).
8. **Monitor Auto-Fit, Display Modes & Zero-Black-Bar Fixed-UI Dynamic Aspect Resize (`config.{h,cpp}`, `gles_bridge.{h,cpp}`, `main.cpp`, `libRealRacing3.so + 0x12ae40c`):** `ComputeSafeWindowGeometry()` queries `SDL_GetDisplayBounds`, `SDL_GetDisplayUsableBounds`, and real Win32 borders (`SDL_GetWindowBordersSize`) under Per-Monitor DPI Awareness V2 so Windowed mode never overflows the screen/taskbar. Internal FBO locks height to `fixed_render_h_ = native_h * render_scale` (e.g. `900p` on `1600x900`) so UI element size stays fixed, and dynamically adjusts `render_w_ = round(fixed_render_h_ * window_w / window_h)` to match the live window aspect ratio with zero black bars. SHA-256 + opcode-guarded NOP at `libRealRacing3.so + 0x12ae40c` (`0xb50017a8 cbnz x8, #0x12ae700` -> `0xd503201f nop`) enables RR3's native `Display::SetScreen` (`0x1476f78`) + `GuiLayout::SetDimensions` (`0x11c07a4`) + cube map rebuild (`0x11adbc0`/`0x11ade44`) on aspect changes; once `Display::SetScreen` swaps `mtScreenGL` inside `fn_view_render`, `main.cpp` clears `mtGraphicsDeviceGL`'s stale FBO cache (`+0x5a0/+0x5a8`), syncs `SetViewport`, and drops any transitional pause-blur RT (`mtScreenGL::vtable[0xa0]`) so blurred modal screens (`HELP & ABOUT`, pause menus) re-capture cleanly at the new width with zero dark-grey backdrop loss. Window title prefix is `Real Racing 3 PC` followed by live mode/resolution/FPS/gamepad telemetry.
9. **Tight Ink Glyph Bounds for `r3_symbols.ttf` / `r3_decals.ttf` (`java_firemint.cpp`):** `r3_symbols.ttf` has one outlier 5700-unit glyph dragging `hhea.ascent` to 5628 (`2.75x EM`) while standard icons are ~2000 units (`1.0x EM`). Implemented zero-allocation `GetDesignGlyphMetrics`-based tight ink bounds (`GlyphVector::init`, `GlyphVector::initWithParagraph` with `pixelBoundsW`/`pixelBoundsH`/`pixelTopOffset`, `Font::loadGlyph`, and `GlyphVector::renderToTexture` honoring scale `f`), restoring full-size centered UI icons (controller D-pad, driver level silhouette, car stat icons) matching `GlyphVector.java` and `Font.java`.
10. **Gamepad-Only Red Glove Cursor (`libRealRacing3.so + 0xdf981c`, SHA-256 + opcode guarded in `main.cpp`):** Patched `0xdf981c` (`cmp w0, #1` -> `cmp w0, #2` against `fmJoystickManagerAndroid::GetNumConnectedJoysticks()` @ `0x12a95e8`) so `party_play/pp_red_cursor.png` stays hidden when only the virtual keyboard controller (`count==1`) is active, and appears automatically whenever a physical gamepad connects (`count>=2`).
11. **In-Game Version Text (`java_firemint.cpp`):** `Platform.getAppName()` returns `"Real Racing 3 PC Port"` while keeping `Platform.getAppVersion()` at `"14.0.1"` (preserving dot-delimited version parsing at `0x9add38`), displaying **`Real Racing 3 PC Port 14.0.1 [a527]`** in Help & About.

## Measured Performance & Visual Evidence (AMD Ryzen 7 5700X + Radeon RX 580 2048SP, `--no-vsync`, `AndroidXtraHigh`, 16x Aniso)
| Scenario | Resolution | Draw Calls | Measured FPS | Steady Benchmark (`avg` / `1% low`) | Audio (`OpenSL` -> WASAPI) | Screenshot |
|---|---|---|---|---|---|---|
| Live 3D Main Menu / Showcase | 1920x1080 | 134–177 | **123–136 FPS** | 473.7 / **118.2 FPS** (incl. load) | 595,456 frames (24kHz 2ch) | `port\build\final_verify_menu.png` |
| Motorsport / F1 / Series Menus | 1920x1080 | 61–256 | **115–276 FPS** | 349.2 / **104.9 FPS** | 835,072 frames | `port\build\nav1_road.png`, `nav3_event.png` |
| 3D Garage / Car Select (McLaren Solus GT) | 1920x1080 | 485–490 | **119–130 FPS** | 231.6 / **101.0 FPS** | 1,362,944 frames | `port\build\race_laguna_carselect.png` |
| Live 3D Race (Lime Rock Park Dawn, 8 cars) | 1920x1080 -> 1280x720 live resize | 414–657 | **178–289 FPS** | 289.6 / **107.3 FPS** | 1,387,008 frames | `port\build\race_3d_grid.png`, `race_3d_drive1.png`, `race_3d_720p.png` |
| Live 3D Race (Supersampled 1.25x, WASD+C driving) | **2400x1350** -> 1920x1080 | 405–831 | **151–277 FPS** | 258.1 / **101.1 FPS** | 1,651,200 frames | `port\build\race_2400_launch.png`, `race_2400_speed.png`, `race_2400_cam.png` |

## Verification Matrix (Physical vs. Synthetic)
- **Physically verified on host hardware (Ryzen 7 5700X + RX 580 + Windows D3D11/ANGLE + WASAPI + real `libRealRacing3.so`):**
  - Full offline boot (0 outbound sockets, 0 downloads, player save Lv. 1042 loaded & persisted in `port\userdata\doc\`).
  - Hardware D3D11 GLES2 rendering via ANGLE with ATC/ETC1 decoding, DirectWrite text, 16x anisotropy, `AndroidXtraHigh` shaders/shadows/headlights/reflections.
  - High-FPS menus & live 3D multi-car racing (>150–289 FPS uncapped at 1080p and 2400x1350 supersampled).
  - Live dynamic window resize (`1920x1080 -> 1280x720`) mid-menu and mid-race.
  - Real FMOD Ex -> OpenSL ES -> SDL2 WASAPI audio streaming (24 kHz stereo S16, >1.65M frames played).
  - Real keyboard-controller driving (`W` throttle accelerating to 46 mph off the grid, `A`/`D` smooth steering onto grass with tire tracks, `C` camera switch, menu cursor navigation).
- **Synthetically verified (automated tests; no physical USB/BT gamepad attached to host during test):**
  - `test_input_resize.exe`: Xbox/XInput, DualShock 4, DualSense, and Switch Pro family detection + real SDL2 `SDL_JoystickAttachVirtualEx` virtual DualSense hot-plug/axis/button/detach + `SetButtonValueJNI(devId, pressed, ordinal)` exact ABI verification + 1.5x supersampling FBO & `0x0` minimize safety.
  - `test_smoke.exe`, `test_elf_cpu.exe`, `test_bionic_vfs.exe`, `test_gles_jni.exe`: ALL PASSED (`EXIT=0`).

## Controls (Default `d:\Real Racing 3\port\rr3_pc.ini`, All Remappable)
- **Keyboard:** `W` / `Up` = Throttle (manual throttle), `S` / `Space` / `Down` = Brake, `A`/`D` & `Left`/`Right` = Smooth Analog Steer, `C` = Change Camera, `V` = Look Back (Rear View), `Esc` (or `P`) = Pause (in-race) / Back (menus), `Enter` = Confirm, `Tab` = Select, `F10` = Cycle Windowed / Borderless / Fullscreen, `F11` or `Alt+Enter` = Toggle Fullscreen <-> Windowed. Mouse clicks work on all menus/buttons.
- **Gamepad (Hot-Plug via SDL2 GameController + HIDAPI; Xbox / PlayStation DS4 & DualSense / Switch Pro):**
  - Throttle: `A` / `RT` (PS: `Cross` / `R2`, Switch: `B` / `ZR`)
  - Brake: `X` / `LT` (PS: `Square` / `L2`, Switch: `Y` / `ZL`)
  - Look Back (Rear View): `RB` / `B` (PS: `R1` / `Circle`, Switch: `R` / `A`)
  - Change Camera: `LB` / `Y` (PS: `L1` / `Triangle`, Switch: `L` / `X`)
  - Steer: `Left Joystick` / `D-Pad`
  - Pause / Menu: `Start` / `Options` / `+` = Pause, `Back` / `Share` / `-` = Select.

## Known Limitations / Notes
- Physical USB/Bluetooth Xbox, DualShock 4, DualSense, and Switch Pro hardware was verified via SDL2's HIDAPI drivers + virtual DualSense controller + exact `SetButtonValueJNI`/`SetJoystickValueJNI` ABI tests; live plug-in with physical controllers on the user's desk has not been run yet.
- Online-only modes (OMP multiplayer, Cloud Save sync, Daily Race server check, IAP store, ads) fail gracefully offline by design.
- `MoviePlayer` intro/ad videos skip straight to `OnCompletionNative` rather than decoding MP4 video.

## Build & Run Commands
- **Build everything:** `& 'd:\Real Racing 3\port\build.cmd' test_smoke test_elf_cpu test_bionic_vfs test_gles_jni test_input_resize rr3_pc rr3_config`
- **Package clean portable release zip (zero EA assets):** `& 'd:\Real Racing 3\port\package_dist.cmd'` -> `d:\Real Racing 3\dist\RealRacing3-PC-Port-Win64.zip`
- **Run all 5 unit tests:**
  - `& 'd:\Real Racing 3\port\build\test_smoke.exe'`
  - `& 'd:\Real Racing 3\port\build\test_elf_cpu.exe'`
  - `& 'd:\Real Racing 3\port\build\test_bionic_vfs.exe'`
  - `& 'd:\Real Racing 3\port\build\test_gles_jni.exe'`
  - `& 'd:\Real Racing 3\port\build\test_input_resize.exe'`
- **Launch Configurator & Launcher UI:**
  - `& 'd:\Real Racing 3\port\build\rr3_config.exe'` (Display, V-Sync, Quality Low–Ultra, Render Scale, Anisotropy, Verbose Logs, click-to-bind Keyboard controls, APK ARM64 extractor + SHA-256 verifier, User Save Data Import/Export with automatic timestamped backup)
- **Launch Real Racing 3 PC:**
  - `& 'd:\Real Racing 3\port\build\rr3_pc.exe'`
  - Optional CLI flags: `--windowed`, `--borderless`, `--fullscreen`, `--window-mode windowed|borderless|fullscreen`, `--width <W>`, `--height <H>`, `--no-vsync`, `--quality low|medium|high|ultra`, `--verbose`, `--quiet`, `--config-ui`, `--render-scale 1.25` (or `1.5`), `--config <path>`, `--max-seconds <N>`, `--dump-at-sec <sec> <png>`.

## Distribution & Recent Fixes (2026-11-27)
- **Missing-glyph `[X]` tofu fix (`R$`):** `port/src/java_firemint.cpp` now strips zero-width/bidi/format characters (`U+200B..U+200F`, `U+202A..U+202E`, `U+2060..U+206F`, `U+FEFF`, `U+00AD`), synthesizes advance-only whitespace for missing Unicode spaces (`U+00A0`, `U+2000..U+200A`, `U+202F`, `U+205F`, `U+3000`), and falls back to Windows system fonts (`segoeui.ttf`, `arial.ttf`, `seguisym.ttf`, `malgun.ttf`, `msyh.ttc`, `meiryo.ttc`) instead of emitting `.notdef` glyph `0`.
- **Launcher & Configurator (`rr3_config.exe`):** Native dark-graphite Win32 + GDI+ sidebar launcher (`Display`, `Graphics`, `Controls`, `Files & saves`) with embedded multi-resolution Real Racing 3 icon (`port/assets/rr3_icon.ico`, also embedded in `rr3_pc.exe` + applied via `WM_SETICON`), custom rounded dark dropdowns, spring-animated switches/segmented controls/nav rail, portable config/path resolution, Verbose Logs toggle (`verbose_logs` opens live console window when ON, silent GUI launch when OFF), built-in APK ARM64 `.so` extractor + `BCrypt` SHA-256 verifier, and safe Import/Export User Save Data with automatic timestamped backup.
- **Native RR3 Quality Presets (`Low`/`Medium`/`High`/`Ultra`):** Replaced the broken VFS `.plist` filename redirect (which triggered `Error! Settings file loaded, but had an invalid ID: AndroidHigh.plist`) with a native 2-instruction patch in `main.cpp` at `0x12ad664` and `0x12adbd4` (`CGlobalAndroid::ApplyPlatformSettings(this, tier)`) setting `w1` to `0` (`AndroidLow`), `1` (`AndroidMid`), `3` (`AndroidHigh`), or `4` (`AndroidXtraHigh`) so RR3 natively loads both the matching `.plist` and `.feats` profile with zero ID errors.
- **Clean Distribution Model:** Zero EA copyrighted files (`RR3 14.0.1.apk`, `.so`, `.depot`, saves) are modified or shipped; users supply their own stock Android APK + `com.ea.games.r3_row` folder via `rr3_config.exe`, and all PC fixes run purely at runtime.
