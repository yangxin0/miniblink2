# miniblink-modern — plan & loop state

> A **standalone, single-process embedder of modern Blink (Chromium M150)**: a
> hand-written tiny "content layer" (`miniblink_host`) that boots the real Blink
> engine in-process and renders HTML/CSS/JS to pixels through a pure-C ABI
> (`mb_capi`), with a `wke` compatibility layer and an `mb_shot` CLI on top.
> **No CEF, no browser process, no cross-process Mojo.** Successor to miniblink49.
>
> This file is the lean loop state. Full per-milestone history is in
> **`PROGRESS_ARCHIVE.md`** (do not append routine history here — keep this file small).

---

## ⚠️ Discipline (read before acting)
The core product is **done**. Each tick must move a *substantive* milestone forward or
fix a *real* bug — **not** add another test case / micro-flag / "characterize" probe to
have something to commit. If no substantive step exists and nothing is broken, **say so
and stop**. Verify everything empirically; **revert anything that doesn't verify** (end
each tick clean, no uncommitted changes). Never leak processes (run bounded: background +
watchdog SIGKILL, then `pgrep -x`). Network features verified against PUBLIC hosts
(example.com / httpbin / badssl, `dangerouslyDisableSandbox`), never localhost.

## Current state (complete)
- **Engine:** modern M150 Blink renders HTML→pixels in-process — V8/JS, cutting-edge CSS
  (`:has()`, nesting, `@container`, `oklch()`), canvas 2D, SVG, Web Components/Shadow DOM,
  fetch/XHR (headers/status/cookies/redirects/CJK), blob: URLs, Web Crypto, observers, WAAPI,
  forms+submit-nav, mouse/keyboard/touch/wheel input, host-side history, iframes, workers,
  IndexedDB/OPFS/cache-storage/DOM-storage, WebSocket, audio/video metadata, i18n.
- **`mb_capi` C ABI** — lifecycle / load / JS eval / selector scraping / input / screenshots
  (PNG·JPEG·PDF) / cookies / network config + interception / device emulation / history /
  **opt-in compositor** (`mbSetCompositingEnabled` + `mbViewComposite`/`mbViewCompositorPixel`).
- **`wke` compat layer** — full jsValue object model + async callbacks.
- **`mb_shot` CLI** — request-config → interact → synchronize → extract → capture, exit codes.
- **Major arcs DONE** (details in archive): network interception (static+dynamic request/response
  + header inject + load-error + CLI + wke peers); **WebGL** (in-process ANGLE/SwiftShader GLES2
  over an in-process GPU command buffer); **WebGPU** (in-process Dawn device + production context);
  **software compositor #1** (see below); device/media emulation; AX snapshot; print-background.
- **Tests (all green, no leaked procs):** `mb_smoke` 165, `mb_smoke_platform` 46, `mb_smoke_render`
  133, `mb_shot_smoke` 66, `wke_smoke` 117; probes gl/gpu/dawn/webgpu/webgpu2/compositor{,2,3,4,5}
  + `mb_compositor_widget_smoke`. This coverage is **sufficient** — do not pad it.

## Deferred gap list (strict build order — user directive: "start from 1st to last")
| # | Feature | Status |
|---|---------|--------|
| **1** | **Software compositor / cc raster → pixels** | **✅ COMPLETE** — a live page rasters through cc → viz::Display → bitmap, in-process headless (opt-in, default off). See below. |
| 2 | WebRTC (peer connections, getUserMedia) | **NEXT** |
| 3 | Cache-body large-blob durability | upstream blink bug; likely document-as-unfixable |
| 4 | Geolocation | deferred |
| 5 | PWA install | deferred |
| 6 | Permissions API (full) | deferred |
| 7 | Per-origin storage isolation hardening | deferred |
| 8 | IndexedDB transaction atomicity | deferred |
| 9 | IndexedDB blob-on-disk persistence | in-session works; disk persistence fragile |
| 10 | Worker storage origin wildcards | deferred |
| 11 | Third-party storage partitioning | deferred |
| 12 | Child-frame / session history routing | `history.back()/forward()` non-functional (needs browser-side history index) |
| 13 | Video frame stepping (per-currentTime) | deferred (lower value) |
| 14 | Real audio output | silent sink, untestable headless |
| 15 | Trusted touch into sub-frames | shared null-LayerTreeHost root cause — may be unblocked by #1 now |
| 16 | Device-emulation visual transform | layout/media-query emulation done; visual transform may use #1 now |
| 17 | wke request-side interception (`wkeOnLoadUrlBegin`) | drafted earlier, reverted to keep strict order |
| 18 | wke aliases / API variants | deferred |

