@echo off
rem Build SysWidget.exe (portable, statically linked). Uses g++ from PATH,
rem the WinLibs winget package, or MSVC if that is all that exists.
setlocal
cd /d "%~dp0"

set "GPP="
where g++ >nul 2>nul && set "GPP=g++"
if not defined GPP (
  for /f "usebackq delims=" %%i in (`dir /s /b "%LOCALAPPDATA%\Microsoft\WinGet\Packages\*mingw64\bin\g++.exe" 2^>nul`) do (
    if not defined GPP set "GPP=%%i"
  )
)
if not defined GPP (
  echo [error] g++ not found. Install: winget install BrechtSanders.WinLibs.POSIX.UCRT
  exit /b 1
)

if not exist bin mkdir bin
echo [build] %GPP%
"%GPP%" -std=c++20 -O2 -municode -mwindows -Wall -Wextra -Wno-unused-parameter ^
  src\main.cpp -o bin\SysWidget.exe ^
  -static ^
  -ldwmapi -ld2d1 -ldwrite -ld3d11 -ldxgi -ldcomp ^
  -luuid -lole32 -loleaut32 -luser32 -lgdi32 -lshell32 -ladvapi32 -lshlwapi
if errorlevel 1 (
  echo [error] build failed
  exit /b 1
)
echo [ok] bin\SysWidget.exe
