Real Racing 3 PC Port (Windows x64), v14.0.1
============================================

ANDROID FILES REQUIRED
----------------------
This download includes only the Windows PC runtime and launcher. No EA or
Firemonkeys game files are bundled. You need two items from Android:

1. Real Racing 3 APK
   Contains the compiled ARM64 game engine (libRealRacing3.so, libc++_shared.so,
   libfmodex.so). Without these libraries there is no game code to run.
2. Android game data folder (com.ea.games.r3_row or its files\ subfolder)
   Contains tracks, cars, audio, shaders, and UI assets inside .depot.

All PC port fixes, graphics presets, and text changes run in memory at runtime.
Your original Android files are never modified.

QUICK START
-----------
1. Extract this zip into any writable folder (for example C:\Games\RealRacing3PC\).
2. Open rr3_pc.exe and go to "Files & saves".
3. Select your Real Racing 3 APK and click "Extract".
4. Select your com.ea.games.r3_row (or files\) folder.
5. Optional: Click "Import save" to bring in your Android character.2.dat or
   a .zip save backup.
6. Adjust Display, Graphics, or Controls if needed, then click "Play".
   (Tip: run rr3_pc.exe --play to skip the launcher straight into the game.)

CONTROLS
--------
Keyboard:
  W / Up Arrow       Throttle (manual acceleration)
  S / Space          Brake
  A / D / Left/Right Steer
  C                  Camera
  V                  Look back
  Escape             Pause / Back
  F10                Cycle Windowed / Borderless / Fullscreen
  F11 / Alt+Enter    Toggle Fullscreen

Gamepad (Xbox, PlayStation DualShock 4 / DualSense, Switch Pro):
  A / Cross or RT / R2     Throttle
  X / Square or LT / L2    Brake
  Left Stick or D-Pad      Steer
  LB / L1 or Y / Triangle  Camera
  RB / R1 or B / Circle    Look back
  Menu / Options           Pause

SAVES
-----
PC saves are stored in userdata\doc\ next to rr3_pc.exe. Use "Files & saves"
in the launcher to export backups or import saves. Importing always copies
your existing saves to a timestamped backup folder first.