## Compositor #1 — COMPLETE (architecture, for reference)
Opt-in (`mbSetCompositingEnabled`, default OFF; the software-paint screenshot path is untouched).
`mb::SoftwareCompositor` (platform/mb_compositor.{h,cc}) owns: a holder-backed `GLInProcessContext`
(for its `SharedImageInterface`) + the in-process GPU service's `SharedImageManager` + a
`viz::Display` over a capturing `SoftwareOutputSurface` + a port of `ui::DirectLayerTreeFrameSink`.
`MbWidget` (compositing branch) installs the patch-0012 frame-sink hook, calls
`InitializeCompositing` (single-thread synchronous `LayerTreeSettings`) + `SetWindowRectSynchronously
ForTesting` (sets cc's device viewport). `Composite()` drives `LayerTreeHostForTesting()->Composite
ForTest(raster=true)` then the Display draw → captured bitmap. Patches it needed: **0012** (blink
`AllocateNewLayerTreeFrameSink` host hook), **0013** (in-process SII channel-lost no-ops), **0014**
(holder `SharedImageManager` thread-safe — Display reads on main thread, GPU produces on GPU thread).
Remaining polish if ever wanted: read the composited frame into the actual `mbPaintToBitmap`
screenshot path; non-yellow/complex-page pixel tests.

---

## Operational facts
- **Project:** `/Users/yangxin/dennis/chrome/miniblink-modern/`, include-root `src/`
  (`src/miniblink_host/` = host + `mb_capi`; `src/wke/` = wke layer; `patches/` = donor patches).
- **Donor tree (already builds):** `/Users/yangxin/dennis/chrome/chromium-150.0.7871.24`,
  `out/Release` gen'd, `is_component_build=true`, macOS SDK, gn at `buildtools/mac/gn`.
- **wke parity reference:** `/Users/yangxin/dennis/chrome/miniblink49` (`wke/wke.h`).
- **Build & test:** `./build.sh /Users/yangxin/dennis/chrome/chromium-150.0.7871.24` stages sources
  into the donor tree, applies `patches/*.patch`, ninjas everything, runs the smoke suites + probes.
  (`wke_smoke` is built but run it manually after host changes.)
- **Donor patches (`patches/`):** 0001–0014. Notable: 0006 de-testonly GPU in-process targets,
  0012 frame-sink host hook, 0013 in-process SII channel-lost no-op, 0014 thread-safe SharedImageManager.
  Each patch must round-trip (build.sh applies with reverse-check idempotency).
- **Commits:** author `Xin Yang <yangxin0@outlook.com>`, ~72-col body explaining WHY, **NO
  AI / Co-Authored-By trailer**:
  `git -c user.name="Xin Yang" -c user.email="yangxin0@outlook.com" commit --no-verify`
  Commit per-milestone, only at a clean, tested state.

## Accepted by-design (NOT tasks — don't "fix")
- `window.open()` returns NULL (the embedder owns view creation; URL+name surfaced via the
  OnCreateNewWindow callback). Not a bug; don't make it return a view.
- `<meta viewport>` desktop-ignored; color emoji monochrome (no color-emoji font bundled).
- `<select>` arrow-keys on a CLOSED menulist don't cycle options inline — macOS-correct (the input
  opens the native popup, which we don't render). Set `.value` / click options for automation.
- Sub-frame synthetic input (click/move/wheel/drag/contextmenu/keyboard) is crash-free and routes
  correctly; clicking `<select>`/`<input type=file|date>` renders no popup but doesn't crash.
- `history.back()/forward()` page-driven traversal is non-functional (needs a browser-side history
  index; see gap #12). pushState/replaceState + sessionStorage DO work.
