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
# Runtime data the engine loads at startup is staged alongside the lib:
#   blink_resources.pak, icudtl.dat, snapshot_blob.bin, v8_context_snapshot.bin.
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

# Runtime data the engine loads at startup (best-effort; names vary by config).
echo "==> building runtime data files"
for f in blink_resources.pak icudtl.dat snapshot_blob.bin v8_context_snapshot.bin; do
  ( cd "$TREE" && "$NINJA" -C "$OUT" "$f" >/dev/null 2>&1 ) && echo "  built $f" || true
done

DIST="$HERE/dist/$MODE"
mkdir -p "$DIST"

# 4a. Shared deliverable: the self-contained dylib straight from the link.
if [ "$FORM" != static ]; then
  cp "$TREE/$OUT/libminiblink2.dylib" "$DIST/"
fi

# 4b. Static deliverable: merge every archive/object that fed the dylib link into
#     ONE complete libminiblink2.a (GN can't, so do it with libtool). Follows an
#     @response file if the link rule uses one.
if [ "$FORM" != shared ]; then
  echo "==> merging link archives -> libminiblink2.a"
  LINKCMD="$( cd "$TREE" && ninja -C "$OUT" -t commands "$SHARED" | tail -1 )"
  INPUTS="$LINKCMD"
  for tok in $LINKCMD; do
    case "$tok" in
      @*.rsp) rsp="${tok#@}"; [ -f "$TREE/$OUT/$rsp" ] && INPUTS="$INPUTS $(cat "$TREE/$OUT/$rsp")" ;;
    esac
  done
  ARCHIVES=()
  for tok in $INPUTS; do
    case "$tok" in
      -Wl,-force_load,*) f="${tok#-Wl,-force_load,}" ;;
      *.a|*.o)           f="$tok" ;;
      *) continue ;;
    esac
    [ -f "$TREE/$OUT/$f" ] && ARCHIVES+=("$TREE/$OUT/$f")
  done
  if [ "${#ARCHIVES[@]}" -eq 0 ]; then
    echo "  WARNING: found no input archives to merge (link rule format changed?)" >&2
  else
    echo "  merging ${#ARCHIVES[@]} archives/objects"
    libtool -static -o "$DIST/libminiblink2.a" "${ARCHIVES[@]}" 2>/dev/null
  fi
fi

# 4c. Runtime data.
for f in blink_resources.pak icudtl.dat snapshot_blob.bin v8_context_snapshot.bin; do
  [ -f "$TREE/$OUT/$f" ] && cp "$TREE/$OUT/$f" "$DIST/"
done

echo "==> done. dist: $DIST"
ls -lh "$DIST"
