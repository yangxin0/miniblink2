#!/usr/bin/env bash
# build-lib.sh — build the self-contained libminiblink2 SDK from the miniblink2 sources.
#
#   scripts/build-lib.sh [--release|--debug] [--ship]
#                        [--webgpu] [--video] [--ml] [--wasm] [--av1-encode]
#                        [--tracing] [--swiftshader] [--icu-full]
#                        [--chromium DIR] [--depot DIR] [--no-stage] [--print-only]
#
# Profiles (each with its own out dir — see the "one out dir per profile" note below):
#   --debug           debugger build: no optimization, full symbols, DCHECKs.
#                     -> dist/debug/libminiblink2.dylib          (out/mono-debug)
#   --release         dev release: -O2, DCHECK assertions COMPILED IN (Chromium's
#                     developer convention — fast, but internal bugs abort loudly).
#                     -> dist/release/libminiblink2.{dylib,a}    (out/mono-release)
#   --release --ship  the publishable SDK. Each artifact gets the strategy that makes
#                     IT smallest; both are -Oz, DCHECKs compiled out, stripped:
#                       dylib: ThinLTO (whole-program opt at OUR link)
#                                                        (out/mono-release-dynamic)
#                       .a:    native machine code, NO ThinLTO — an archive never
#                              goes through our linker, so bitcode would just bloat
#                              it 8x and force lld on consumers; native -Oz links
#                              with ANY toolchain          (out/mono-release-static)
#
# Feature flags are include-only (default OFF, to trim toward miniblink49's footprint):
#   --webgpu  WebGPU/Dawn      --video  <video> decode (audio is always on)
#   --ml      WebNN on-device ML (TFLite/LiteRT/XNNPACK backend)
#   --wasm    WebAssembly (V8 wasm engine: Liftoff + wasm TurboFan + builtins)
#   --av1-encode  AV1 encoding via libaom (WebCodecs/MediaRecorder/WebRTC send; decode stays)
#   --tracing  OPTIONAL_TRACE_EVENT instrumentation (extra trace-event coverage + strings)
#   --swiftshader  ship SwiftShader software Vulkan in dist/ (headless/CI/no-GPU fallback)
#   --icu-full  ship the untrimmed icudtl.dat (all ~90 locales, 10.4MB); default trims
#               to root+en+zh (6.3MB) — override the keep list with MB_ICU_KEEP=en,zh,ja
#
# CHROMIUM (the Chromium checkout) and DEPOT (depot_tools) default to siblings of this
# project; override with env vars (CHROMIUM=… DEPOT=…) or the --chromium/--depot flags.
#
# Unlike the default build (is_component_build=true), this links the WHOLE engine
# (Blink, V8, skia, base, ...) statically into ONE artifact instead of ~286 sibling
# component dylibs. The first build of a given profile is a full from-scratch compile
# (slow); re-runs are incremental.
#
# Each dist/<mode>/ is a self-contained SDK:
#   libminiblink2.{dylib,a}            the library (exposes the miniblink2 mb* C API)
#   include/miniblink2/{view,automation}.h    the public headers
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
MODE=release         # release | debug
SHIP=0               # --ship: publishable artifacts (release only) — see header
STAGE=1
PRINT_ONLY=0
WEBGPU=0             # WebGPU (Dawn) OFF by default (smaller lib); --webgpu links it in
VIDEO=0              # <video> decode OFF by default (audio stays on); --video adds it
ML=0                 # WebNN on-device ML (TFLite/LiteRT/XNNPACK) OFF by default; --ml adds it
WASM=0               # WebAssembly OFF by default (window.WebAssembly absent); --wasm adds it
AV1ENC=0             # AV1 encoding (libaom) OFF by default; --av1-encode adds it
TRACING=0            # OPTIONAL_TRACE_EVENT macros OFF by default; --tracing adds them
SWIFTSHADER=0        # SwiftShader NOT shipped in dist by default; --swiftshader adds it
ICUFULL=0            # icudtl.dat trimmed to root+en+zh by default; --icu-full ships all locales

