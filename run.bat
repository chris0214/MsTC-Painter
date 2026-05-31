@echo off
setlocal

:: Build first (Debug by default)
call "%~dp0build.bat" %1
if errorlevel 1 exit /b 1

:: Determine build dir
set "BUILD_TYPE=Debug"
if /i "%1"=="release" set "BUILD_TYPE=Release"
set "BUILD_DIR=build\%BUILD_TYPE%"

:: Run
echo.
echo Running msTCTextureStudio (%BUILD_TYPE%)...
echo.
"%BUILD_DIR%\msTCTextureStudio.exe"
