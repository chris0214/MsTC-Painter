@echo off
setlocal
pushd "%~dp0"

:: Environment paths
set "VCVARSALL=G:\VS Build\VC\Auxiliary\Build\vcvarsall.bat"
set "QT_DIR=G:\Qt\6.10.1\msvc2022_64"
set "CMAKE=G:\cmake\bin\cmake.exe"
set "NINJA=G:\cmake\bin\ninja.exe"

:: Setup MSVC x64 environment (vcvarsall may set its own VCPKG_ROOT, so we reset it after)
if not defined VSCMD_ARG_TGT_ARCH (
    call "%VCVARSALL%" x64
    if errorlevel 1 (
        echo ERROR: vcvarsall.bat failed
        exit /b 1
    )
)

:: Force our vcpkg, must be set AFTER vcvarsall (which sets its own bundled vcpkg)
set "VCPKG_ROOT=G:\vcpkg"

:: Determine build type
set "BUILD_TYPE=Debug"
if /i "%1"=="release" set "BUILD_TYPE=Release"

set "BUILD_DIR=build\%BUILD_TYPE%"

echo.
echo ========================================
echo  msTC Texture Studio - %BUILD_TYPE% Build
echo ========================================
echo.

:: Configure
if not exist "%BUILD_DIR%\build.ninja" (
    "%CMAKE%" -S . -B "%BUILD_DIR%" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=%BUILD_TYPE% -DCMAKE_PREFIX_PATH="%QT_DIR%" -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows
    if errorlevel 1 (
        echo ERROR: CMake configure failed
        exit /b 1
    )
)

:: Build
"%CMAKE%" --build "%BUILD_DIR%" --parallel
if errorlevel 1 (
    echo ERROR: Build failed
    exit /b 1
)

:: Deploy Qt DLLs (only if not already deployed)
if not exist "%BUILD_DIR%\Qt6Core.dll" (
    echo Deploying Qt runtime...
    "%QT_DIR%\bin\windeployqt.exe" --no-translations "%BUILD_DIR%\msTCTextureStudio.exe"
)

echo.
echo Build complete: %BUILD_DIR%\msTCTextureStudio.exe
echo.