while [ $# -gt 0 ]; do
  case "$1" in
    --release) MODE=release ;;
    --debug)   MODE=debug ;;
    --ship) SHIP=1 ;;               # publishable SDK (release only): -Oz, no DCHECKs, stripped
    --chromium) CHROMIUM="$2"; shift ;;
    --depot) DEPOT="$2"; shift ;;
    --no-stage) STAGE=0 ;;          # skip the source sync (sources already staged)
    --print-only) PRINT_ONLY=1 ;;   # gn gen + ninja dry-run, no compile
    --webgpu) WEBGPU=1 ;;           # include WebGPU (Dawn ~97MB); default excludes it
    --video) VIDEO=1 ;;             # include <video> decode (ffmpeg video + AV1); default off
    --ml) ML=1 ;;                   # include WebNN on-device ML (TFLite backend); default off
    --wasm) WASM=1 ;;               # include WebAssembly (V8 wasm engine); default off
    --av1-encode) AV1ENC=1 ;;       # include AV1 encoding (libaom); default off
    --tracing) TRACING=1 ;;         # include OPTIONAL_TRACE_EVENT coverage; default off
    --swiftshader) SWIFTSHADER=1 ;; # ship SwiftShader software Vulkan in dist; default off
    --icu-full) ICUFULL=1 ;;        # ship untrimmed icudtl.dat (all locales); default trims
    --shared|--static|--both|--size-optimized)
      echo "build-lib.sh: '$1' was removed. Every run builds the profile's artifacts:" >&2
      echo "  --release        -> dev dylib + dev .a" >&2
      echo "  --release --ship -> ThinLTO dylib + native -Oz .a (the publishable SDK)" >&2
      echo "  --debug          -> debug dylib" >&2
      exit 2 ;;
    -h|--help) sed -n '2,48p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "build-lib.sh: unknown arg '$1'" >&2; exit 2 ;;
  esac
  shift
done

if [ "$SHIP" = 1 ] && [ "$MODE" = debug ]; then
  echo "build-lib.sh: --ship applies to --release only (a debug build is not a ship artifact)." >&2
  exit 2
fi

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

DEST="$CHROMIUM/third_party/blink/renderer/miniblink_host"
MB2_DEST="$CHROMIUM/third_party/blink/renderer/miniblink2"
CURL_DEST="$CHROMIUM/third_party/blink/renderer/miniblink2_curl"
GN_PATH="third_party/blink/renderer/miniblink_host"
SHARED="$GN_PATH:miniblink2"
DIST="$HERE/dist/$MODE"

# One out dir PER PROFILE: profile flags change every compile command (-Oz/ThinLTO/
# no-DCHECK vs -O2/DCHECK), and ninja keys rebuilds on the command hash — sharing one
# dir makes every profile switch a full ~28k-object recompile even though all the .o
# files are present. A dedicated dir keeps each profile incremental; the cost is disk
# (~10GB per tree). --ship uses TWO dirs because its dylib (ThinLTO bitcode objects)
# and its .a (native -Oz objects) cannot come from the same compilation.

# Serialize builds per out dir. Two concurrent builds in one dir would race —
# staging/gn gen vs compile, and two ninjas corrupt .ninja_deps ("premature end of
# file"), forcing a near-full rebuild. mkdir is atomic (portable lock); the trap
# frees every acquired lock on exit or signal. (A SIGKILLed run may leave a stale
# lock — the message says how to clear it.)
LOCKS=""
release_locks() { for l in $LOCKS; do rmdir "$l" 2>/dev/null; done; }
trap release_locks EXIT INT TERM
acquire_lock() {  # $1 = out dir (relative to $CHROMIUM)
  local lock="$CHROMIUM/$1.build-lib.lock"
  if ! mkdir "$lock" 2>/dev/null; then
    echo "error: another build-lib.sh is already building $1." >&2
    echo "  (lock: $lock — if no build is running, remove it: rmdir '$lock')" >&2
    exit 1
  fi
  LOCKS="$LOCKS $lock"
}

