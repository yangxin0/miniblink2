# Releases

Release notes for miniblink2. Each release is an annotated git tag
(`v*`). `PROGRESS.md` has the day-to-day development journal.

---

## v0.3 — 2026-07-06 (`v0.3`)

**The embedder-API release.** Both rounds of the Ultralight-informed API
program (IMPROVEMENT.md, items 1–13, distilled from the Glyph host
integration) are designed, shipped, and verified — a 720-sample pointer-sweep
harness with a damage-gated blit path shows 0 flicker with frames staying
live. The public header also splits by audience.

**Interactive-host surface (round 1).**

- `mbUpdate` — a re-entrancy-safe update tick that never nests the host's run
  loop; EngineScope guards all engine-entering exports; `mbInEngineCall` +
  `mbDefer` for work scheduled while the engine is on the stack.
- Damage flag (`mbViewIsDirty`) with strict snapshot semantics —
  `mbRepaintToBitmap` returning 0 now guarantees "buffer untouched" (kills the
  blank-flash-on-hover class of bug).
- Load lifecycle completed: `mbOnBeginLoading` (main-frame commit) +
  `mbOnFailLoading` (top-level failure funnel) join finish/DOM-ready.
- Per-view request mocking: `mbOnRequestMock(view, cb, userdata)` on the
  subresource loader path (process-wide hook remains the fallback).
- Typed input events: `mbMouseEvent` / `mbWheelEvent` with struct_size
  versioning, float deltas, reserved gesture phase.
- **Header split by audience** (breaking): `include/miniblink2/webview.h`
  (117 embedder exports) + `automation.h` (79 automation exports,
  pumping/blocking calls flagged). No umbrella header — every consumer names
  its audience.

**Round 2: profiles, budgets, diagnostics.**

- **Sessions** — browsing profiles as capability handles:
  `mbCreateSession(name, persist_path)` / `mbCreateViewInSession` /
  `mbSessionClearStorage` / `mbSessionFlush`. Everything origin-keyed
  partitions per session (DOM storage, IndexedDB, OPFS, buckets, locks,
  BroadcastChannel) plus per-session curl cookie jars; persistent profiles
  restore at create and flush at teardown. Ephemeral, implicit default
  session keeps plain `mbCreateView` disk-free.
- `mbSetMaxUpdateTime` — time-bounded update slice (Glyph runs 8 ms);
  `mbPumpMessages` stays run-to-idle for automation.
- `mbSetUserStylesheet` — per-view engine-side CSS, invisible to the document
  (user origin; author-level variant noted as follow-up).
- `mbPurgeMemory` / `mbLogMemoryUsage` — memory-pressure broadcast + V8
  low-memory GC for pooled hidden views.
- `mbEvalJSCatch` — JS exceptions surface (message + line) instead of being
  swallowed; the binding-lifecycle contract (what dies on navigation, where
  to re-inject) is documented in webview.h.
- `mbSetFontFamilies` (per-view CJK-aware font defaults), `mbUpdateAt`
  (frame-timestamped updates for display-link hosts).
- **DevTools stage A**: in-process CDP bridge — `mbDevToolsAttach` /
  `mbDevToolsSend` / `mbDevToolsDetach` drive blink's compiled-in
  DevToolsAgent directly (verified by a Runtime.evaluate round trip); ordinary
  Chrome is the intended frontend via a host-side WS bridge (stage B, lives in
  the embedder). Patches 0024–0026 fix the null-deref/browserless corners this
  exposed.
- Scrollbar clicks scroll instead of crashing (patch 0023); paint-purity fix:
  the lifecycle update is unconditionally the last step before paint replay.

**Build & docs.**

- `BUILD.md`: full build guide — profiles, feature flags, and the one-time
  macOS arm64 bootstrap for a tarball Chromium tree (cipd gn, pinned clang +
  objdump package, Metal toolchain, pinned node, and the
  Linux-binaries-in-the-tarball trap: rustc, esbuild, ninja, rollup's native
  binding), plus the missing-REF-paks gotcha.
- `MB_JOBS=N` caps ninja parallelism for shared machines.
- IMPROVEMENT.md and IMPROVEMENT2.md merged into one ledger with continuous
  item numbering; source comments repointed.
- API header renamed `view.h` → `webview.h`.

---

## v0.2 — 2026-07-03 (`v0.2`)

**API consolidation.** The public surface is now exactly one header —
`include/miniblink2/miniblink2.h`, the `mb*` C API (formerly `mb_capi.h`,
now living at `src/miniblink2/`). The legacy `wke` compatibility layer
(miniblink49 signatures) is removed: its wrapper, smoke suite, demo, the
`wkexe` sample and the `_wke*` exports are gone, and the `minibrowser`
sample is ported to the mb API (`mbRepaintToBitmap` blit loop, trusted
`mbSend*` input, `mbOn*` callbacks).

**Binary-size pruning.** The ship dylib drops **97 → 88 MB** and the shipped
SDK footprint **~153 → ~105 MB** (incl. stripping ANGLE's libGLESv2 18.3 →
11.9 MB), with every cut an include-only toggle in
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
- Exports pinned to the CamelCase `mb*` API (was 1,828 symbols),
  feeding `-dead_strip`; V8 snapshots now come from the same-flags build
  (flag-mismatched snapshots SIGTRAP at `mbInitialize`).
- New: `scripts/sizemap.py` per-component size attribution; `BACKLOG.md` §E
  documents the measured leftovers deliberately not cut (WebRTC is a feature).

**Distribution & build workflow.**

- New `scripts/package.sh` (the miniblink49 `package-macos.sh` equivalent):
  zips the SDK as `miniblink2-macos-arm64-<mode>[-kind].zip` (47 MB release
  dynamic) with `lib/` + `include/` + `resources/` + a generated README. The
  staged dylibs are made portable (vendored-curl reference rewritten to
  `@loader_path`, `@rpath` install ids, ad-hoc re-signing) and the whole
  curl chain + ANGLE + runtime data ship inside.
- Per-profile out dirs (`out/mono-release` dev vs `out/mono-release-ship`):
  dev↔ship switches no longer force a full ~28k-object recompile — a no-change
  ship rebuild is ~3 minutes, the post-refactor rebuild here was 5 ninja edges.
- The rebrand from miniblink-modern to **miniblink2** lands throughout
  (docs, sources, patches; remote: github.com/yangxin0/miniblink2).

Verified: samples boot and render against the pruned ship build; Intl
(zh/en), CJK segmentation, collation, canvas, and root-fallback for trimmed
locales all pass; `RTCPeerConnection` signaling intact; the packaged zip
compiles and boots the mb-API sample from a clean directory.

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
