@echo off
setlocal
pushd "%~dp0"

set "VCVARSALL=G:\VS Build\VC\Auxiliary\Build\vcvarsall.bat"
if not defined VSCMD_ARG_TGT_ARCH (
    call "%VCVARSALL%" x64
)

set "OUT=test_raycast.exe"
cl /nologo /std:c++20 /EHsc /W4 /permissive- /utf-8 /D_UNICODE /DUNICODE /DNOMINMAX ^
   /Isrc ^
   tests\test_raycast.cpp ^
   /link /OUT:%OUT%
if errorlevel 1 exit /b 1

".\%OUT%"