# 1. Stage sources into the donor tree (same contract as build.sh) unless --no-stage.
if [ "$STAGE" = 1 ]; then
  echo "==> staging host + miniblink2 API sources -> $DEST"
  # rsync, NOT rm -rf + cp -R: rsync preserves mtimes and copies ONLY changed files, so
  # ninja's incremental build reuses every unchanged .o. (cp -R stamps every file with a
  # fresh mtime, forcing ninja to recompile all ~60 host objects on every run.) Trailing
  # slashes sync directory CONTENTS; --delete prunes files removed from src.
  mkdir -p "$DEST" "$MB2_DEST" "$CURL_DEST"
  rsync -a --delete "$HERE/src/miniblink_host/" "$DEST/"
  rsync -a --delete "$HERE/src/miniblink2/" "$MB2_DEST/"
  # Vendored curl SDK (headers + dylib chain) staged like the sources, so
  # BUILD.gn references it RELATIVELY — no absolute repo path in build files.
  # -a keeps the libcurl.dylib symlink.
  rsync -a --delete "$HERE/third_party/curl/include" "$HERE/third_party/curl/lib" "$CURL_DEST/"
  # Retarget the STAGED libcurl's install id to THIS repo's copy. The id baked
  # into the tracked binary may be stale (folder renames), and the tracked file
  # must never be modified (no binary churn in git). The linker records this id
  # as the runtime load path of every binary it links, so dev builds always
  # point at wherever the repo currently lives; package.sh rewrites it to
  # @loader_path for distribution. Re-sign: install_name_tool invalidates the
  # ad-hoc signature and dyld kills invalidly-signed arm64 images at load.
  install_name_tool -id "$HERE/third_party/curl/lib/libcurl.4.dylib" \
      "$CURL_DEST/lib/libcurl.4.dylib" 2>/dev/null
  codesign --force --sign - "$CURL_DEST/lib/libcurl.4.dylib" 2>/dev/null
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

# Feature-flag GN values (shared by every pass).
[ "$WEBGPU" = 1 ] && USE_DAWN=true || USE_DAWN=false
[ "$VIDEO" = 1 ] && VID=true || VID=false        # video decoders (audio stays regardless)
[ "$ML" = 1 ] && MLV=true || MLV=false           # WebNN TFLite/LiteRT/XNNPACK backend
[ "$WASM" = 1 ] && WASMV=true || WASMV=false     # V8 WebAssembly engine
[ "$AV1ENC" = 1 ] && AOMV=true || AOMV=false     # libaom AV1 encoder
[ "$TRACING" = 1 ] && TRACEV=true || TRACEV=false # OPTIONAL_TRACE_EVENT macros

# write_args OUT THINLTO — emit args.gn for one pass.
#   Profile args: debug = symbols+DCHECKs; dev release = -O2, DCHECKs ON, light symbols;
#   ship = -Oz + DCHECKs compiled out + no unwind tables (+ ThinLTO for the dylib pass).
write_args() {
  local out="$1" thinlto="$2"
  local is_debug sym bsym profile_args
  if [ "$MODE" = debug ]; then
    is_debug=true; sym=2; bsym=1
    profile_args='# debug profile: assertions + full symbols (see is_debug/symbol_level)'
  elif [ "$SHIP" = 1 ]; then
    is_debug=false; sym=0; bsym=0   # stripped anyway; 0 also speeds the (LTO) link
    profile_args='optimize_for_size = true          # -Oz/-Os instead of -O2/-O3
dcheck_always_on = false          # compile out every DCHECK assertion + its message string
exclude_unwind_tables = true      # drop C++ unwind tables (no in-process crash backtraces)'
    if [ "$thinlto" = 1 ]; then
      profile_args="$profile_args
# ThinLTO for the DYLIB pass only: whole-program dead-code elimination + inlining +
# ICF happen at OUR solink, so the shipped dylib is smallest. The STATIC pass omits
# this on purpose — an archive never goes through our linker, so bitcode objects
# would bloat it ~8x and force an LTO-capable linker (lld) on every consumer.
use_thin_lto = true
thin_lto_enable_optimizations = true"
    fi
  else
    is_debug=false; sym=1; bsym=0
    profile_args='# dev release: -O2, DCHECK assertions compiled IN (Chromium developer default) —
# near-ship speed, but internal engine bugs abort with a message instead of
# corrupting. Use --ship for the publishable artifacts.'
  fi
  mkdir -p "$CHROMIUM/$out"
  cat > "$CHROMIUM/$out/args.gn" <<EOF
is_debug = $is_debug
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
# input files); WebNN's mojo interface stays. webnn_use_tflite doubles as the umbrella
# on-device-ML switch: patches/0019 swaps the TFLite language-detection model (Blink's
# LanguageDetector / translation API backend — the last renderer TFLite user, ~1MB of
# tflite + tflite_support + tflite_kernels) for an always-unavailable stub when off.
webnn_use_tflite = $MLV
webnn_use_litert = $MLV
build_tflite_with_xnnpack = $MLV
# WebAssembly — off by default (~3-5MB of V8: Liftoff, wasm TurboFan pipeline, wasm
# builtins); --wasm adds it. Safe to disable: V8 keeps the whole external Wasm API as
# UNREACHABLE()/no-op stubs when off (api.cc "#if !V8_ENABLE_WEBASSEMBLY" block), so
# Blink's unconditional references (WasmStreaming, WasmModuleObject, SetWasm*Callback)
# still link; window.WebAssembly is simply absent at runtime. Absent in miniblink49 too.
v8_enable_webassembly = $WASMV
# AV1 ENCODING (libaom, ~1MB incl. av1/aom asm) — off by default; --av1-encode adds it.
# Only feeds WebCodecs VideoEncoder, MediaRecorder and WebRTC AV1 *send*; AV1 DECODE is
# untouched (dav1d + the unconditional VideoToolbox/libgav1 path stay in — see the
# enable_ffmpeg_video_decoders note above). Cleanly GN-gated in media/, webcodecs/ and
# webrtc/modules/video_coding/codecs/av1/.
enable_libaom = $AOMV
# TRACING — the perfetto client library itself is mandatory in M150 base (the old
# enable_base_tracing off-switch is gone), but OPTIONAL_TRACE_EVENT macros are a
# documented size knob (Chromium ships Android/ChromeOS with them off). Off drops that
# instrumentation + its string literals; --tracing restores full trace coverage for
# perf work. Trimming perfetto core further would need patches (Tier 2).
optional_trace_events_enabled = $TRACEV
$profile_args
symbol_level = $sym
blink_symbol_level = $bsym
use_system_xcode = true
clang_use_chrome_plugins = false
use_clang_modules = false
enable_precompiled_headers = false
mac_sdk_min = "26.0"
angle_enable_metal = true          # ANGLE Metal (hardware GPU) backend — needs full Xcode (`xcrun metal`
                                  # compiles .metal->.air). With only CommandLineTools this must
                                  # be false (SwiftShader software fallback). true = GPU WebGL.
EOF
}

