#!/bin/sh
# Syntax-checks the Windows-only sources with mingw-w64 (brew install mingw-w64), from a Mac or
# Linux machine that can't build them. Not MSVC, but it catches most mistakes before a venue
# update finds them. Needs a configured build/ (for the JSON and HTTP libraries).
# Usage: tools/wincheck.sh [files...]   (default: every Windows-only source)
cd "$(dirname "$0")/.." || exit 1
CXX=${CXX:-x86_64-w64-mingw32-g++}
J=$(find build/_deps -path "*nlohmann/json.hpp" | head -1 | sed 's#/nlohmann/json.hpp##')
H=$(dirname "$(find build/_deps -name httplib.h | head -1)")
S=$(find build/_deps -name stb_image.h | head -1 | xargs dirname)
FILES=${*:-"engine/src/render/d3d11/d3d11_renderer.cpp engine/src/io/video_mf.cpp engine/src/audio/audio_wasapi.cpp engine/src/io/capture_mf.cpp engine/src/core/canvases.cpp engine/src/api/http_api.cpp receiver/src/present_d3d11.cpp receiver/src/main.cpp receiver/src/present_headless.cpp"}
fail=0
for f in $FILES; do
  [ -f "$f" ] || continue
  inc="-I engine/src -I $J -I $H -I $S"
  case $f in receiver/*) inc="-I receiver/src -I $J -I $H"; defs="-DUNICODE -D_UNICODE";; *) defs="";; esac
  if out=$($CXX -std=c++20 -fsyntax-only -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0A00 $defs $inc "$f" 2>&1 | grep -E "error" | head -15); [ -n "$out" ]; then
    echo "FAIL $f"; echo "$out"; fail=1
  else
    echo "ok   $f"
  fi
done
exit $fail
