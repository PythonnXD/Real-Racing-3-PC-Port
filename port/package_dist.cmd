@echo off
setlocal enabledelayedexpansion

set "BUILD_DIR=d:\Real Racing 3\port\build"
set "DIST_ROOT=d:\Real Racing 3\dist"
set "STAGE_DIR=%DIST_ROOT%\RealRacing3PC"
set "ZIP_PATH=%DIST_ROOT%\RealRacing3-PC-Port-Win64.zip"

if not exist "%BUILD_DIR%\rr3_pc.exe" (
    echo [ERROR] Missing %BUILD_DIR%\rr3_pc.exe - build first with port\build.cmd
    exit /b 1
)

if exist "%STAGE_DIR%" rmdir /s /q "%STAGE_DIR%"
if exist "%ZIP_PATH%" del /f /q "%ZIP_PATH%"
mkdir "%STAGE_DIR%"

copy /y "%BUILD_DIR%\rr3_pc.exe" "%STAGE_DIR%\" >nul
copy /y "%BUILD_DIR%\SDL2.dll" "%STAGE_DIR%\" >nul
copy /y "%BUILD_DIR%\libEGL.dll" "%STAGE_DIR%\" >nul
copy /y "%BUILD_DIR%\libGLESv2.dll" "%STAGE_DIR%\" >nul
copy /y "%BUILD_DIR%\d3dcompiler_47.dll" "%STAGE_DIR%\" >nul
copy /y "d:\Real Racing 3\port\README_DIST.txt" "%STAGE_DIR%\README.txt" >nul

> "%STAGE_DIR%\rr3_pc.ini" (
    echo [display]
    echo # window_mode: windowed ^| borderless ^| fullscreen
    echo window_mode=windowed
    echo # 0 = auto-detect best size fitting your monitor without overflowing
    echo width=0
    echo height=0
    echo vsync=true
    echo fps_cap=0
    echo unlock_menu_fps=true
    echo.
    echo [graphics]
    echo # quality_preset: low ^| medium ^| high ^| ultra
    echo quality_preset=ultra
    echo render_scale=1.0
    echo anisotropy=16
    echo msaa_samples=0
    echo.
    echo [logging]
    echo # verbose_logs: true opens a live console window with detailed engine/JNI/GLES logs
    echo verbose_logs=false
    echo.
    echo [input]
    echo android_tv_mode=false
    echo steer_ramp_rate=4.5
    echo steer_return_rate=7.0
    echo key_throttle=W
    echo key_brake=S
    echo key_left=A
    echo key_right=D
    echo key_handbrake=Space
    echo key_camera=C
    echo key_rear_view=V
    echo key_confirm=Return
    echo key_back=Escape
    echo key_pause=Escape
)

powershell -NoProfile -Command "Compress-Archive -Path '%STAGE_DIR%\*' -DestinationPath '%ZIP_PATH%' -Force"
if errorlevel 1 exit /b 1

echo [OK] Created clean portable release: %ZIP_PATH%
powershell -NoProfile -Command "Add-Type -AssemblyName System.IO.Compression.FileSystem; [System.IO.Compression.ZipFile]::OpenRead('%ZIP_PATH%').Entries | Select-Object FullName, Length | Format-Table -AutoSize"
exit /b 0
