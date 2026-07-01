#!/usr/bin/env bash
# build-lib.sh — build a self-contained libminiblink2 (single .dylib and/or .a),
# release or debug, from the miniblink-modern sources.
#
#   scripts/build-lib.sh [--shared|--static|--both] [--release|--debug]
#                      [--webgpu] [--video] [--ml] [--size-optimized]
#                      [--chromium DIR] [--depot DIR] [--no-stage] [--print-only]
#
# Feature flags are include-only (default OFF, to trim toward miniblink49's footprint):
#   --webgpu  WebGPU/Dawn      --video  <video> decode (audio is always on)
#   --ml      WebNN on-device ML (TFLite/LiteRT/XNNPACK backend)
#   --size-optimized  size-optimized ship build (ThinLTO + -Oz + no DCHECKs; release only, slow)
#
# --webgpu links WebGPU (Dawn, ~97MB) into the .dylib AND the .a; omitted by default to
# keep the library smaller (navigator.gpu.requestAdapter() then resolves to null).
#
# CHROMIUM (the Chromium checkout) and DEPOT (depot_tools) default to siblings of this
# project; override with env vars (CHROMIUM=… DEPOT=…) or the --chromium/--depot flags.
#
# Unlike the default build (is_component_build=true), this links the WHOLE engine
# (Blink, V8, skia, base, ...) statically into ONE artifact instead of ~286 sibling
# component dylibs:
#
#   --shared  -> dist/<mode>/libminiblink2.dylib   (self-contained shared library)
#   --static  -> dist/<mode>/libminiblink2.a       (one complete archive)
#   --both    -> both of the above
#
# The first build of a given mode is a full from-scratch compile of the engine in
# non-component mode (slow; shares nothing with out/Release). Re-runs are incremental.
#
# Each dist/<mode>/ is a self-contained SDK:
#   libminiblink2.{dylib,a}            the library (exposes mb_capi + the wke API)
#   include/miniblink2/{wke.h,mb_capi.h}  public headers
#   blink_resources.pak, icudtl.dat, snapshot_blob.bin, v8_context_snapshot.bin
#                               runtime data the engine loads at startup
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"   # repo root (this script lives in scripts/)
# This project, the Chromium checkout, and depot_tools are SIBLINGS under one parent:
#   <parent>/{<this-project>, chromium-150.0.7871.24, depot_tools}
# Both default to siblings of this project; override via env (CHROMIUM=… DEPOT=…) or
# the --chromium / --depot flags. No absolute path is hardcoded.
PARENT="$(dirname "$HERE")"
CHROMIUM="${CHROMIUM:-$PARENT/chromium-150.0.7871.24}"
DEPOT="${DEPOT:-$PARENT/depot_tools}"
FORM=shared          # shared | static | both
MODE=release         # release | debug
STAGE=1
PRINT_ONLY=0
WEBGPU=0             # WebGPU (Dawn) OFF by default (smaller lib); --webgpu links it in
VIDEO=0              # <video> decode OFF by default (audio stays on); --video adds it
ML=0                 # WebNN on-device ML (TFLite/LiteRT/XNNPACK) OFF by default; --ml adds it
SIZE=0               # --size-optimized: ThinLTO + size opt + no DCHECKs ship build (release only, slow)

while [ $# -gt 0 ]; do
  case "$1" in
    --shared) FORM=shared ;;
    --static) FORM=static ;;
    --both)   FORM=both ;;
    --release) MODE=release ;;
    --debug)   MODE=debug ;;
    --chromium) CHROMIUM="$2"; shift ;;
    --depot) DEPOT="$2"; shift ;;
    --no-stage) STAGE=0 ;;          # skip the source sync (sources already staged)
    --print-only) PRINT_ONLY=1 ;;   # gn gen + ninja dry-run, no compile
    --webgpu) WEBGPU=1 ;;           # include WebGPU (Dawn ~97MB); default excludes it
    --video) VIDEO=1 ;;             # include <video> decode (ffmpeg video + AV1); default off
    --ml) ML=1 ;;                   # include WebNN on-device ML (TFLite backend); default off
    --size-optimized) SIZE=1 ;;               # size-optimized ship build: ThinLTO + -Oz + no DCHECKs
    -h|--help) sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "build-lib.sh: unknown arg '$1'" >&2; exit 2 ;;
  esac
  shift
