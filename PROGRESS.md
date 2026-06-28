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
- **Tests (all green, no leaked procs):** `mb_smoke` 171, `mb_smoke_platform` 46, `mb_smoke_render`
  135, `mb_shot_smoke` 66, `wke_smoke` 119; probes gl/gpu/dawn/webgpu/webgpu2/compositor{,2,3,4,5}
  + `mb_compositor_widget_smoke`. This coverage is **sufficient** — do not pad it.

## Deferred gap list (strict build order — user directive: "start from 1st to last")
> **✅ ENTIRE GAP LIST #1–#18 COMPLETE** (as of 2026-06-28). Every numbered item is DONE or
> resolved as N/A-headless / accepted-by-design with empirical justification. No numbered gap
> remains. Remaining work is the **deferred follow-ups** listed under the table (each genuinely
> lower-value than the gap list was); pick the highest-value one per tick or stop if none is ripe.
| # | Feature | Status |
|---|---------|--------|
| **1** | **Software compositor / cc raster → pixels** | **✅ COMPLETE** — a live page rasters through cc → viz::Display → bitmap, in-process headless (opt-in, default off). See below. |
| 2 | WebRTC | **DONE (SDP) — connectivity deferred by-design.** SDP/signaling WORKS (RTCPeerConnection + createOffer/Answer + data-channel SDP + two-peer handshake to signalingState=stable). Real peer connectivity (ICE/DTLS) deferred: zero headless-automation value + needs heavy `//services/network` or a reimplemented P2P UDP socket stack (diagnosis + reuse plan in archive if ever wanted). getUserMedia = no devices (headless). |
| 3 | Cache-body large-blob durability | **ACCEPTED by-design** (4 investigations) — >256KB cached bodies read in RAPID succession intermittently come back empty: blink's in-process BlobBytesProvider stalls/empties under load. Found a real sub-bug (MbBlob::Clone copied empty data_ before async materialize — fix: defer clone), but the deeper provider stall remains + the fix risks page-blocking HANGS (worse than the current intermittent-empty). Not safely fixable from our layer. |
| 4 | Geolocation | **✅ DONE** — getCurrentPosition + permissions tracked the configured fix already; this tick FIXED watchPosition (was flooding ~180 cb/s because QueryNextPosition replied instantly every re-query) to report once then hold until `mbSetGeolocation` changes the fix, delivering a live update per move. Test 23d3. |
| 5 | PWA install | **Addressed / install-flow N/A headless.** getInstalledRelatedApps works (23ak); the install flow (beforeinstallprompt → prompt → appinstalled) is browser-UI-driven, meaningless headless, and doesn't hang (41o). The Web App Manifest is readable via DOM (`link[rel=manifest]` href + fetch); exposing blink's PARSED manifest (ManifestManager) is the only value-add but needs frame-interface-provider plumbing for marginal gain over fetch — deferred. |
| 6 | Permissions API | **✅ DONE** — query/request/has already worked (clipboard/notifications/wake-lock/geolocation granted, rest denied). This tick wired AddPermissionObserver (was a no-op) so `permissions.query({name:'geolocation'}).onchange` fires GRANTED↔DENIED on mbSetGeolocation/Clear (geolocation is the only runtime-dynamic permission). Test 23d2b. |
| 7 | Per-origin storage isolation | **✅ DONE** — DOM-storage/Cache/IDB/OPFS/BroadcastChannel were already origin-scoped; the one hole was **navigator.locks**, whose state lived PER-LockManager-bind (each context isolated, same-origin contexts never shared). Made locks a process-wide per-ORIGIN store (a bucket's locks scope to its (origin,bucket) key). Verified (23g2): two same-origin VIEWS now share the lock namespace (2nd view's ifAvailable sees view 1's held lock). |
| 8 | IndexedDB transaction atomicity | **✅ DONE** — explicit `transaction.abort()` rollback already worked (snapshot/restore, 23x). The hole was ERROR-driven: `Put` ignored the put mode, so `add()` silently overwrote instead of failing with ConstraintError on a duplicate key — meaning a failing add never triggered the unhandled-error auto-abort. Now AddOnly rejects an existing primary key with ConstraintError; the unhandled error aborts + rolls back. Test 23x2. |
| 9 | IndexedDB blob-on-disk persistence | **✅ DONE** (stale "fragile" note) — `mbSaveIndexedDB`/`mbLoadIndexedDB` capture blob bytes via async per-blob remote reads + re-mint on load. Tested + passing: 37n2 (Blob round-trips through save/DELETE-db/load — bytes from DISK, not the in-session handle), 37n3 (File name/lastModified/type + a 300KB BytesProvider blob). No remaining gap. |
| 10 | Worker storage origin | **✅ DONE** — dedicated + shared workers already alloc a synthetic frame_key and `MbSetFrameOrigin` to the script's origin, so a same-origin worker shares its window's IDB/locks and cross-origin is isolated. (Opaque data:/blob: worker = "null" wildcard for window↔worker BroadcastChannel bridging — a deliberate tradeoff.) |
| — | **Child-frame BrowserInterfaceBroker** | **✅ FIXED this tick** (bonus, blocks #11/#12) — `CreateChildFrame` passed an EMPTY browser broker, so every broker-backed API (storage/locks/permissions/geolocation/IDB) HUNG in iframes. Now child frames get `MakeFrameInterfaceBroker(child frame_key)` (origin set on DidCommitNavigation). Test 23g3: a same-origin iframe's held lock blocks the parent. |
| 11 | Third-party storage partitioning | **✅ DONE** (broker-scoped backends) — DidCommitNavigation now sets a frame's storage scope to `frame-origin` + `top-level-origin` when they differ, so a third-party iframe (e.g. widget.test in a.com vs b.com) gets ISOLATED IDB/Cache/locks per embedding site (first-party + same-origin frames key by the bare origin, unchanged). Test 23g4 (widget@t1's lock invisible to widget@t2). localStorage ALSO partitioned now (follow-up done): `MbFrameClient::DoCommit` computes a top-level-site-partitioned `WebNavigationParams::storage_key` for cross-site child frames (blink defaulted it to first-party), and `KeyForStorageKey` honors the StorageKey's cross-site top-level site. Test 23g5 (w3pls@t1's localStorage invisible to w3pls@t2). |
| 12 | Session / child-frame history | **✅ DONE (main-frame)** — `history.back()/forward()/go()`, pushState/replaceState, popstate, AND `history.length` all work (the archive's "non-functional / length stays 1" is stale — it's implemented via per-frame history_items_ + SetHistoryListFromNavigation + GoToEntryAtOffset replay). Tests 78c (multi-view back+popstate), 78c2 (length + back/forward traverse on a fresh view), 86 (host-driven). CHILD-FRAME history now ROUTES to the joint history (follow-up done): an iframe's `history.back()/forward()/go()` (and `navigation.traverseTo`) was a silent no-op — the child frame never bound a history sink, so blink's `GoToEntryAtOffset` on the child's LocalFrameHost was dropped — even though `iframe.history.length` already reported the main count. Now `CreateChildFrame` registers the child's frame_key sink to FORWARD to the main frame's joint session history (per HTML spec `window.history` is shared across the browsing context). Test 23at3 (an iframe's `history.back()` traverses the main frame to `/a` + fires its popstate). NOTE: per-entry frame-TREE snapshots (an iframe nav creating its own joint entry that back() reverts in isolation) still aren't modeled — that needs HistoryItem child trees; the common case (iframe back navigates the page) now works. |
| 13 | Video frame stepping (per-currentTime) | **✅ DONE** (stale note) — the player decodes the whole VPX stream, indexes frames by timestamp, and Paint() selects the frame at currentTime; drawImage/screenshot pull it. Test (mb_smoke_render): seeking 0→1.8s shows a DIFFERENT frame. |
| 14 | Real audio output | **N/A by-design** — audio PROCESSING works (OfflineAudioContext, 41c); real speaker output is meaningless + untestable headless. |
| 15 | Trusted touch into sub-frames | **✅ DONE** — `mbSendTouchTap` routes a trusted WebPointerEvent(kTouch) into an iframe firing touch-pointerdown, no crash (the SetMouseCapture null-guard from patch 0011 + the child-frame broker cover the touch hit-test/capture path too). Test 35z2. |
| 16 | Device-emulation visual transform | **Addressed / visual transform N/A headless** — `mbEmulateDevice` does the LAYOUT/media-query emulation (pointer/hover/viewport/dpr — the valuable part). The DevTools `EnableDeviceEmulation` "fit-to-window" VISUAL scale is cosmetic for headless (screenshots already render at device size via resize+DPR) + crashes on the null LayerTreeHost; deferred. |
| 17 | wke request-side interception (`wkeOnLoadUrlBegin`) | **✅ DONE** — fires per-request with a tagged WkeNetJob; `wkeNetSetData`/`wkeNetSetMIMEType` on a request job serve a canned response with NO network fetch (the classic offline-mock hook), backed by `mbSetRequestMockCallback`. The same setters still rewrite response bodies for `wkeOnLoadUrlEnd` (job kind dispatch). `wkeNetHookRequest` stub for parity. wke_smoke test. |
| 18 | wke aliases / API variants | **✅ DONE (real subset; rest N/A-headless).** Added the classic miniblink49 editor/load names real apps call: `wkeSelectAll` (a genuinely-NEW capability — whole-document select for select-all-then-copy scraping; verified via getSelection), `wkeCopy`/`wkeCut`/`wkePaste`/`wkeDelete`, plus `wkeStopLoading` (new `mbStopLoading` host backend → `WebLocalFrame::DeprecatedStopLoading`) and `wkeIsLoadComplete` alias. wke_smoke 118→119. The remaining ~120 unimplemented reference names are Windows windowing (`wkeCreateWebWindow`/`wkeShowWindow`/`wkeFireWindowsMessage`/HWND/DC), the native message loop, and W-suffix wide-string variants — **N/A for a headless macOS embedder**; not stubbed (an embedder should get a link error, not a silent no-op). |

