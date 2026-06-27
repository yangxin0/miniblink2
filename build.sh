#!/usr/bin/env bash
# build.sh — build miniblink-modern against a Chromium M150 component checkout.
#
# Usage: ./build.sh /path/to/chromium-150.x.y.z
#
# Stages this project's host sources into the tree (they must live under
# //third_party/blink/renderer to see Blink's renderer-internal targets + config),
# wires the GN target, builds, vendors the resource pak, and runs the smoke test.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
TREE="${1:?usage: build.sh /path/to/chromium-150.x.y.z}"
OUT="${OUT:-out/Release}"
DEST="$TREE/third_party/blink/renderer/miniblink_host"
WKE_DEST="$TREE/third_party/blink/renderer/wke"

[ -d "$TREE/third_party/blink/renderer" ] || { echo "not a chromium tree: $TREE" >&2; exit 1; }

echo "==> staging host sources -> $DEST"
rm -rf "$DEST"
cp -R "$HERE/src/miniblink_host" "$DEST"
# wke compatibility layer (built into the host lib + a wke_smoke target; its
# BUILD.gn references live in miniblink_host/BUILD.gn via ../wke/).
echo "==> staging wke sources -> $WKE_DEST"
rm -rf "$WKE_DEST"
cp -R "$HERE/src/wke" "$WKE_DEST"

echo "==> applying blink compatibility patches"
for p in "$HERE"/patches/*.patch; do
  [ -f "$p" ] || continue
  if git -C "$TREE" apply --reverse --check "$p" 2>/dev/null; then
    echo "  (already applied: $(basename "$p"))"
  elif git -C "$TREE" apply "$p" 2>/dev/null; then
    echo "  applied $(basename "$p")"
  else
    echo "  WARN: could not apply $(basename "$p")"
  fi
done

# Wire the target into the build graph (idempotent): hang it on the root gn_all group.
ROOT_BUILD="$TREE/BUILD.gn"
if ! grep -q "miniblink_host" "$ROOT_BUILD"; then
  echo "==> NOTE: add this to $ROOT_BUILD group(\"gn_all\") deps:"
  echo '      deps += [ "//third_party/blink/renderer/miniblink_host" ]'
  echo "    (one-time manual step), then re-run."
fi

export PATH="$TREE/buildtools/mac:$PATH"
echo "==> gn gen"
( cd "$TREE" && gn gen "$OUT" >/dev/null )

echo "==> ninja miniblink_host mb_smoke mb_smoke_platform mb_smoke_render mb_shot mb_demo wke_smoke wke_demo mb_gl_probe mb_gpu_probe mb_dawn_probe mb_webgpu_probe mb_webgpu2_probe mb_compositor_probe mb_compositor2_probe mb_compositor3_probe mb_compositor4_probe"
( cd "$TREE" && ninja -C "$OUT" miniblink_host mb_smoke mb_smoke_platform mb_smoke_render mb_shot mb_demo wke_smoke wke_demo mb_gl_probe mb_gpu_probe mb_dawn_probe mb_webgpu_probe mb_webgpu2_probe mb_compositor_probe mb_compositor2_probe mb_compositor3_probe mb_compositor4_probe )

echo "==> vendor resource paks next to the binary"
cp "$TREE/$OUT/gen/third_party/blink/public/resources/blink_resources.pak" \
   "$TREE/$OUT/blink_resources.pak"
cp "$TREE/$OUT/gen/third_party/blink/renderer/modules/media_controls/resources/media_controls_resources_100_percent.pak" \
   "$TREE/$OUT/media_controls_resources_100_percent.pak"

echo "==> smoke tests (library/ABI — split into themed programs)"
( cd "$TREE/$OUT" && DYLD_LIBRARY_PATH="$PWD" ./mb_smoke )
( cd "$TREE/$OUT" && DYLD_LIBRARY_PATH="$PWD" ./mb_smoke_platform )
( cd "$TREE/$OUT" && DYLD_LIBRARY_PATH="$PWD" ./mb_smoke_render )

echo "==> mb_shot CLI smoke (argument parsing + stdout extraction)"
bash "$(dirname "$0")/src/miniblink_host/test/mb_shot_smoke.sh" "$TREE/$OUT"

echo "==> mb_gl_probe (WebGL milestone A: in-process ANGLE/SwiftShader GL)"
( cd "$TREE/$OUT" && DYLD_LIBRARY_PATH="$PWD" ./mb_gl_probe )

echo "==> mb_gpu_probe (WebGL milestone B: in-process GPU command buffer + GLES2)"
( cd "$TREE/$OUT" && DYLD_LIBRARY_PATH="$PWD" ./mb_gpu_probe )

echo "==> mb_dawn_probe (WebGPU milestone A: in-process Dawn instance + WebGPU device)"
( cd "$TREE/$OUT" && DYLD_LIBRARY_PATH="$PWD" ./mb_dawn_probe )

echo "==> mb_webgpu_probe (WebGPU milestone B: in-process WebGPU command buffer + dawn wire)"
( cd "$TREE/$OUT" && DYLD_LIBRARY_PATH="$PWD" ./mb_webgpu_probe )

echo "==> mb_webgpu2_probe (WebGPU milestone C1: PRODUCTION non-test in-process WebGPU context)"
( cd "$TREE/$OUT" && DYLD_LIBRARY_PATH="$PWD" ./mb_webgpu2_probe )

echo "==> mb_compositor_probe (Compositor milestone A: in-process viz software render-pass -> bitmap)"
( cd "$TREE/$OUT" && DYLD_LIBRARY_PATH="$PWD" ./mb_compositor_probe )

echo "==> mb_compositor2_probe (Compositor milestone B: in-process viz::Display -> bitmap)"
( cd "$TREE/$OUT" && DYLD_LIBRARY_PATH="$PWD" ./mb_compositor2_probe )

echo "==> mb_compositor3_probe (Compositor milestone C: cc frame sink -> viz::Display -> bitmap)"
( cd "$TREE/$OUT" && DYLD_LIBRARY_PATH="$PWD" ./mb_compositor3_probe )

echo "==> mb_compositor4_probe (Compositor milestone D1: PRODUCTION viz::Display -> bitmap)"
( cd "$TREE/$OUT" && DYLD_LIBRARY_PATH="$PWD" ./mb_compositor4_probe )

echo "==> artifacts: $TREE/$OUT/libminiblink_host.dylib  +  blink_resources.pak"
