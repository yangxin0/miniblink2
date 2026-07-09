#!/usr/bin/env bash
# Build the macOS minibrowser sample against the dev (component) miniblink-host dylib.
#
#   ./build.sh [donor-tree]
#
# The sample drives the miniblink2 mb C API (miniblink2.h) and links
# libminiblink_host.dylib, so build the host lib first:  ../build.sh <donor-tree>
# (To build against the packaged SDK instead, use scripts/build-samples.sh.)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/.." && pwd)"
# Donor tree defaults to the sibling checkout (same convention as build-lib.sh).
DONOR="${1:-$(dirname "$PROJ")/chromium-150.0.7871.24}"
OUT="$DONOR/out/Release"
DYLIB="$OUT/libminiblink_host.dylib"

if [ ! -f "$DYLIB" ]; then
  echo "error: $DYLIB not found."
  echo "       build the host lib first:  $PROJ/build.sh $DONOR"
  exit 1
fi

CXXFLAGS=(-ObjC++ -std=c++20 -fobjc-arc
          -Wno-deprecated-anon-enum-enum-conversion -I "$PROJ/src")
LDFLAGS=(-L "$OUT" -lminiblink_host -framework Cocoa -Wl,-rpath,"$OUT")

# Output the binaries INTO out/Release so they sit next to the engine's runtime
# data (icudtl.dat, the resource paks, the v8 snapshot) and libminiblink_host.dylib
# — the engine loads ICU/resources relative to the executable, so a sample built
# elsewhere aborts with "icudtl.dat not found". This mirrors mb_shot / mb_demo.
echo "==> building minibrowser -> $OUT/minibrowser"
clang++ "${CXXFLAGS[@]}" "$HERE/minibrowser_main.mm" "${LDFLAGS[@]}" -o "$OUT/minibrowser"

# The OS-independent Ultralight-parity set (see README.md): headless samples are
# plain C/C++; windowed ones add the Cocoa backend of the shared scaffold
# (compat/mac; compat/win is the Win32 peer built by samples/build.ps1).
SCAFFOLD="$HERE/compat/mac/mb_window.mm"
for s in sample2_basic_app sample3_resizable_app sample4_javascript \
         sample5_file_loading sample9_multi_window; do
  echo "==> building $s -> $OUT/$s"
  clang++ "${CXXFLAGS[@]}" -I "$HERE" "$HERE/$s.cc" "$SCAFFOLD" "${LDFLAGS[@]}" \
    -framework QuartzCore -o "$OUT/$s"
done
echo "==> building sample1_render_to_png -> $OUT/sample1_render_to_png"
clang++ "${CXXFLAGS[@]}" "$HERE/sample1_render_to_png.cc" "${LDFLAGS[@]}" \
  -o "$OUT/sample1_render_to_png"
echo "==> building sample6_intro_c_api (plain C99) -> $OUT/sample6_intro_c_api"
clang -std=c99 -I "$PROJ/src" "$HERE/sample6_intro_c_api.c" \
  -L "$OUT" -lminiblink_host -Wl,-rpath,"$OUT" -o "$OUT/sample6_intro_c_api"

echo "==> built: $OUT/minibrowser + the sample set"
echo "    run:  (cd $OUT && ./minibrowser https://example.com)"