# merge_static OUT — merge ALL the dylib's link inputs in $OUT into one complete
# dist/libminiblink2.a. The exact set is the solink edge's \$in (.a/.o) PLUS its
# separate `rlibs =` variable (Chromium links Rust via that, not \$in) PLUS the
# force_loaded host archive. (The post-link .rsp is deleted by ninja, and GN's
# static_library template won't forward complete_static_library, so we read the
# edge directly.) The merge tool must run from $OUT — paths are relative to it.
merge_static() {
  local out="$1"
  echo "==> merging link inputs -> libminiblink2.a"
  local nj="$CHROMIUM/$out/obj/$GN_PATH/miniblink2.ninja"
  local list; list="$(mktemp)"
  # $in: everything after "solink", up to the first " |"
  grep ": solink " "$nj" | head -1 | sed -E 's/^.*: solink //; s/ \|.*$//' \
    | tr ' ' '\n' | grep -E "\.(a|o)$" > "$list"
  # rlibs variable
  sed -n '/: solink /,/^$/p' "$nj" | grep "^  rlibs =" | sed 's/^  rlibs = //' \
    | tr ' ' '\n' | grep -E "\.rlib$" >> "$list"
  # force_loaded host archive (in case it isn't already in $in)
  echo "obj/$GN_PATH/libminiblink_host.a" >> "$list"
  sort -u "$list" -o "$list"
  echo "  merging $(wc -l < "$list") inputs (rlibs: $(grep -c '\.rlib$' "$list"))"
  # Merge with llvm-ar via an MRI script: ADDLIB flattens each input archive's members,
  # ADDMOD adds loose objects, into ONE uniform archive. (llvm-ar also handles bitcode
  # members uniformly if a ThinLTO .a is ever wanted; Apple libtool would split
  # bitcode/native into fat slices the linker can't use.) Fall back to libtool if
  # llvm-ar is absent (a native .a links fine either way).
  local llvm_ar="$CHROMIUM/third_party/llvm-build/Release+Asserts/bin/llvm-ar"
  rm -f "$DIST/libminiblink2.a"
  if [ -x "$llvm_ar" ]; then
    local mri; mri="$(mktemp)"
    { echo "create $DIST/libminiblink2.a"
      while IFS= read -r m; do
        case "$m" in *.a|*.rlib) echo "addlib $m" ;; *.o) echo "addmod $m" ;; esac
      done < "$list"
      echo "save"; echo "end"; } > "$mri"
    ( cd "$CHROMIUM/$out" && "$llvm_ar" -M < "$mri" )
    rm -f "$mri"
  else
    ( cd "$CHROMIUM/$out" && libtool -static -o "$DIST/libminiblink2.a" -filelist "$list" 2>/dev/null )
  fi
  rm -f "$list"
  if [ "$SHIP" = 1 ]; then
    # Ship archive: strip local symbols (the miniblink49 recipe — strip -x keeps every
    # external symbol + anything referenced by a relocation, so the archive stays
    # statically linkable). Native objects only; a bitcode .a can't be stripped.
    echo "==> stripping local symbols from libminiblink2.a"
    strip -x "$DIST/libminiblink2.a"
  fi
}

