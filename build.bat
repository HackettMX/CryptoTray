@echo off
setlocal
:: vcvars64.bat calls reg.exe, which may be blocked by policy: set MSVC env manually.
set "VCTOOLS=C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231"
set "SDKDIR=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER=10.0.26100.0"

if not exist "%VCTOOLS%\bin\Hostx64\x64\cl.exe" (
    echo [ERR] cl.exe not found: %VCTOOLS%
    echo       Edit VCTOOLS in build.bat to match your installed MSVC version.
    exit /b 1
)

set "INCLUDE=%VCTOOLS%\include;%SDKDIR%\Include\%SDKVER%\ucrt;%SDKDIR%\Include\%SDKVER%\um;%SDKDIR%\Include\%SDKVER%\shared;%SDKDIR%\Include\%SDKVER%\winrt"
set "LIB=%VCTOOLS%\lib\x64;%SDKDIR%\Lib\%SDKVER%\ucrt\x64;%SDKDIR%\Lib\%SDKVER%\um\x64"
set "PATH=%VCTOOLS%\bin\Hostx64\x64;%SDKDIR%\bin\%SDKVER%\x64;C:\Strawberry\c\bin;%PATH%"

cd /d "%~dp0"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl
if errorlevel 1 exit /b 1
cmake --build build
if errorlevel 1 exit /b 1

echo.
echo [OK] build\CryptoTray.exe  build\test_okx.exe  build\test_rate.exe
