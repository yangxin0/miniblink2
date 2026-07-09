#!/usr/bin/env bash
# build-samples.sh — build the mb-API samples against a dist/ SDK (macOS).
#
#   scripts/build-samples.sh [--release|--debug] [--dyn|--static|--both] [--chromium DIR]
#
# Builds the whole sample set from samples/ against the libraries in
# dist/<mode>/ (build them first with scripts/build-lib.sh):
#   --dyn     -> every sample, linked against libminiblink2.dylib (@loader_path)
#   --static  -> minibrowser_static only, linked against libminiblink2.a
# Windowed samples are OS-INDEPENDENT: they share the scaffold interface in
# samples/compat/mb_window.h; this script links the Cocoa backend
# (compat/mac/), samples/build.ps1 links the Win32 one (compat/win/).
#
# Run a built sample FROM dist/<mode>/ — it loads icudtl.dat / the paks / the V8 snapshot
# from beside the binary:  (cd dist/release && ./minibrowser_dyn https://example.com)
#
# NOTE: if dist holds a LEGACY ThinLTO bitcode .a (pre---ship builds), the static link re-runs LTO
# codegen and needs an LTO-capable linker — this script uses the Chromium clang++/lld for it
# (hence --chromium / CHROMIUM, defaulting to a sibling checkout, like build-lib.sh). The
# dynamic build just uses the system clang++ (the dylib is an ordinary native library).
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
PARENT="$(dirname "$HERE")"
CHROMIUM="${CHROMIUM:-$PARENT/chromium-150.0.7871.24}"
MODE=release
FORM=both

while [ $# -gt 0 ]; do
  case "$1" in
    --release) MODE=release ;;
    --debug)   MODE=debug ;;
    --dyn)     FORM=dyn ;;
    --static)  FORM=static ;;
    --both)    FORM=both ;;
    --chromium) CHROMIUM="$2"; shift ;;
    -h|--help) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "build-samples.sh: unknown arg '$1'" >&2; exit 2 ;;
  esac
  shift
done

DIST="$HERE/dist/$MODE"
SAMPLES="$HERE/samples"
# minibrowser (sample 8) is OS-independent app code + the compat scaffold +
# the CDP bridge; the same trio builds on Windows via samples/build.ps1.
SRC=("$SAMPLES/sample8_minibrowser/main.cc"
     "$SAMPLES/sample8_minibrowser/cdp_bridge.cc"
     "$SAMPLES/compat/mac/mb_window.mm")
[ -f "${SRC[0]}" ] || { echo "error: missing ${SRC[0]}" >&2; exit 1; }

# samples #include "miniblink2/automation.h"; -I src resolves it (the same header the
# SDK ships as dist/include/miniblink2/automation.h).
# -isysroot is explicit: the Chromium clang++ (used for the static/LTO link) has no default
# macOS SDK, unlike the system clang++.
SDKROOT="$(xcrun --show-sdk-path 2>/dev/null || echo /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk)"
CXXFLAGS=(-ObjC++ -std=c++20 -fobjc-arc
          -Wno-deprecated-anon-enum-enum-conversion
          -isysroot "$SDKROOT"
          -I "$HERE/src")

# --- dynamic: the dylib already resolves the whole engine + curl, so a sample only needs
# --- itself + Cocoa; @loader_path lets it find libminiblink2.dylib sitting beside it.
if [ "$FORM" = dyn ] || [ "$FORM" = both ]; then
  [ -f "$DIST/libminiblink2.dylib" ] || {
    echo "error: no $DIST/libminiblink2.dylib — build it first: scripts/build-lib.sh --shared ${MODE:+--$MODE}" >&2; exit 1; }
  LDDYN=(-L "$DIST" -lminiblink2 -Wl,-rpath,@loader_path)

  echo "==> minibrowser_dyn   (links libminiblink2.dylib)"
  clang++ "${CXXFLAGS[@]}" -I "$SAMPLES" "${SRC[@]}" "${LDDYN[@]}" \
    -framework Cocoa -framework QuartzCore \
    -o "$DIST/minibrowser_dyn"
  [ "$MODE" != debug ] && strip -x "$DIST/minibrowser_dyn"   # drop local symbols (release)
  echo "    -> $DIST/minibrowser_dyn ($(du -h "$DIST/minibrowser_dyn" | cut -f1))"

  # The Ultralight-parity sample set (see samples/README.md). Headless ones are
  # plain C/C++; windowed ones add the shared Cocoa scaffold from samples/compat/.
  SCAFFOLD="$SAMPLES/compat/mac/mb_window.mm"
  build_one() {  # name, extra sources...
    local name="$1"; shift
    echo "==> $name"
    clang++ "${CXXFLAGS[@]}" -I "$SAMPLES" "$@" "${LDDYN[@]}" \
      -framework Cocoa -framework QuartzCore \
      -o "$DIST/$name"
    [ "$MODE" != debug ] && strip -x "$DIST/$name"
    echo "    -> $DIST/$name"
  }
  build_one sample1_render_to_png "$SAMPLES/sample1_render_to_png/main.cc"
  build_one sample2_basic_app     "$SAMPLES/sample2_basic_app/main.cc"    "$SCAFFOLD"
  build_one sample3_resizable_app "$SAMPLES/sample3_resizable_app/main.cc" "$SCAFFOLD"
  build_one sample4_javascript    "$SAMPLES/sample4_javascript/main.cc"   "$SCAFFOLD"
  build_one sample5_file_loading  "$SAMPLES/sample5_file_loading/main.cc" "$SCAFFOLD"
  build_one sample9_multi_window  "$SAMPLES/sample9_multi_window/main.cc" "$SCAFFOLD"
  # Sample 6 is PLAIN C — proving the mb headers are C-clean end to end.
  echo "==> sample6_intro_c_api (compiled as C99)"
  clang -std=c99 -isysroot "$SDKROOT" -I "$HERE/src" \
    "$SAMPLES/sample6_intro_c_api/main.c" "${LDDYN[@]}" \
    -o "$DIST/sample6_intro_c_api"
  [ "$MODE" != debug ] && strip -x "$DIST/sample6_intro_c_api"
  echo "    -> $DIST/sample6_intro_c_api"