# build_pass OUT THINLTO WANT_DYLIB WANT_A WANT_SNAPSHOT — provision + build one out
# dir and collect its artifacts into dist.
build_pass() {
  local out="$1" thinlto="$2" want_dylib="$3" want_a="$4" want_snapshot="$5"
  acquire_lock "$out"
  write_args "$out" "$thinlto"
  echo "==> gn gen $out"
  ( cd "$CHROMIUM" && gn gen "$out" >/dev/null )
  if [ "$PRINT_ONLY" = 1 ]; then
    echo "==> dry-run: ninja -n $SHARED  ($out)"
    ( cd "$CHROMIUM" && ninja -C "$out" -n "$SHARED" >/dev/null && echo "  graph OK" )
    return 0
  fi
  # The dylib target is built in EVERY pass — even a static-only pass needs its solink
  # edge (the merge reads the actual link inputs from it). v8_context_snapshot.arm64.bin
  # is built where dist data comes from (not taken from REF): V8 snapshots are
  # BUILD-FLAG-DEPENDENT — the serialized-data magic embeds V8's flag configuration, so
  # a snapshot from a differently-flagged build SIGTRAPs the engine at mbInitialize.
  local targets=("$SHARED")
  [ "$want_snapshot" = 1 ] && targets+=(v8_context_snapshot.arm64.bin)
  echo "==> $NINJA -C $out ${targets[*]}   (first build of a profile is a full compile — slow)"
  ( cd "$CHROMIUM" && "$NINJA" -C "$out" "${targets[@]}" )

  if [ "$want_dylib" = 1 ]; then
    cp "$CHROMIUM/$out/libminiblink2.dylib" "$DIST/"
    # Release/ship: strip local/debug symbols — keeps the exported mb* dynamic symbols
    # (so consumers still link), drops the local-symbol table. Debug keeps them for
    # readable backtraces (MB_STACK_DUMP).
    if [ "$MODE" != debug ]; then
      strip -x "$DIST/libminiblink2.dylib" && echo "  stripped dylib (local symbols removed)"
    fi
  fi
  [ "$want_a" = 1 ] && merge_static "$out"
  return 0
}

mkdir -p "$DIST/include/miniblink2"

