@echo off
setlocal
call "D:\Visual Studio\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "CMAKE=D:\Visual Studio\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=D:\Visual Studio\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if not exist "d:\Real Racing 3\port\build\build.ninja" (
    "%CMAKE%" -S "d:\Real Racing 3\port" -B "d:\Real Racing 3\port\build" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=RelWithDebInfo
    if errorlevel 1 exit /b 1
)
if "%~1"=="" (
    "%CMAKE%" --build "d:\Real Racing 3\port\build"
) else (
    "%CMAKE%" --build "d:\Real Racing 3\port\build" --target %*
)
exit /b %errorlevel%
