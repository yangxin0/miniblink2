#!/usr/bin/env bash
# package.sh — package the miniblink2 macOS SDK from dist/<mode>/ into a zip,
# the miniblink49 tools/package-macos.sh equivalent (lib/ + include/ + README).
#
#   scripts/package.sh [--release|--debug] [--dynamic|--static|--both] [--out ZIP]
#
# Packages what scripts/build-lib.sh already put in dist/<mode>/ (build first!):
#   <name>/lib/        libminiblink2.dylib + ANGLE GL dylibs + the vendored libcurl
#                      chain [dynamic/both]; libminiblink2.a [static/both]
#   <name>/include/miniblink2/miniblink2.h
#   <name>/resources/  blink_resources.pak, icudtl.dat, V8 snapshots, media pak —
#                      the engine loads these from the EXECUTABLE's directory
#   <name>/README.md   generated link + runtime instructions
#
# Unlike miniblink49 (system libicucore/libxml2/libcurl), this engine bundles its
# data and networking, so the package must carry the runtime files and the curl
# chain — and must be PORTABLE. Fixups applied to the STAGED copies only (dist/
# itself is untouched):
#   - libminiblink2.dylib references the vendored curl by ABSOLUTE build-machine
#     path -> rewritten to @loader_path/libcurl.4.dylib (the rest of the curl
#     chain already uses @loader_path internally, from build-curl-macos.sh)
#   - libminiblink2.dylib install id "./libminiblink2.dylib" -> @rpath/…
#   - ad-hoc re-sign of every modified Mach-O (install_name_tool invalidates the
#     arm64 code signature; an unsigned/invalid dylib is killed at load on macOS)
#
# The dylib and (--ship) .a in dist/ are already stripped by build-lib.sh; the
# .a is strip -x'd here too when it contains native objects (harmless if already
# stripped). A LEGACY ThinLTO bitcode .a (pre---ship builds) can't be stripped or
# linked by plain clang+ld64 — the generated README gets lld instructions then.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
MODE=release
KIND=dynamic          # dynamic | static | both — static adds the multi-GB .a
OUT=""

while [ $# -gt 0 ]; do
  case "$1" in
    --release) MODE=release ;;
    --debug)   MODE=debug ;;
    --dynamic) KIND=dynamic ;;
    --static)  KIND=static ;;
    --both)    KIND=both ;;
    --out) OUT="$2"; shift ;;
    -h|--help) sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "package.sh: unknown arg '$1'" >&2; exit 2 ;;
  esac
  shift
done

WANT_DYNAMIC=false; WANT_STATIC=false
case "$KIND" in
  dynamic) WANT_DYNAMIC=true; SUFFIX="-dynamic" ;;
  static)  WANT_STATIC=true;  SUFFIX="-static" ;;
  both)    WANT_DYNAMIC=true; WANT_STATIC=true; SUFFIX="" ;;
esac

DIST="$HERE/dist/$MODE"
CURL_LIB="$HERE/third_party/curl/lib"
[ -f "$DIST/include/miniblink2/miniblink2.h" ] || {
  echo "error: no SDK in $DIST — build it first: scripts/build-lib.sh --$MODE" >&2; exit 1; }
if [ "$WANT_DYNAMIC" = true ] && [ ! -f "$DIST/libminiblink2.dylib" ]; then
  echo "error: no $DIST/libminiblink2.dylib — build it: scripts/build-lib.sh --shared --$MODE" >&2; exit 1
fi
if [ "$WANT_STATIC" = true ] && [ ! -f "$DIST/libminiblink2.a" ]; then
  echo "error: no $DIST/libminiblink2.a — build it: scripts/build-lib.sh --static --$MODE" >&2; exit 1
fi

NAME="miniblink2-macos-arm64-$MODE$SUFFIX"
[ -n "$OUT" ] || OUT="$HERE/dist/$NAME.zip"
mkdir -p "$(dirname "$OUT")"
OUT="$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")"

STAGE_ROOT="$(mktemp -d)"
trap 'rm -rf "$STAGE_ROOT"' EXIT INT TERM
STAGE="$STAGE_ROOT/$NAME"
mkdir -p "$STAGE/lib" "$STAGE/include/miniblink2" "$STAGE/resources"

