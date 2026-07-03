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

# Output the binary INTO out/Release so it sits next to the engine's runtime
# data (icudtl.dat, the resource paks, the v8 snapshot) and libminiblink_host.dylib
# — the engine loads ICU/resources relative to the executable, so a sample built
# elsewhere aborts with "icudtl.dat not found". This mirrors mb_shot / mb_demo.
echo "==> building minibrowser -> $OUT/minibrowser"
clang++ "${CXXFLAGS[@]}" "$HERE/minibrowser_main.mm" "${LDFLAGS[@]}" -o "$OUT/minibrowser"

echo "==> built: $OUT/minibrowser"
echo "    run:  (cd $OUT && ./minibrowser https://example.com)"
