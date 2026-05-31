@echo off
setlocal
pushd "%~dp0"

set "VCVARSALL=G:\VS Build\VC\Auxiliary\Build\vcvarsall.bat"
if not defined VSCMD_ARG_TGT_ARCH (
    call "%VCVARSALL%" x64
)

:: Resolve Stb include via the project's vcpkg_installed (same as CMake's find_package(Stb))
set "STB_INC=%~dp0build\Debug\vcpkg_installed\x64-windows\include"

:: Qt is needed because TextureGroup.h pulls QImage / UvLines.h pulls QPointF
set "QT_DIR=G:\Qt\6.10.1\msvc2022_64"

set "OUT=test_export.exe"
cl /nologo /std:c++20 /EHsc /W4 /permissive- /utf-8 /Zc:__cplusplus /D_UNICODE /DUNICODE /DNOMINMAX ^
   /MDd ^
   /Isrc /I"%STB_INC%" /I"%QT_DIR%\include" /I"%QT_DIR%\include\QtCore" /I"%QT_DIR%\include\QtGui" ^
   tests\test_export.cpp ^
   src\editor\TextureExport.cpp src\editor\TextureDocument.cpp ^
   src\editor\Layer.cpp src\editor\LayerStack.cpp ^
   src\editor\UndoHistory.cpp ^
   /link /LIBPATH:"%QT_DIR%\lib" /LIBPATH:"%~dp0build\Debug\vcpkg_installed\x64-windows\debug\lib" ^
   Qt6Cored.lib Qt6Guid.lib fmtd.lib spdlogd.lib /OUT:%OUT%
if errorlevel 1 exit /b 1

".\%OUT%"
