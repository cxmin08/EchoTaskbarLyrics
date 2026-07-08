@echo off
REM SPDX-License-Identifier: GPL-3.0
REM Build script for EchoTaskbarLyrics
REM Automatically applies MSVC 14.44 toolset to match ixwebsocket.lib

setlocal

set "PROJECT_DIR=%~dp0"
set "BUILD_DIR=%PROJECT_DIR%out\build"
set "TOOLSET_VERSION=14.44.35207"

if "%1"=="" (
    echo Usage: build [debug^|release^|clean]
    echo.
    echo   debug   - Build Debug configuration
    echo   release - Build Release configuration
    echo   clean   - Remove all build directories
    goto :eof
)

if /i "%1"=="clean" (
    echo Cleaning build directories...
    if exist "%BUILD_DIR%\x64-Debug" rmdir /s /q "%BUILD_DIR%\x64-Debug"
    if exist "%BUILD_DIR%\x64-Release" rmdir /s /q "%BUILD_DIR%\x64-Release"
    if exist "%BUILD_DIR%\x64-Debug-ninja" rmdir /s /q "%BUILD_DIR%\x64-Debug-ninja"
    echo Done.
    goto :eof
)

if /i "%1"=="debug" (
    set "CONFIG=x64-Debug"
    set "CONFIG_TYPE=Debug"
) else if /i "%1"=="release" (
    set "CONFIG=x64-Release"
    set "CONFIG_TYPE=Release"
) else (
    echo Unknown configuration: %1
    goto :eof
)

echo Configuring %CONFIG%...
cmake -B "%BUILD_DIR%\%CONFIG%" -S "%PROJECT_DIR%" --preset %CONFIG%
if errorlevel 1 (
    echo Configure failed!
    exit /b 1
)

echo.
echo Building %CONFIG% (%CONFIG_TYPE%) with MSVC %TOOLSET_VERSION%...
cmake --build "%BUILD_DIR%\%CONFIG%" --config %CONFIG_TYPE% -- /p:PlatformToolsetVersion=%TOOLSET_VERSION%
if errorlevel 1 (
    echo Build failed!
    exit /b 1
)

echo.
echo Build successful! Output: %BUILD_DIR%\%CONFIG%\%CONFIG_TYPE%\EchoTaskbarLyrics.exe