done

[ -d "$CHROMIUM/third_party/blink/renderer" ] || {
  echo "error: not a Chromium checkout: $CHROMIUM" >&2
  echo "  expected $PARENT/chromium-150.0.7871.24 (sibling of $(basename "$HERE")/)," >&2
  echo "  or set CHROMIUM=/path/to/chromium-<ver> (or --chromium DIR)." >&2
  exit 1
}

# depot_tools provides gn + ninja; add it to PATH and fail early with a clear message
# if gn still isn't resolvable. Use plain ninja, NOT autoninja: autoninja auto-selects
# the siso backend, whose vendored binary is the wrong architecture in this tree (Exec
# format error). gn gen still emits build.ninja (we pin use_siso=false below), which
# plain ninja runs.
[ -d "$DEPOT" ] && export PATH="$DEPOT:$PATH"
command -v gn >/dev/null 2>&1 || {
  echo "error: 'gn' not found — need depot_tools to build Chromium." >&2
  echo "  expected $PARENT/depot_tools (sibling of $(basename "$HERE")/)," >&2
  echo "  or set DEPOT=/path/to/depot_tools (or --depot DIR)." >&2
  exit 1
}
NINJA="ninja"

OUT="out/mono-$MODE"
DEST="$CHROMIUM/third_party/blink/renderer/miniblink_host"
WKE_DEST="$CHROMIUM/third_party/blink/renderer/wke"
GN_PATH="third_party/blink/renderer/miniblink_host"

# Serialize builds that share this out dir. Two concurrent build-lib.sh runs would race —
# one's `rm -rf`/rsync staging + gn gen while the other is compiling, and two ninjas in the
# same dir corrupt .ninja_deps ("premature end of file"), which then forces a near-full
# rebuild. mkdir is atomic, so it's a portable lock; the trap frees it on any normal exit or
# signal. (If a run was SIGKILLed it may leave a stale lock — the message says how to clear.)
LOCK="$CHROMIUM/$OUT.build-lib.lock"
if ! mkdir "$LOCK" 2>/dev/null; then
  echo "error: another build-lib.sh is already building $OUT." >&2
  echo "  (lock: $LOCK — if no build is running, remove it: rmdir '$LOCK')" >&2
  exit 1
fi
trap 'rmdir "$LOCK" 2>/dev/null' EXIT INT TERM

