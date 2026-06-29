#!/usr/bin/env bash
# Build the macOS wke sample apps against the modern miniblink-host dylib.
#
#   ./build.sh [donor-tree]
#
# The samples link against libminiblink_host.dylib (which contains the wke layer),
# so build the host lib first:  ../build.sh <donor-tree>
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/.." && pwd)"
DONOR="${1:-/Users/yangxin/dennis/chrome/chromium-150.0.7871.24}"
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
# elsewhere aborts with "icudtl.dat not found". This mirrors mb_shot / wke_demo.
for s in wkexe minibrowser; do
  echo "==> building $s -> $OUT/$s"
  clang++ "${CXXFLAGS[@]}" "$HERE/${s}_main.mm" "${LDFLAGS[@]}" -o "$OUT/$s"
done

echo "==> built: $OUT/{wkexe,minibrowser}"
echo "    run e.g.:  (cd $OUT && ./wkexe https://example.com)"
echo "               (cd $OUT && ./minibrowser https://example.com)"
