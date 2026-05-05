@echo off
setlocal

:: Copyright 2026 Jens Bråkenhielm
:: SPDX-License-Identifier: MIT
::
:: ---------------------------------------------------------------------------
:: build.bat  -  Builds wol.exe with MSVC (cl.exe)
::
:: Usage:
::   build.bat             release build   ->  wol.exe
::   build.bat debug       debug build     ->  wold.exe  (debug symbols, no optimisation)
::   build.bat archive     create release archive  ->  wol-vX.Y.Z-windows-x64.zip
::                         (wol.exe must exist; run build.bat first)
::
:: Build:   cl.exe via Developer Command Prompt, or auto-located via vswhere.exe
::          Requires VS 2017+ / Build Tools with "Desktop development with C++"
:: Archive: PowerShell (Windows 10+); 7-Zip used instead if found (max compression)
:: Version: extracted automatically from wol.c
:: ---------------------------------------------------------------------------

set DEBUG=0
set ARCHIVE=0
if not "%~2"=="" (
    echo Error: too many arguments
    exit /b 1
)
if /i "%~1"=="debug" (
    set DEBUG=1
) else if /i "%~1"=="archive" (
    set ARCHIVE=1
) else if not "%~1"=="" (
    echo Error: unknown argument: %~1
    exit /b 1
)

if "%ARCHIVE%"=="1" goto :archive

:: Build
if "%DEBUG%"=="1" (
    set OUT=wold.exe
) else (
    set OUT=wol.exe
)

if not exist "wol.c" (
    echo Error: source file not found: wol.c
    exit /b 1
)

:: Use cl.exe as-is if the environment is already initialised.
where /q cl.exe && goto :build

:: Locate the latest VS installation with C++ tools via vswhere.
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe

if not exist "%VSWHERE%" (
    echo Error: vswhere.exe not found.
    echo        Install Visual Studio 2017+ or Build Tools for Visual Studio.
    echo        https://aka.ms/vs/17/release/vs_BuildTools.exe
    exit /b 1
)

for /f "usebackq delims=" %%i in (
    `"%VSWHERE%" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`
) do set VS_PATH=%%i

if not defined VS_PATH (
    echo Error: No VS installation found with C++ build tools.
    echo        Open Visual Studio Installer and add the
    echo        "Desktop development with C++" workload.
    exit /b 1
)

echo Initialising MSVC x64 toolchain...
echo   %VS_PATH%
echo.
call "%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1

:build
if "%DEBUG%"=="1" (
    cl.exe /nologo /W4 /WX /Od /Zi /std:c17 /MTd /DDEBUG /DWOL_SELF_TEST ^
        /Fe:"%OUT%" wol.c ^
        /link /SUBSYSTEM:CONSOLE /MACHINE:X64 /DEBUG
) else (
    cl.exe /nologo /W4 /WX /O2 /std:c17 /MT /DNDEBUG ^
        /Fe:"%OUT%" wol.c ^
        /link /SUBSYSTEM:CONSOLE /MACHINE:X64
)

if %ERRORLEVEL%==0 (
    echo.
    if "%DEBUG%"=="1" (
        echo Build successful: %OUT% (debug^)
    ) else (
        echo Build successful: %OUT%
    )
) else (
    echo.
    echo Build FAILED  [ERRORLEVEL=%ERRORLEVEL%]
)

goto :end

:archive
:: Extract version from wol.c (#define WOL_VERSION "X.Y.Z")
for /f "tokens=3 delims= " %%i in ('findstr /B /C:"#define WOL_VERSION" wol.c') do set VERSION_RAW=%%i
if defined VERSION_RAW set VERSION=%VERSION_RAW:"=%

if not defined VERSION (
    echo Error: could not extract version from wol.c
    exit /b 1
)

if not exist "wol.exe" (
    echo Error: wol.exe not found -- run build.bat first
    exit /b 1
)

if not exist "readme.txt" (
    echo Error: readme.txt not found
    exit /b 1
)

set ZIP=wol-v%VERSION%-windows-x64.zip

:: Prefer 7-Zip if available (on PATH or default install location) for max compression.
set SEVENZIP=
where /q 7z.exe && set SEVENZIP=7z.exe
if not defined SEVENZIP (
    if exist "%ProgramFiles%\7-Zip\7z.exe" set SEVENZIP=%ProgramFiles%\7-Zip\7z.exe
)

if defined SEVENZIP (
    echo 7-Zip found, using it instead of PowerShell.
    "%SEVENZIP%" a -tzip -mx=9 "%ZIP%" wol.exe readme.txt >nul 2>&1
) else (
    powershell -NoProfile -Command "Compress-Archive -Path 'wol.exe','readme.txt' -DestinationPath '%ZIP%' -Force"
)

if %ERRORLEVEL%==0 (
    echo.
    echo Archive created: %ZIP%
) else (
    echo.
    echo Archive FAILED  [ERRORLEVEL=%ERRORLEVEL%]
    exit /b 1
)

:end
endlocal