# 1. Stage sources into the donor tree (same contract as build.sh) unless --no-stage.
if [ "$STAGE" = 1 ]; then
  echo "==> staging host + wke sources -> $DEST"
  # rsync, NOT rm -rf + cp -R: rsync preserves mtimes and copies ONLY changed files, so
  # ninja's incremental build reuses every unchanged .o. (cp -R stamps every file with a
  # fresh mtime, forcing ninja to recompile all ~60 host objects on every run.) Trailing
  # slashes sync directory CONTENTS; --delete prunes files removed from src.
  mkdir -p "$DEST" "$WKE_DEST"
  rsync -a --delete "$HERE/src/miniblink_host/" "$DEST/"
  rsync -a --delete "$HERE/src/wke/" "$WKE_DEST/"
  echo "==> applying blink compatibility patches"
  for p in "$HERE"/patches/*.patch; do
    [ -f "$p" ] || continue
    if git -C "$CHROMIUM" apply --reverse --check "$p" 2>/dev/null; then
      :  # already applied
    elif git -C "$CHROMIUM" apply "$p" 2>/dev/null; then
      echo "  applied $(basename "$p")"
    else
      echo "  ERROR: could not apply $(basename "$p") — aborting." >&2
      exit 1
    fi
  done
fi

# 2. Provision the NON-component out dir (the one switch that makes a single file).
if [ "$MODE" = debug ]; then IS_DEBUG=true; SYM=2; BSYM=1; else IS_DEBUG=false; SYM=1; BSYM=0; fi
[ "$WEBGPU" = 1 ] && USE_DAWN=true || USE_DAWN=false
[ "$VIDEO" = 1 ] && VID=true || VID=false        # video decoders (audio stays regardless)
[ "$ML" = 1 ] && MLV=true || MLV=false           # WebNN TFLite/LiteRT/XNNPACK backend
# --size-optimized: the size-optimized ship config. Release only (ThinLTO is a slow full-rebuild link;
# debug stays fast + keeps DCHECKs). The default build is a fast DEV release (-O2, no LTO, no
# dedup, DCHECKs on) — this trades build time for a much smaller dylib.
if [ "$SIZE" = 1 ] && [ "$MODE" != debug ]; then
  SIZE_ARGS='optimize_for_size = true          # -Oz/-Os instead of -O2/-O3
dcheck_always_on = false          # compile out every DCHECK assertion + its message string
use_thin_lto = true               # cross-module dead-code elimination + inlining + ICF
thin_lto_enable_optimizations = true
exclude_unwind_tables = true      # drop C++ unwind tables (no in-process crash backtraces)'
  SYM=0; BSYM=0                    # stripped anyway; 0 also speeds the LTO build
else
  SIZE_ARGS='# size-opt off — pass --size-optimized (release) for the small ThinLTO ship build'
fi
mkdir -p "$CHROMIUM/$OUT"
cat > "$CHROMIUM/$OUT/args.gn" <<EOF
is_debug = $IS_DEBUG
is_component_build = false        # single-file: statically link the whole engine
use_siso = false                  # siso binary is wrong-arch here; use ninja backend
use_dawn = $USE_DAWN              # WebGPU/Dawn — off unless --webgpu (Dawn native ~97MB).
                                  # Gates WebGPU out of BOTH the .dylib and the merged .a
                                  # (the static merge reads the actual link inputs).
skia_use_dawn = $USE_DAWN         # must track use_dawn (SKIA_USE_DAWN without USE_DAWN is a
                                  # compile #error); Skia falls back to Ganesh-over-GL, which
                                  # is what mb_gpu_thread already selects (gr_context_type=kGL).
# <video> decode — off by default (audio stays on); --video adds it. Keeps ffmpeg AUDIO
# decoders + the media pipeline; drops ffmpeg VIDEO decoders (H.264 etc.) via the cleanly
# gated enable_ffmpeg_video_decoders. NOTE: media_use_libvpx (VP8/9) is left ENABLED — the
# media decoder factory references media::VpxVideoDecoder's ctor at STARTUP, so disabling it
# only made the symbol undefined (a -U masked the link error, but dyld crashes at load).
# libvpx is shared with WebRTC, so keeping it costs ~nothing. AV1 (dav1d) is likewise left on
# (the macOS VideoToolbox AV1 accelerator is compiled unconditionally and needs libgav1).
enable_ffmpeg_video_decoders = $VID
# WebNN on-device ML (navigator.ml) — off by default; --ml adds the TFLite/LiteRT backend.
# Absent in miniblink49. Off drops most of //services/webnn's TFLite+XNNPACK (~79% of its
# input files); WebNN's LiteRT core still links a remainder. WebNN's mojo interface stays.
webnn_use_tflite = $MLV
webnn_use_litert = $MLV
build_tflite_with_xnnpack = $MLV
$SIZE_ARGS
symbol_level = $SYM
blink_symbol_level = $BSYM
use_system_xcode = true
clang_use_chrome_plugins = false
use_clang_modules = false
enable_precompiled_headers = false
mac_sdk_min = "26.0"
angle_enable_metal = false
EOF
echo "==> gn gen $OUT  (is_debug=$IS_DEBUG, is_component_build=false)"
( cd "$CHROMIUM" && gn gen "$OUT" >/dev/null )

# 3. Build the monolith shared library. It compiles every archive that BOTH the
#    .dylib and the merged .a need, so we always build it. First run compiles the
#    whole engine in non-component mode — slow; re-runs are incremental.
SHARED="$GN_PATH:miniblink2"
if [ "$PRINT_ONLY" = 1 ]; then
  echo "==> dry-run: ninja -n $SHARED"
  ( cd "$CHROMIUM" && ninja -C "$OUT" -n "$SHARED" >/dev/null && echo "  graph OK" )
  exit 0
fi
echo "==> $NINJA -C $OUT $SHARED   (first build is a full non-component compile — slow)"
( cd "$CHROMIUM" && "$NINJA" -C "$OUT" "$SHARED" )

# Runtime data the engine loads at startup is copied from a reference build (REF,
# default out/Release), NOT rebuilt: these are architecture/version-neutral data
# files, and building them in a fresh non-component out dir drags in a huge resource
# pipeline for no benefit. Verified compatible — the monolith's V8 (same chromium-150
# source + release flags) loads out/Release's v8_context_snapshot.
REF="${REF:-$CHROMIUM/out/Release}"

DIST="$HERE/dist/$MODE"
mkdir -p "$DIST/include/miniblink2"

# Public API headers — the SDK surface, grouped under one miniblink2/ namespace. The
# library exposes BOTH the mb_capi C API and the wke compatibility API; both headers
# are self-contained (standard-library includes only). Consumer:
#   -Idist/<mode>/include   +   #include "miniblink2/wke.h" / "miniblink2/mb_capi.h"
cp "$HERE/src/wke/wke.h"                     "$DIST/include/miniblink2/wke.h"
cp "$HERE/src/miniblink_host/capi/mb_capi.h" "$DIST/include/miniblink2/mb_capi.h"

# 4a. Shared deliverable: the self-contained dylib straight from the link.
if [ "$FORM" != static ]; then
  cp "$CHROMIUM/$OUT/libminiblink2.dylib" "$DIST/"
  # Release: strip local/debug symbols — keeps the exported mb*/wke* dynamic symbols
  # (so consumers still link), drops the ~120MB symbol table (~330MB -> ~210MB). Debug
  # keeps them for readable backtraces (MB_STACK_DUMP).
  if [ "$MODE" != debug ]; then
    strip -x "$DIST/libminiblink2.dylib" && echo "  stripped dylib (local symbols removed)"
  fi