## Deferred follow-ups (post-gap-list — pick highest-value per tick, or stop if none ripe)
Each is genuinely lower-value than the gap list. Verify tractability before committing to one.
- **localStorage third-party partitioning** ✅ DONE (2026-06-28) — see gap #11 row. Root cause: our
  `DoCommit` never set `WebNavigationParams::storage_key`, so blink defaulted a cross-site iframe to
  a first-party key. Fix: compute the partitioned StorageKey (top-level SchemefulSite + ancestor
  chain bit) for cross-site child frames at commit, and have `KeyForStorageKey` append the top site.
  Bonus: blink's own StorageKey is now correct for cross-site iframes (IDB/quota/etc.). Test 23g5.
- **Compositor → screenshot path** ✅ DONE (2026-06-28) — `mbPaintToBitmap` now reflects the
  composited frame for compositing views: it drives one fresh synchronous `Composite()` then copies
  the compositor's `captured_bitmap()` (cc → viz::Display → bitmap) into the output — a direct
  `readPixels` on an exact viewport match, a `drawImageRect` scale otherwise. Non-compositing views
  are untouched (software paint record). `mb_compositor_widget_smoke` now asserts the painted center
  pixel is the page's yellow (`shot_yellow=1`), so the compositor flows all the way into the
  user-facing screenshot API (`mbSavePng`/`mbSaveJpeg` ride on `PaintToBitmap` too).