fi

# --- static: libminiblink2.a is the COMPLETE engine archive, so the consumer must supply
# --- every system framework/lib it uses. Derive that set from the dylib's load commands when
# --- present (kept in sync automatically), plus vendored libcurl. -Wl,-ObjC forces ObjC
# --- category objects in. (--release --ship produces a NATIVE .a; lld also handles a
# --- legacy ThinLTO bitcode archive if dist still holds one.)
if [ "$FORM" = static ] || [ "$FORM" = both ]; then
  [ -f "$DIST/libminiblink2.a" ] || {
    echo "error: no $DIST/libminiblink2.a — build it first: scripts/build-lib.sh --static ${MODE:+--$MODE}" >&2; exit 1; }
  CLANGXX="$CHROMIUM/third_party/llvm-build/Release+Asserts/bin/clang++"
  [ -x "$CLANGXX" ] || CLANGXX=clang++   # system clang++ works for a non-LTO (native) .a

  # Defensive: a LEGACY ThinLTO bitcode .a merged with Apple `libtool` (pre-llvm-ar
  # builds) is a FAT archive with the bitcode in a cputype(0) slice and the native
  # objects in an arm64 slice. Linking
  # for arm64 then reads ONLY the arm64 slice and reports every mb* symbol undefined.
  # Detect that and stop with guidance rather than a wall of "undefined symbol" errors.
  if lipo -info "$DIST/libminiblink2.a" 2>/dev/null | grep -q "cputype (0)"; then
    echo "error: $DIST/libminiblink2.a is a legacy libtool-merged bitcode archive; it" >&2
    echo "       can't be static-linked as-is (bitcode lands in a cputype(0) fat slice the" >&2
    echo "       linker ignores). Options:" >&2
    echo "         - use the dynamic library instead:  scripts/build-samples.sh --dyn" >&2
    echo "         - rebuild the archive (native objects):" >&2
    echo "               scripts/build-lib.sh --release --ship   # then re-run --static here" >&2
    exit 1
  fi

  FW=()
  if [ -f "$DIST/libminiblink2.dylib" ]; then
    while read -r d; do
      case "$d" in
        *.framework/*) FW+=(-framework "$(basename "${d%%.framework*}")") ;;
      esac
    done < <(otool -L "$DIST/libminiblink2.dylib" | tail -n +2 | awk '{print $1}' | sort -u)
  else
    # Fallback set if the dylib wasn't built (the frameworks the engine links on macOS).
    for f in Cocoa AppKit Foundation CoreFoundation CoreGraphics CoreText CoreServices \
             ApplicationServices Carbon Quartz QuartzCore Metal MetalKit OpenGL IOSurface \
             CoreVideo CoreMedia CoreAudio AudioToolbox AudioUnit AVFoundation AVFAudio \
             VideoToolbox CoreML Accelerate Security SystemConfiguration Network CFNetwork \
             IOKit CoreBluetooth IOBluetooth LocalAuthentication CryptoTokenKit \
             UniformTypeIdentifiers MediaAccessibility ScreenCaptureKit OpenDirectory \
             Accessibility CoreMediaIO; do FW+=(-framework "$f"); done
  fi

  echo "==> minibrowser_static  (links libminiblink2.a; ${#FW[@]} frameworks; $(basename "$CLANGXX") + lld)"
  echo "    (a native .a links in ~a minute; a legacy bitcode .a re-runs LTO — several min)"
  "$CLANGXX" "${CXXFLAGS[@]}" -I "$SAMPLES" "${SRC[@]}" \
    "$DIST/libminiblink2.a" \
    -fuse-ld=lld -Wl,-ObjC -Wl,-dead_strip \
    "${FW[@]}" \
    -L "$HERE/third_party/curl/lib" -lcurl \
    -lresolv -lsandbox -lbsm \
    -o "$DIST/minibrowser_static"
  # Strip local symbols (release): the static link retains ~500k of them (~67MB of __LINKEDIT),
  # which is why an unstripped minibrowser_static (173MB) dwarfs the dylib (97MB, already
  # stripped). Stripped it's ~110MB — i.e. the engine code + the sample, no symbol-table bloat.
  [ "$MODE" != debug ] && strip -x "$DIST/minibrowser_static"
  # The tracked repo libcurl carries whatever install id it was BUILT with (the tracked
  # binary is never modified — no git churn), which may be a stale path from before a
  # folder rename. Retarget the RECORDED reference in the produced binary to this repo's
  # copy, then re-sign (arm64 kills invalidly-signed images at load).
  CURL_REF="$(otool -L "$DIST/minibrowser_static" | awk '/third_party\/curl/{print $1}')"
  if [ -n "$CURL_REF" ] && [ "$CURL_REF" != "$HERE/third_party/curl/lib/libcurl.4.dylib" ]; then
    install_name_tool -change "$CURL_REF" "$HERE/third_party/curl/lib/libcurl.4.dylib" \
        "$DIST/minibrowser_static" 2>/dev/null
  fi
  codesign --force --sign - "$DIST/minibrowser_static" 2>/dev/null
  echo "    -> $DIST/minibrowser_static ($(du -h "$DIST/minibrowser_static" | cut -f1))"
fi

echo "==> done. run FROM the dist dir so runtime data is found:"
echo "    (cd $DIST && ./minibrowser_dyn https://example.com)"
