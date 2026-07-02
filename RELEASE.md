# Releases

Release notes for miniblink2. Each release is an annotated git tag
(`v*`). `PROGRESS.md` has the day-to-day development journal.

---

## v0.2 — 2026-07-02 (unreleased)

**Binary-size pruning.** The ship dylib drops **97 → 88 MB** and the shipped
SDK footprint **~153 → ~112 MB**, with every cut an include-only toggle in
`scripts/build-lib.sh` (nothing deleted — each flag restores its feature):

- **`--wasm`** (default off, ~4.5 MB): V8's wasm engine compiled out;
  `window.WebAssembly` absent, like miniblink49. Patch 0020 fills a real
  upstream gap (V8's no-wasm stubs miss `v8::WasmModuleCompilation`, which
  Blink links unconditionally) with graceful fail-the-load stubs.
- **`--ml` extended** (~4 MB total off by default): patches 0019/0021 remove
  the last renderer TFLite users — the language-detection model (Blink
  `LanguageDetector` reports unavailable) and WebRTC's neural residual echo
  estimator (the same size exclusion upstream ships on Fuchsia).
- **`--av1-encode`** (default off, ~1 MB): libaom out; AV1 *decode* untouched.
- **`--tracing`** (default off, ~0.5 MB): OPTIONAL_TRACE_EVENT instrumentation
  compiled out (the Android/ChromeOS ship default).
- **`--swiftshader`** (default off, 20 MB of dist): the Metal path never loads
  it; the software-Vulkan fallback ships only on request.
- **`--icu-full`** (default off): `icudtl.dat` trimmed 10.4 → 6.3 MB by
  `scripts/trim_icu.py` (keeps root+en+zh, all CJK break/segmentation data,
  converters; other locales fall back to root).
- Exports pinned to the CamelCase `wke*`/`mb*` API (353 symbols, was 1,828),
  feeding `-dead_strip`; V8 snapshots now come from the same-flags build
  (flag-mismatched snapshots SIGTRAP at `mbInitialize`).
- New: `scripts/sizemap.py` per-component size attribution; `BACKLOG.md` §E
  documents the measured leftovers deliberately not cut (WebRTC is a feature).

Verified: samples boot and render against the pruned ship build; Intl
(zh/en), CJK segmentation, collation, canvas, and root-fallback for trimmed
locales all pass; `RTCPeerConnection` signaling intact.

---

## v0.1 — 2026-07-02 (`v0.1`)

**First baseline release.** A standalone, single-process embedder of modern
Blink (Chromium M150 / V8 15): a hand-written tiny content layer boots the
real engine in-process — no CEF, no separate browser process, no cross-process
Mojo IPC — behind a small C ABI.

- **Self-contained SDK** in `dist/<release|debug>/`: the whole engine as one
  `libminiblink2.dylib` or one complete `libminiblink2.a`, the two public C
  headers (`wke.h` classic miniblink API + `mb_capi.h` native ABI), and the
  runtime data (resource paks, `icudtl.dat`, V8 context snapshot, ANGLE).
- **GPU-accelerated WebGL on Metal** via ANGLE: `map.baidu.com` and MapLibre
  GL render their full vector maps on the GPU; SwiftShader ships as the
  software fallback. Dawn/WebGPU is compiled out unless `--webgpu`.
- **Network aligned with miniblink49**: curl_multi reactor; real sites load
  and render, including youtube.com.
- **Video playback**: the real `WebMediaPlayerImpl` pipeline plays YouTube
  video (VP9/MSE); audio output is the next step.
- **Sample apps** built against the SDK: `minibrowser_dyn` (89 KB app linking
  the dylib), `minibrowser_static` (fully self-contained), `mb_shot`.
- **Size-optimized builds** (`--size-optimized`, ThinLTO): 186 → 97 MB,
  roughly at parity with miniblink49 as shipped.