- **Child-frame joint session history** ✅ ROUTING DONE (2026-06-28) — an iframe's history.back()/
  forward()/go() now forwards to the MAIN frame's joint session history (was a silent no-op; the
  child never bound a history sink). See gap #12 row. Test 23at3. Remaining (deferred, low value):
  per-entry frame-TREE snapshots so an iframe nav creates its own joint entry that back() reverts in
  isolation — needs HistoryItem child trees + a per-entry frame tree; the common case works now.
- **UA Client Hints** ✅ DONE (2026-06-28) — an empirical feature sweep found navigator.userAgentData
  .brands was EMPTY despite the rich UA string (an automation tell). Now `UserAgentMetadataOverride`
  returns realistic Chrome-150/macOS metadata for the built-in UA (custom UA stays empty). Test 19c.
- **Browser-identity / anti-detection tells** ✅ MOSTLY DONE (2026-06-28) — a bot-detection sweep found
  several headless tells that are also correctness bugs. FIXED: `window.chrome` (absent → injected
  app/runtime/csi/loadTimes stub at document-start), `window.screen` (0x0 → 1920x1080 desktop monitor
  via patch 0015 in `WidgetBase::InitializeNonCompositing`), `window.outer{Width,Height}` (0x0 → inner
  + ~79px chrome via `SetScreenRects`). Test 19d. REMAINING tell (deferred): `navigator.plugins` is
  empty + `navigator.pdfViewerEnabled` is false (real Chrome has the 5 built-in PDF-viewer entries) —
  needs blink plugin-data registration; medium effort. WebGL UNMASKED_RENDERER still reveals
  "SwiftShader" (would need a WebGL getParameter shim) — low priority.
- **WebRTC peer connectivity** (#2): SDP/signaling works; real ICE/DTLS needs a P2P UDP socket stack.
  Zero headless-automation value — only do if explicitly asked.
- **Cache large-blob durability** (#3): >256KB cached bodies intermittently read empty under rapid
  succession (blink in-process BlobBytesProvider stall). Not safely fixable from our layer (4 tries).

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
Composited frame is now wired into `mbPaintToBitmap` (see deferred follow-ups → "Compositor →
screenshot path", DONE). Remaining polish if ever wanted: non-yellow/complex-page pixel tests.

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