# Public API header — the SDK surface: the miniblink2 mb* C API, self-contained
# (standard-library includes only). Consumer:
#   -Idist/<mode>/include   +   #include "miniblink2/miniblink2.h"
cp "$HERE"/src/miniblink2/*.h "$DIST/include/miniblink2/"
rm -f "$DIST/include/miniblink2/wke.h" "$DIST/include/miniblink2/mb_capi.h"  # pre-rename leftovers

# 2+3. Run the profile's pass(es). DATA_OUT is where runtime data (V8 snapshots,
# ANGLE dylibs) is collected from afterwards.
if [ "$MODE" = debug ]; then
  #                 out             thinlto dylib a  snapshot
  build_pass        out/mono-debug  0       1     0  1
  DATA_OUT=out/mono-debug
elif [ "$SHIP" = 1 ]; then
  build_pass        out/mono-release-dynamic 1     1 0 1
  build_pass        out/mono-release-static  0     0 1 0
  DATA_OUT=out/mono-release-dynamic
else
  build_pass        out/mono-release 0      1     1  1
  DATA_OUT=out/mono-release
fi
[ "$PRINT_ONLY" = 1 ] && exit 0

# Flag-NEUTRAL runtime data (resource paks, ICU data) is copied from a reference build
# (REF, default out/Release), NOT rebuilt: building those in a fresh non-component out
# dir drags in a huge resource pipeline for no benefit. The V8 snapshots are NOT taken
# from REF — see the flag-dependence note in build_pass; they come from $DATA_OUT.
REF="${REF:-$CHROMIUM/out/Release}"

# 4. Runtime data.
for f in blink_resources.pak icudtl.dat media_controls_resources_100_percent.pak; do
  [ -f "$REF/$f" ] && cp "$REF/$f" "$DIST/"
done
for f in snapshot_blob.bin v8_context_snapshot.bin v8_context_snapshot.arm64.bin; do
  [ -f "$CHROMIUM/$DATA_OUT/$f" ] && cp "$CHROMIUM/$DATA_OUT/$f" "$DIST/"
done
# ICU locale trim (10.4MB -> ~6.3MB): drop per-locale collation/display-name bundles for
# locales outside MB_ICU_KEEP (default root+en+zh). ICU falls back to root for a missing
# bundle, so other-locale pages still render — they lose locale-tailored Intl output only.
# brkitr (incl. the CJK segmentation dictionary), converters, normalization and the
# res_index/pool infrastructure are always kept. --icu-full ships the whole file instead.
if [ "$ICUFULL" = 0 ] && [ -f "$REF/icudtl.dat" ]; then
  python3 "$HERE/scripts/trim_icu.py" --keep "${MB_ICU_KEEP:-en,zh}" \
    "$REF/icudtl.dat" "$DIST/icudtl.dat"
fi

# GL driver dylibs (ANGLE, + SwiftShader if --swiftshader) + the SwiftShader Vulkan ICD
# manifest. The engine dlopen's the GL implementation from the executable's own directory at
# runtime for WebGL / GPU-accelerated <canvas> — it is deliberately NOT statically linked
# (Chromium's standard GL loading). Built by the monolith in $DATA_OUT; the dylibs are
# self-contained (reference each other via @rpath).
GL_DYLIBS="libEGL.dylib libGLESv2.dylib"
if [ "$SWIFTSHADER" = 1 ]; then
  # SwiftShader = the --use-angle=swiftshader software fallback (headless/CI/no-GPU; also
  # Dawn's WebGPU fallback adapter). ~20MB. The default Metal path never loads it, so the
  # shipped SDK omits it unless --swiftshader. vk_swiftshader_icd.json is REQUIRED alongside:
  # it's the Vulkan ICD manifest that tells the loader where libvk_swiftshader.dylib is
  # (relative "./" path) and which surface extensions it provides — without it SwANGLE's
  # eglInitialize fails the VK_KHR_surface/VK_EXT_metal_surface check and WebGL gets no
  # display (2D canvas + layout/paint via Skia raster are unaffected).
  GL_DYLIBS="$GL_DYLIBS libvk_swiftshader.dylib vk_swiftshader_icd.json"
else
  rm -f "$DIST/libvk_swiftshader.dylib" "$DIST/vk_swiftshader_icd.json"  # drop stale copies
fi
for f in $GL_DYLIBS; do
  [ -f "$CHROMIUM/$DATA_OUT/$f" ] || continue
  cp "$CHROMIUM/$DATA_OUT/$f" "$DIST/"
  # Release: strip local symbols from the GL driver dylibs too (same rationale as
  # the engine dylib above) — libGLESv2 alone carries ~51k locals, 18.3MB -> 11.9MB.
  case "$f" in *.dylib)
    [ "$MODE" != debug ] && strip -x "$DIST/$f" && echo "  stripped $f" ;;
  esac
done
# the engine loads "v8_context_snapshot.bin"; derive it from the arch-suffixed file
# just copied from $DATA_OUT. Unconditional overwrite: dist may hold a STALE plain-named
# copy from an earlier differently-flagged build (V8 snapshots are flag-dependent).
[ -f "$DIST/v8_context_snapshot.arm64.bin" ] \
  && cp "$DIST/v8_context_snapshot.arm64.bin" "$DIST/v8_context_snapshot.bin"

echo "==> done. dist tree ($DIST):"
( cd "$DIST" && find . -type f | sort | while read -r f; do
    printf '   %-7s %s\n' "$(ls -lh "$f" | awk '{print $5}')" "${f#./}"; done )