# --- lib/ -------------------------------------------------------------------
# The vendored curl chain ships with BOTH kinds: the dylib needs it at load time
# (@loader_path), and a static consumer links -lcurl against the same chain.
echo "==> [$MODE] staging vendored libcurl chain"
for f in libcurl.4.dylib libnghttp2.14.dylib libidn2.0.dylib libssl.3.dylib \
         libcrypto.3.dylib libintl.8.dylib libunistring.5.dylib; do
  [ -f "$CURL_LIB/$f" ] || continue
  cp "$CURL_LIB/$f" "$STAGE/lib/"
  # Strip local symbols from the STAGED copies (the vendored originals keep
  # theirs) — ~10k locals across the chain, libcrypto being most of it. These
  # are NOT Chromium linker-signed binaries, so strip invalidates their ad-hoc
  # signature (dyld then SIGKILLs the consumer at load) — re-sign every one.
  if [ "$MODE" != debug ]; then
    strip -x "$STAGE/lib/$f"
    codesign --force --sign - "$STAGE/lib/$f" 2>/dev/null
  fi
done
ln -s libcurl.4.dylib "$STAGE/lib/libcurl.dylib"   # so -lcurl resolves
# libcurl's own install id is an absolute build-machine path; make it portable.
install_name_tool -id "@loader_path/libcurl.4.dylib" "$STAGE/lib/libcurl.4.dylib"
codesign --force --sign - "$STAGE/lib/libcurl.4.dylib" 2>/dev/null

if [ "$WANT_DYNAMIC" = true ]; then
  echo "==> [$MODE] staging libminiblink2.dylib + ANGLE GL dylibs"
  cp "$DIST/libminiblink2.dylib" "$STAGE/lib/"
  for f in libEGL.dylib libGLESv2.dylib libvk_swiftshader.dylib vk_swiftshader_icd.json; do
    [ -f "$DIST/$f" ] && cp "$DIST/$f" "$STAGE/lib/"
  done
  # Portability fixups (see header). The curl reference is whatever absolute
  # path this machine built with — read it from the load commands.
  CURL_REF="$(otool -L "$STAGE/lib/libminiblink2.dylib" | awk '/third_party\/curl/{print $1}')"
  [ -n "$CURL_REF" ] && install_name_tool -change "$CURL_REF" \
      "@loader_path/libcurl.4.dylib" "$STAGE/lib/libminiblink2.dylib"
  install_name_tool -id "@rpath/libminiblink2.dylib" "$STAGE/lib/libminiblink2.dylib"
  codesign --force --sign - "$STAGE/lib/libminiblink2.dylib" 2>/dev/null
  # Self-check: no build-machine paths may survive in the shipped load commands.
  if otool -L "$STAGE/lib/libminiblink2.dylib" | grep -q "$HERE"; then
    echo "error: staged dylib still references a build-machine path:" >&2
    otool -L "$STAGE/lib/libminiblink2.dylib" | grep "$HERE" >&2; exit 1
  fi
fi

STATIC_IS_BITCODE=false
if [ "$WANT_STATIC" = true ]; then
  echo "==> [$MODE] staging libminiblink2.a ($(du -h "$DIST/libminiblink2.a" | cut -f1))"
  cp "$DIST/libminiblink2.a" "$STAGE/lib/"
  if lipo -info "$STAGE/lib/libminiblink2.a" 2>/dev/null | grep -q "cputype (0)"; then
    # Legacy ThinLTO bitcode archive: not strippable, and only linkable with an
    # LTO-capable toolchain — documented in the README below.
    STATIC_IS_BITCODE=true
    echo "    ThinLTO bitcode archive — skipping strip (locals go away at the consumer's LTO link)"
  else
    echo "==> [$MODE] stripping local symbols from libminiblink2.a (keeps it linkable)"
    strip -x "$STAGE/lib/libminiblink2.a"
  fi
fi

