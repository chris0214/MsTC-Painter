@echo off
setlocal
pushd "%~dp0"

set "VCVARSALL=G:\VS Build\VC\Auxiliary\Build\vcvarsall.bat"
if not defined VSCMD_ARG_TGT_ARCH (
    call "%VCVARSALL%" x64
)

set "OUT=test_layers.exe"
cl /nologo /std:c++20 /EHsc /W4 /permissive- /utf-8 /Zc:__cplusplus /D_UNICODE /DUNICODE /DNOMINMAX ^
   /MDd ^
   /Isrc ^
   tests\test_layers.cpp ^
   src\editor\Layer.cpp src\editor\LayerStack.cpp src\editor\TextureDocument.cpp ^
   src\editor\UndoHistory.cpp ^
   /link /OUT:%OUT%
if errorlevel 1 exit /b 1

".\%OUT%"