fi

# 4b. Static deliverable: merge ALL the dylib's link inputs into one complete
#     libminiblink2.a. The exact set is the solink edge's $in (.a/.o) PLUS its
#     separate `rlibs =` variable (Chromium links Rust via that, not $in) PLUS the
#     force_loaded host archive. (The post-link .rsp is deleted by ninja, and GN's
#     static_library template won't forward complete_static_library, so we read the
#     edge directly.) libtool must run from $OUT — the paths are relative to it.
if [ "$FORM" != shared ]; then
  echo "==> merging link inputs -> libminiblink2.a"
  NJ="$CHROMIUM/$OUT/obj/$GN_PATH/miniblink2.ninja"
  LIST="$(mktemp)"
  # $in: everything after "solink", up to the first " |"
  grep ": solink " "$NJ" | head -1 | sed -E 's/^.*: solink //; s/ \|.*$//' \
    | tr ' ' '\n' | grep -E "\.(a|o)$" > "$LIST"
  # rlibs variable
  sed -n '/: solink /,/^$/p' "$NJ" | grep "^  rlibs =" | sed 's/^  rlibs = //' \
    | tr ' ' '\n' | grep -E "\.rlib$" >> "$LIST"
  # force_loaded host archive (in case it isn't already in $in)
  echo "obj/$GN_PATH/libminiblink_host.a" >> "$LIST"
  sort -u "$LIST" -o "$LIST"
  echo "  merging $(wc -l < "$LIST") inputs (rlibs: $(grep -c '\.rlib$' "$LIST"))"
  # Merge with llvm-ar via an MRI script: ADDLIB flattens each input archive's members,
  # ADDMOD adds loose objects, into ONE uniform archive. Apple `libtool` can't do this for a
  # --size-optimized build: its objects are ThinLTO BITCODE, and libtool emits a FAT archive
  # splitting bitcode (cputype 0) from native (arm64) objects, so an arm64 link sees only the
  # native slice and every wke*/mb* symbol comes up undefined. llvm-ar treats bitcode + native
  # uniformly, so the .a is consumable (scripts/build-samples.sh --static). Fall back to
  # libtool if llvm-ar is absent (a native, non-LTO .a links fine either way).
  LLVM_AR="$CHROMIUM/third_party/llvm-build/Release+Asserts/bin/llvm-ar"
  rm -f "$DIST/libminiblink2.a"
  if [ -x "$LLVM_AR" ]; then
    MRI="$(mktemp)"
    { echo "create $DIST/libminiblink2.a"
      while IFS= read -r m; do
        case "$m" in *.a|*.rlib) echo "addlib $m" ;; *.o) echo "addmod $m" ;; esac
      done < "$LIST"
      echo "save"; echo "end"; } > "$MRI"
    ( cd "$CHROMIUM/$OUT" && "$LLVM_AR" -M < "$MRI" )
    rm -f "$MRI"
  else
    ( cd "$CHROMIUM/$OUT" && libtool -static -o "$DIST/libminiblink2.a" -filelist "$LIST" 2>/dev/null )
  fi
  rm -f "$LIST"