# --- include/ + resources/ ---------------------------------------------------
echo "==> [$MODE] staging headers + runtime data"
cp "$DIST"/include/miniblink2/*.h "$STAGE/include/miniblink2/"
for f in blink_resources.pak icudtl.dat media_controls_resources_100_percent.pak \
         snapshot_blob.bin v8_context_snapshot.bin v8_context_snapshot.arm64.bin; do
  [ -f "$DIST/$f" ] && cp "$DIST/$f" "$STAGE/resources/"
done

# --- README.md ----------------------------------------------------------------
# Derive the framework list a static consumer must supply from the dylib's load
# commands (kept in sync with the engine automatically) — build-samples.sh trick.
FRAMEWORKS=""
if [ -f "$DIST/libminiblink2.dylib" ]; then
  FRAMEWORKS="$(otool -L "$DIST/libminiblink2.dylib" | tail -n +2 | awk '{print $1}' \
    | sed -n 's|.*/\([A-Za-z0-9_]*\)\.framework/.*|-framework \1|p' | sort -u | tr '\n' ' ')"
fi

LIB_LINES=""; USE_LINES=""
if [ "$WANT_DYNAMIC" = true ]; then
  LIB_LINES+="- \`lib/libminiblink2.dylib\` — the whole engine as one shared library (the miniblink2 mb* C API); loads the \`lib/libcurl*\` chain via \`@loader_path\`."$'\n'
  LIB_LINES+="- \`lib/libEGL.dylib\`, \`lib/libGLESv2.dylib\` — ANGLE (Metal); dlopen'd from the executable's directory for WebGL."$'\n'
  USE_LINES+="Link dynamically:

    clang++ app.mm -Iinclude -Llib -lminiblink2 -Wl,-rpath,@executable_path

At runtime, place next to your executable: \`libminiblink2.dylib\`, the
\`libcurl*\`/\`libssl*\`/\`libcrypto*\`/\`libnghttp2*\`/\`libidn2*\`/\`libintl*\`/\`libunistring*\`
chain, the ANGLE dylibs, and every file from \`resources/\` (the engine loads its
data and GL drivers from the executable's own directory).
"
fi
if [ "$WANT_STATIC" = true ]; then
  LIB_LINES+="- \`lib/libminiblink2.a\` — the complete engine merged into one archive."$'\n'
  if [ "$STATIC_IS_BITCODE" = true ]; then
    USE_LINES+="Link statically (this archive is ThinLTO BITCODE — it needs an
LTO-capable linker, i.e. a recent clang++ with lld; plain Xcode
clang+ld64 will report every symbol undefined):

    clang++ app.mm -Iinclude lib/libminiblink2.a \\
      -fuse-ld=lld -Wl,-ObjC -Wl,-dead_strip \\
      $FRAMEWORKS\\
      -Llib -lcurl -lresolv -lsandbox -lbsm -o app
"
  else
    USE_LINES+="Link statically (native archive):

    clang++ app.mm -Iinclude lib/libminiblink2.a \\
      -Wl,-ObjC -Wl,-dead_strip \\
      $FRAMEWORKS\\
      -Llib -lcurl -lresolv -lsandbox -lbsm -o app
"
  fi
  USE_LINES+="
Static binaries still need the \`libcurl*\` chain and \`resources/\` files next to
the executable at runtime.
"
fi

cat > "$STAGE/README.md" <<EOF
# miniblink2 macOS SDK (arm64, $MODE, $KIND)

Standalone single-process Blink (Chromium M150 / V8 15) behind the classic
miniblink2 \`mb\` C API — the spiritual successor to
miniblink49, rebuilt on the modern engine. https://github.com/yangxin0/miniblink2

$LIB_LINES- \`include/miniblink2/miniblink2.h\` — the public header (the mb* C API).
- \`resources/\` — engine runtime data (resource paks, ICU data, V8 snapshots).
  **Required**: copy these next to your executable.

$USE_LINES
Default build profile (include-only flags, see the repo's build docs): audio on;
video/WebGPU/WebAssembly/on-device-ML off; ICU trimmed to root+en+zh.
EOF

# --- zip ----------------------------------------------------------------------
echo "==> [$MODE] zipping -> $OUT"
rm -f "$OUT"
( cd "$STAGE_ROOT" && zip -q9 -ry "$OUT" "$NAME" )
echo "==> done: $OUT"
ls -lh "$OUT" | awk '{print "    "$5, $NF}'
