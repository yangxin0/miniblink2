#!/usr/bin/env bash
# build-samples.sh — build the wke sample app (minibrowser) against a dist/ SDK.
#
#   scripts/build-samples.sh [--release|--debug] [--dyn|--static|--both] [--chromium DIR]
#
# Rebuilds dist/<mode>/minibrowser_{dyn,static} from samples/minibrowser_main.mm against the
# libraries in dist/<mode>/ (build them first with scripts/build-lib.sh):
#   --dyn     -> minibrowser_dyn     links libminiblink2.dylib (rpath @loader_path)
#   --static  -> minibrowser_static  links libminiblink2.a (the complete archive)
#
# Run a built sample FROM dist/<mode>/ — it loads icudtl.dat / the paks / the V8 snapshot
# from beside the binary:  (cd dist/release && ./minibrowser_dyn https://example.com)
#
# NOTE: a --size-optimized static .a is ThinLTO BITCODE, so the static link re-runs LTO
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
SRC="$HERE/samples/minibrowser_main.mm"
[ -f "$SRC" ] || { echo "error: missing $SRC" >&2; exit 1; }

# samples #include "wke/wke.h"; -I src resolves it (same header staged into dist/include).
# -isysroot is explicit: the Chromium clang++ (used for the static/LTO link) has no default
# macOS SDK, unlike the system clang++.
SDKROOT="$(xcrun --show-sdk-path 2>/dev/null || echo /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk)"
CXXFLAGS=(-ObjC++ -std=c++20 -fobjc-arc
          -Wno-deprecated-anon-enum-enum-conversion
          -isysroot "$SDKROOT"
          -I "$HERE/src")

# --- dynamic: the dylib already resolves the whole engine + curl, so the sample only needs
# --- itself + Cocoa; @loader_path lets it find libminiblink2.dylib sitting beside it.
if [ "$FORM" = dyn ] || [ "$FORM" = both ]; then
  [ -f "$DIST/libminiblink2.dylib" ] || {
    echo "error: no $DIST/libminiblink2.dylib — build it first: scripts/build-lib.sh --shared ${MODE:+--$MODE}" >&2; exit 1; }
  echo "==> minibrowser_dyn   (links libminiblink2.dylib)"
  clang++ "${CXXFLAGS[@]}" "$SRC" \
    -L "$DIST" -lminiblink2 -Wl,-rpath,@loader_path \
    -framework Cocoa \
    -o "$DIST/minibrowser_dyn"
  [ "$MODE" != debug ] && strip -x "$DIST/minibrowser_dyn"   # drop local symbols (release)
  echo "    -> $DIST/minibrowser_dyn ($(du -h "$DIST/minibrowser_dyn" | cut -f1))"
fi

# --- static: libminiblink2.a is the COMPLETE engine archive, so the consumer must supply
# --- every system framework/lib it uses. Derive that set from the dylib's load commands when
# --- present (kept in sync automatically), plus vendored libcurl. -Wl,-ObjC forces ObjC
# --- category objects in. lld handles the ThinLTO bitcode a --size-optimized .a contains.
if [ "$FORM" = static ] || [ "$FORM" = both ]; then
  [ -f "$DIST/libminiblink2.a" ] || {
    echo "error: no $DIST/libminiblink2.a — build it first: scripts/build-lib.sh --static ${MODE:+--$MODE}" >&2; exit 1; }
  CLANGXX="$CHROMIUM/third_party/llvm-build/Release+Asserts/bin/clang++"
  [ -x "$CLANGXX" ] || CLANGXX=clang++   # system clang++ works for a non-LTO (native) .a

  # A --size-optimized .a is ThinLTO BITCODE. build-lib.sh merges it with Apple `libtool`,
  # which can't put bitcode + native objects in one uniform archive: it emits a FAT archive
  # with the bitcode in a cputype(0) slice and the native objects in an arm64 slice. Linking
  # for arm64 then reads ONLY the arm64 slice and reports every wke*/mb* symbol undefined.
  # Detect that and stop with guidance rather than a wall of "undefined symbol" errors.
  if lipo -info "$DIST/libminiblink2.a" 2>/dev/null | grep -q "cputype (0)"; then
    echo "error: $DIST/libminiblink2.a is a ThinLTO (--size-optimized) bitcode archive; it" >&2
    echo "       can't be static-linked as-is (bitcode lands in a cputype(0) fat slice the" >&2
    echo "       linker ignores). Options:" >&2
    echo "         - use the dynamic library instead:  scripts/build-samples.sh --dyn" >&2
    echo "         - rebuild the archive WITHOUT --size-optimized (native objects):" >&2
    echo "               scripts/build-lib.sh --static   # then re-run --static here" >&2
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

  # The Rust global-allocator glue (rust_allocator_internal::*) is DEFINED in a bitcode object
  # (build/rust/allocator/allocator_impls.o) but REFERENCED only by the native Rust allocator
  # rlib. In a --size-optimized (ThinLTO) link, LTO sees no bitcode user and internalizes/drops
  # those defs, so the native rlib's references come up undefined. Pin them with -u so LTO keeps
  # them live and pulls the defining member. (Regenerate on an uprev:
  #   nm out/.../obj/build/rust/allocator/liballocator_*.rlib | grep rust_allocator_internal)
  UFLAGS=(
    -Wl,-u,__ZN23rust_allocator_internal5allocEmm
    -Wl,-u,__ZN23rust_allocator_internal7deallocEPhmm
    -Wl,-u,__ZN23rust_allocator_internal7reallocEPhmmm
    -Wl,-u,__ZN23rust_allocator_internal12alloc_zeroedEmm
    -Wl,-u,__ZN23rust_allocator_internal24alloc_error_handler_implEv
  )
  echo "==> minibrowser_static  (links libminiblink2.a; ${#FW[@]} frameworks; $(basename "$CLANGXX") + lld)"
  echo "    (ThinLTO codegen of the reachable engine — slow; give it several min)"
  "$CLANGXX" "${CXXFLAGS[@]}" "$SRC" \
    "$DIST/libminiblink2.a" \
    -fuse-ld=lld -Wl,-ObjC -Wl,-dead_strip \
    "${UFLAGS[@]}" \
    "${FW[@]}" \
    -L "$HERE/third_party/curl/lib" -lcurl \
    -lresolv -lsandbox -lbsm \
    -o "$DIST/minibrowser_static"
  # Strip local symbols (release): the static link retains ~500k of them (~67MB of __LINKEDIT),
  # which is why an unstripped minibrowser_static (173MB) dwarfs the dylib (97MB, already
  # stripped). Stripped it's ~110MB — i.e. the engine code + the sample, no symbol-table bloat.
  [ "$MODE" != debug ] && strip -x "$DIST/minibrowser_static"
  echo "    -> $DIST/minibrowser_static ($(du -h "$DIST/minibrowser_static" | cut -f1))"
fi

echo "==> done. run FROM the dist dir so runtime data is found:"
echo "    (cd $DIST && ./minibrowser_dyn https://example.com)"
