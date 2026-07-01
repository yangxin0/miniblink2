#!/usr/bin/env bash
# build-lib.sh — build a self-contained libminiblink2 (single .dylib and/or .a),
# release or debug, from the miniblink-modern sources.
#
#   tools/build-lib.sh [--shared|--static|--both] [--release|--debug]
#                      [--tree DIR] [--no-stage] [--print-only]
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

HERE="$(cd "$(dirname "$0")/.." && pwd)"   # repo root (this script lives in tools/)
TREE="${TREE:-/Users/yangxin/dennis/chrome/chromium-150.0.7871.24}"
FORM=shared          # shared | static | both
MODE=release         # release | debug
STAGE=1
PRINT_ONLY=0

while [ $# -gt 0 ]; do
  case "$1" in
    --shared) FORM=shared ;;
    --static) FORM=static ;;
    --both)   FORM=both ;;
    --release) MODE=release ;;
    --debug)   MODE=debug ;;
    --tree) TREE="$2"; shift ;;
    --no-stage) STAGE=0 ;;          # skip the source sync (sources already staged)
    --print-only) PRINT_ONLY=1 ;;   # gn gen + ninja dry-run, no compile
    -h|--help) sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "build-lib.sh: unknown arg '$1'" >&2; exit 2 ;;
  esac
  shift
done

[ -d "$TREE/third_party/blink/renderer" ] || { echo "not a chromium tree: $TREE" >&2; exit 1; }

# Make gn/ninja resolvable whether run from a login shell or a script. Use plain
# ninja, NOT autoninja: autoninja auto-selects the siso backend, whose vendored
# binary is the wrong architecture in this tree (Exec format error). gn gen still
# emits build.ninja (we also pin use_siso=false below), which plain ninja runs.
DEPOT="$(cd "$TREE/.." && pwd)/depot_tools"
[ -d "$DEPOT" ] && export PATH="$DEPOT:$PATH"
NINJA="ninja"

OUT="out/mono-$MODE"
DEST="$TREE/third_party/blink/renderer/miniblink_host"
WKE_DEST="$TREE/third_party/blink/renderer/wke"
GN_PATH="third_party/blink/renderer/miniblink_host"

# 1. Stage sources into the donor tree (same contract as build.sh) unless --no-stage.
if [ "$STAGE" = 1 ]; then
  echo "==> staging host + wke sources -> $DEST"
  rm -rf "$DEST" "$WKE_DEST"
  cp -R "$HERE/src/miniblink_host" "$DEST"
  cp -R "$HERE/src/wke" "$WKE_DEST"
  echo "==> applying blink compatibility patches"
  for p in "$HERE"/patches/*.patch; do
    [ -f "$p" ] || continue
    if git -C "$TREE" apply --reverse --check "$p" 2>/dev/null; then
      :  # already applied
    elif git -C "$TREE" apply "$p" 2>/dev/null; then
      echo "  applied $(basename "$p")"
    else
      echo "  ERROR: could not apply $(basename "$p") — aborting." >&2
      exit 1
    fi
  done
fi

# 2. Provision the NON-component out dir (the one switch that makes a single file).
if [ "$MODE" = debug ]; then IS_DEBUG=true; SYM=2; BSYM=1; else IS_DEBUG=false; SYM=1; BSYM=0; fi
mkdir -p "$TREE/$OUT"
cat > "$TREE/$OUT/args.gn" <<EOF
is_debug = $IS_DEBUG
is_component_build = false        # single-file: statically link the whole engine
use_siso = false                  # siso binary is wrong-arch here; use ninja backend
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
( cd "$TREE" && gn gen "$OUT" >/dev/null )

# 3. Build the monolith shared library. It compiles every archive that BOTH the
#    .dylib and the merged .a need, so we always build it. First run compiles the
#    whole engine in non-component mode — slow; re-runs are incremental.
SHARED="$GN_PATH:miniblink2"
if [ "$PRINT_ONLY" = 1 ]; then
  echo "==> dry-run: ninja -n $SHARED"
  ( cd "$TREE" && ninja -C "$OUT" -n "$SHARED" >/dev/null && echo "  graph OK" )
  exit 0
fi
echo "==> $NINJA -C $OUT $SHARED   (first build is a full non-component compile — slow)"
( cd "$TREE" && "$NINJA" -C "$OUT" "$SHARED" )

# Runtime data the engine loads at startup is copied from a reference build (REF,
# default out/Release), NOT rebuilt: these are architecture/version-neutral data
# files, and building them in a fresh non-component out dir drags in a huge resource
# pipeline for no benefit. Verified compatible — the monolith's V8 (same chromium-150
# source + release flags) loads out/Release's v8_context_snapshot.
REF="${REF:-$TREE/out/Release}"

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
  cp "$TREE/$OUT/libminiblink2.dylib" "$DIST/"
fi

# 4b. Static deliverable: merge ALL the dylib's link inputs into one complete
#     libminiblink2.a. The exact set is the solink edge's $in (.a/.o) PLUS its
#     separate `rlibs =` variable (Chromium links Rust via that, not $in) PLUS the
#     force_loaded host archive. (The post-link .rsp is deleted by ninja, and GN's
#     static_library template won't forward complete_static_library, so we read the
#     edge directly.) libtool must run from $OUT — the paths are relative to it.
if [ "$FORM" != shared ]; then
  echo "==> merging link inputs -> libminiblink2.a"
  NJ="$TREE/$OUT/obj/$GN_PATH/miniblink2.ninja"
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
  ( cd "$TREE/$OUT" && libtool -static -o "$DIST/libminiblink2.a" -filelist "$LIST" 2>/dev/null )
  rm -f "$LIST"
fi

# 4c. Runtime data — copied from the reference build (see REF note above).
for f in blink_resources.pak icudtl.dat snapshot_blob.bin v8_context_snapshot.bin \
         v8_context_snapshot.arm64.bin media_controls_resources_100_percent.pak; do
  [ -f "$REF/$f" ] && cp "$REF/$f" "$DIST/"
done
# the engine loads "v8_context_snapshot.bin"; provide it if only the arch-suffixed exists
[ ! -f "$DIST/v8_context_snapshot.bin" ] && [ -f "$REF/v8_context_snapshot.arm64.bin" ] \
  && cp "$REF/v8_context_snapshot.arm64.bin" "$DIST/v8_context_snapshot.bin"

echo "==> done. dist tree ($DIST):"
( cd "$DIST" && find . -type f | sort | while read -r f; do
    printf '   %-7s %s\n' "$(ls -lh "$f" | awk '{print $5}')" "${f#./}"; done )