fi

# 4c. Runtime data — copied from the reference build (see REF note above).
for f in blink_resources.pak icudtl.dat snapshot_blob.bin v8_context_snapshot.bin \
         v8_context_snapshot.arm64.bin media_controls_resources_100_percent.pak; do
  [ -f "$REF/$f" ] && cp "$REF/$f" "$DIST/"
done

# GL driver dylibs (ANGLE + SwiftShader) + the SwiftShader Vulkan ICD manifest. The engine
# dlopen's the GL implementation from the executable's own directory at runtime for WebGL /
# GPU-accelerated <canvas> — it is deliberately NOT statically linked (Chromium's standard GL
# loading). Built by the monolith in $OUT; the dylibs are self-contained (reference each other
# via @rpath). vk_swiftshader_icd.json is REQUIRED too: it's the Vulkan ICD manifest that tells
# the loader where libvk_swiftshader.dylib is (relative "./" path) and which surface extensions
# it provides — without it SwANGLE's eglInitialize fails the VK_KHR_surface/VK_EXT_metal_surface
# check and WebGL gets no display (2D canvas + layout/paint via Skia raster are unaffected).
for f in libEGL.dylib libGLESv2.dylib libvk_swiftshader.dylib vk_swiftshader_icd.json; do
  [ -f "$CHROMIUM/$OUT/$f" ] && cp "$CHROMIUM/$OUT/$f" "$DIST/"
done
# the engine loads "v8_context_snapshot.bin"; provide it if only the arch-suffixed exists
[ ! -f "$DIST/v8_context_snapshot.bin" ] && [ -f "$REF/v8_context_snapshot.arm64.bin" ] \
  && cp "$REF/v8_context_snapshot.arm64.bin" "$DIST/v8_context_snapshot.bin"

echo "==> done. dist tree ($DIST):"
( cd "$DIST" && find . -type f | sort | while read -r f; do
    printf '   %-7s %s\n' "$(ls -lh "$f" | awk '{print $5}')" "${f#./}"; done )
