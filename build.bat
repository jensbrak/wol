@echo off
setlocal

:: Copyright 2026 Jens Bråkenhielm
:: SPDX-License-Identifier: MIT
::
:: ---------------------------------------------------------------------------
:: build.bat  -  Builds wol.exe with MSVC (cl.exe)
::
:: Usage:
::   build.bat          release build  ->  wol.exe
::   build.bat debug    debug build    ->  wold.exe  (debug symbols, no optimisation)
::
:: If cl.exe is already on PATH (Developer Command Prompt) it is used as-is.
:: Otherwise the MSVC toolchain is located via vswhere.exe and initialised
:: automatically.
:: Requires Visual Studio 2017+ or Build Tools for VS 2017+ with the
:: "Desktop development with C++" workload.
:: ---------------------------------------------------------------------------

set DEBUG=0
if /i "%~1"=="debug" (
    set DEBUG=1
) else if not "%~1"=="" (
    echo Error: unknown argument: %~1
    exit /b 1
)

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
    cl.exe /nologo /W4 /WX /Od /Zi /std:c17 /MTd /DDEBUG ^
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

endlocal
