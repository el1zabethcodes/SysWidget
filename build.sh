#!/usr/bin/env bash
# Build SysWidget.exe (portable, statically linked).
set -e
cd "$(dirname "$0")"

GPP="${GPP:-}"
if [ -z "$GPP" ]; then
  if command -v g++ >/dev/null 2>&1; then
    GPP=g++
  else
    GPP=$(find "$LOCALAPPDATA/Microsoft/WinGet/Packages" -path '*mingw64/bin/g++.exe' 2>/dev/null | head -1)
  fi
fi
if [ -z "$GPP" ]; then
  echo "[error] g++ not found. Install: winget install BrechtSanders.WinLibs.POSIX.UCRT" >&2
  exit 1
fi

mkdir -p bin
echo "[build] $GPP"
"$GPP" -std=c++20 -O2 -municode -mwindows -Wall -Wextra -Wno-unused-parameter \
  src/main.cpp -o bin/SysWidget.exe \
  -static \
  -ldwmapi -ld2d1 -ldwrite -ld3d11 -ldxgi -ldcomp \
  -luuid -lole32 -loleaut32 -luser32 -lgdi32 -lshell32 -ladvapi32 -lshlwapi -lcomctl32 -luxtheme
echo "[ok] bin/SysWidget.exe"
