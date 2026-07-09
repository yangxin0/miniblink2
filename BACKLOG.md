# miniblink2 — Backlog: what is NOT done, and why

> Consolidated reference for everything that is **missing, deferred, low-value/unfixable,
> or accepted by-design**. The numbered gap list (#1–#18) and all high-value follow-ups are
> DONE. This file is *only* the not-done set, with the rationale for each.
> Last reviewed: 2026-07-06. Since the 06-28 review the project grew an interactive-host
> audience (Glyph; see IMPROVEMENT.md) alongside headless automation — rationales below were
> re-checked against both.

---

## A. Deferred — real features, but genuinely low value for a headless automation/scraping embedder

These *could* be built; none is worth the effort relative to its value. Listed worst-value first.

### A1. Service Workers — INVESTIGATED + DECLINED, then EXPLICITLY REQUESTED
- **2026-07-06 update:** the effort-aware request arrived; the staged plan is
  `docs/SERVICE_WORKERS.md` (S1 registration/lifecycle → S2 running worker →
  S3 fetch interception → S4 persistence; gated off until S3 so the
  no-fake-success rule below is never violated). The original analysis stands:
- **State today:** `navigator.serviceWorker` exists; `register()` **rejects cleanly with a
  `TypeError`** (no hang). Feature-detecting sites degrade gracefully — the correct non-support state.
- **Scope to actually support:** 28 service-worker mojom interfaces + ~40 renderer `.cc` files, plus
  the entire browser-side stack reimplemented in-process: `ServiceWorkerContainerHost`,
  Registration/Object hosts, embedded-worker startup, the install→activate lifecycle state machine,
  `ControllerServiceWorker` + fetch-event dispatch with response streams, and installed-scripts caching.
- **Why declined:** the single largest subsystem in the project (multi-week). Low headless value — SW
  is progressive enhancement; nearly all sites work without it. **No safe minimal subset:** a stub that
  resolves `register()` without running the worker is *harmful* (a site assumes SW works, then breaks
  when its fetches aren't intercepted/cached). A clean rejection is better than a fake success.
- **Build only on an explicit, effort-aware request.**

### A2. WebRTC peer connectivity (real ICE/DTLS) — gap #2 tail
- **State today:** SDP/signaling fully works (`RTCPeerConnection`, createOffer/Answer, data-channel
  SDP, two-peer handshake to `signalingState=stable`). `getUserMedia` = no devices (headless).
- **Missing:** actual peer connectivity (ICE candidate gathering, DTLS, media/data transport).
- **Why deferred:** needs a real `//services/network` P2P UDP socket stack or a reimplemented one.
  **Zero** headless-automation value (no human, no peer, no devices). Only build if explicitly asked.

### A3. Per-entry frame-tree session history — gap #12 tail
- **State today:** main-frame history (`back/forward/go`, pushState/replaceState, popstate,
  `history.length`) works; a child frame's `history.back()` now routes to the **joint** (main-frame)
  history (the common "iframe back navigates the page" case works).
- **Missing:** true per-entry frame-**tree** snapshots — an iframe navigation creating its *own* joint
  history entry that `back()` reverts *in isolation* (reverting only that iframe, not the main frame).
- **Why deferred:** needs `HistoryItem` child trees + a per-entry frame-tree model threaded through the
  navigation flow. Complex; low automation value.

---

## B. Unfixable from our layer (or N/A by the nature of headless)

(B1, the cache-storage large-blob stall, lived here through four investigations
as "unfixable". The fifth found it: the registered MbBlob was SELF-OWNED on its
mojo pipe, so the page dropping its last JS handle deleted the blob
MID-MATERIALIZE — tearing down the BytesProvider before it replied (the empty)
and stranding queued readers (the hang). Registered blobs now use
deferred-delete ownership: they outlive their pipe until the bytes are
assembled and every queued consumer is served. Regression: mb_smoke_render 73e,
10/10 rapid >256KB put/match/read cycles.)

### B1. PWA install flow — gap #5
- `getInstalledRelatedApps` works; the Web App Manifest is readable via DOM. The install flow
  (`beforeinstallprompt` → prompt → `appinstalled`) is browser-UI-driven and meaningless headless
  (it doesn't hang). Exposing blink's *parsed* manifest (ManifestManager) is the only possible
  value-add and needs frame-interface plumbing for marginal gain over `fetch()`. **N/A / deferred.**

### B2. Device-emulation visual transform — gap #16
- `mbEmulateDevice` does the valuable layout/media-query emulation (pointer/hover/viewport/dpr). The
  DevTools "fit-to-window" *visual* scale is cosmetic for headless (screenshots already render at
  device size via resize+DPR) and crashes on the null LayerTreeHost. **N/A / deferred.**

---

## C. Accepted by-design — NOT bugs; do not "fix"

### C1. WebGL `UNMASKED_RENDERER_WEBGL` = ANGLE/SwiftShader
- Reports the **real** software renderer. Deliberately **not spoofed**: it's truthful, software
  rendering is common on real machines (CI/VMs/low-end), and faking a hardware-GPU string would be
  *inconsistent* with our actual canvas/WebGL pixel output (which fingerprinters hash) while a JS
  `getParameter` override is itself `toString`-detectable — a half-measure with real downsides. The
  non-debug `VENDOR`/`RENDERER` already report the spec-masked "WebKit"/"WebKit WebGL" like every
  browser. This is the **only** remaining anti-detection tell; all others are fixed (UA-CH,
  `window.chrome`, screen/window metrics, `navigator.plugins`/`pdfViewerEnabled`, `navigator.webdriver`).

### C2. `window.open()` returns NULL
- The embedder owns view creation; the requested URL + name are surfaced via the `OnCreateNewWindow`
  callback. Not a bug — don't make it return a view.

### C3. `<meta viewport>` desktop-ignored
- Desktop rendering ignores `<meta viewport>` by design.
- (This entry used to also claim "color emoji render monochrome — no color-emoji font bundled".
  That diagnosis was wrong twice over: Apple Color Emoji is a SYSTEM font needing no bundling, and
  the actual bug was mbLoadHTML committing without an authoritative charset — the parser fell back
  to windows-1252 and mojibake'd the emoji's UTF-8 bytes into four Latin-1 glyphs that Times
  rendered in gray. mbLoadHTML now commits UTF-8 per its ABI contract (`utf8_html`), and patch 0028
  additionally stops blink's mac fallback from downgrading a substituted color face to monochrome
  "Apple Symbols" unless text presentation was explicitly requested. Emoji render in color;
  regression: mb_smoke_render 56b asserts vivid pixels.)

### C4. `<select>` arrow-keys on a CLOSED menulist don't cycle options inline
- macOS-correct: the input opens the popup. For automation, set `.value` or click options directly.
- Interactive hosts now RECEIVE the popup instead of a dead-end: `mbOnSelectPopup` delivers the
  items/bounds/selection and the host commits with `mbSelectPopupCommit`/`mbSelectPopupCancel`
  (blink's external popup path; smoke: mb_smoke 43). Date/file choosers remain host-side concerns
  (`mbSetFileForSelector` for file upload).

### C5. Native popups don't render (but never crash)
- The engine never renders OS popups itself; sub-frame synthetic input (click/move/wheel/drag/
  contextmenu/keyboard) routes correctly and is crash-free. `<select>` menulists are surfaced to
  the host via mbOnSelectPopup (see C4); `<input type=file>` uses `mbSetFileForSelector`.

---

## D. Robustness edges (all currently HANDLED — listed so they aren't re-investigated)

Adversarial-content sweep (2026-06-28) — every case is handled gracefully:
- **Synchronous infinite loop** (`while(true){}`) → terminated by the opt-in script watchdog
  (`mbSetScriptTimeout` / `mb_shot --script-timeout`); the embedder recovers and loads the next page.
- **Infinite microtask flood** (`Promise.resolve().then(f)` recursion) → same watchdog catches it
  (drains within one task's microtask checkpoint).
- **Stack overflow** → throws `RangeError`, caught.
- **Infinite `setTimeout` flood** → doesn't block load completion.
- **Memory bombs** (oversized string/array/ArrayBuffer/TypedArray, doubling concat) → `RangeError` via
  V8's own limits; deeply-nested JSON parses without crashing. No OOM crash, no hang.
- **Network** (proxy, ignore-cert, follow-redirects, slow/never-loading subresources, bounded
  `--wait-*` timeouts) → flags exist and timeouts are graceful.

---

## E. Binary-size pruning — remaining candidates (measured 2026-07-02)

Context: the size-pruning pass (see the `--wasm`/`--av1-encode`/`--tracing`/`--swiftshader`/
`--icu-full` toggles in `scripts/build-lib.sh`, plus patch 0019) covered every cut with a
supported GN seam. Measurement tool: `scripts/sizemap.py` (nm-based per-component attribution
of the unstripped dylib). Numbers below are from the 97 MB pre-prune release dylib (82 MB
attributed `__text`). These are the *patch-level* leftovers, hardest-first:

### E1. WebRTC stack — DONE 2026-07-07 (patch 0030 + `--webrtc` toggle)
- Implemented as an include-only build-lib.sh flag, **default OFF** (measured: 7.5 MB of
  attributed `__text` off the dev-release dylib; webrtc bucket 3.28 MB → 0.01 MB, rest from
  RTC bindings/absl/inlined code). `mb_enable_webrtc` (blink config.gni, default true) gates:
  RTC IDL bindings (idl/generated_in_modules.gni), modules/{peerconnection,webrtc},
  platform {peerconnection,p2p,webrtc} + APM proxy, the modules structured-clone
  serializer's RTC types, and media/webrtc (shrinks to constants+features).
  mediastream/getUserMedia survives; smoke 41p asserts RTCPeerConnection absent when off,
  and still tests SDP offer generation when built with --webrtc.
- NOTE: SDP/signaling was a shipped feature (A2); hosts that rely on it must build --webrtc.

#### (original analysis, kept for reference)
Was: ~3.3 MB (2.6 core + 0.7 Blink RTC bindings) — DO NOT cut blindly
- **Tension:** A2 above — SDP/signaling **works and is a shipped feature** (RTCPeerConnection,
  offer/answer, data-channel SDP). Cutting WebRTC removes a working capability, unlike the other
  prunes which only dropped never-used code.
- **No upstream GN seam:** the old `enable_webrtc` arg is long gone; `modules/peerconnection` +
  `modules/mediastream` compile unconditionally and reference `webrtc::` symbols directly, and the
  generated V8 bindings reference the module classes. A cut needs bindings-level surgery
  (IDL list gating), not a component stub like 0019.
- **If ever done:** own toggle (`--webrtc`, include-only), and per the A2 rationale the default
  probably stays ON given signaling is a feature. Revisit only if 3.3 MB matters more than that.

### E2. WebXR bindings — ~0.27 MB; device APIs (USB/BT/serial/HID/NFC) — ~0.23 MB
- Dead on macOS headless, but wired through generated bindings like E1 (small payoff, same
  surgery class). Bundle with E1 if that work ever happens. (The inspector, once listed
  here as a prune candidate, is a shipped public API now — no longer cuttable.)

### E3. Misc measured, no seam yet
- **perfetto core** ~0.5 MB (client library is mandatory in M150 base; only the
  OPTIONAL_TRACE_EVENT layer is gated — that's the `--tracing` toggle).
- **zstd** ~0.33 MB (Content-Encoding: zstd / shared-dictionary support in the fetch stack).
- **Rust crabbyavif + dav1d** ~1 MB (AVIF decode; entangled with the unconditional
  VideoToolbox/libgav1 AV1 path — same landmine as the VpxVideoDecoder startup reference).
- **cnv converters in icudtl.dat** ~0.9 MB (legacy charset decoding for real-web HTML —
  kept deliberately; trimming risks mojibake on GBK/Big5/Shift-JIS pages).

---

## Summary

| Bucket | Count | Net |
|--------|-------|-----|
| Deferred real features (A) | 3 | Service workers (huge/low-value), WebRTC connectivity (zero headless value), per-entry frame-tree history (complex/low-value) |
| Unfixable / N/A headless (B) | 2 | PWA install, device visual transform |
| Accepted by-design (C) | 5 | WebGL renderer string, `window.open`, viewport, `<select>` keys, native popups |
| Robustness edges (D) | — | All handled |

(Fixed out of this file on 2026-07-06: the cache-storage large-blob stall — a blob-lifetime bug,
not an unfixable provider stall; monochrome emoji — an mbLoadHTML charset bug, not a missing font;
and `<select>` clicks dead-ending — now surfaced to the host via mbOnSelectPopup.)

The **feature surface** is complete and validated five ways (feature sweep, functional sweep, real-site,
SPA, adversarial-robustness). A later **correctness-review pass** of the implementation (not features)
did, however, find a tail of real bugs that the capability-focused suites didn't exercise — these were
fixed and given regression tests:
- `WaitForSelector` didn't escape the selector (a `[data-x="y"]` wait timed out forever).
- The libcurl loader retried non-idempotent requests (duplicate POST/PUT/DELETE) and mis-treated
  204/304 empty bodies as failures.
- IndexedDB: use-after-free on `deleteDatabase`/load-from-disk with a live handle; an aborted upgrade
  hung `open()` and didn't roll back schema; a 2nd connection clobbered the 1st's completion routing;
  reverse-cursor `continue(key)` mis-seeked; `deleteObjectStore` leaked records.
- The `wke` `jsValue` store wholesale-cleared at 4096 entries, dangling all live handles; `jsToBoolean`
  mis-coerced null/undefined/NaN/`"0"`/`"false"`.
- A `blob:` URL cloned/fetched before a large blob materialized returned truncated bytes.
- The JS-dialog C callback was installed via a UB `reinterpret_cast` across the C/C++ boundary.
- `build.sh` silently continued when a donor patch failed to apply (→ a binary that builds but
  hangs/crashes); it now aborts.

A **second correctness-review pass** (subsystem audit of the hand-written backends) found and fixed a
further tail of edge bugs the capability suites didn't exercise (all green after; IDB-range + wke cases
given regression tests in `mb_smoke`/`wke_smoke` — the `wke` layer and its suite were later removed
in v0.2, so the wke bullets below are history, not living surface):
- **IndexedDB key ranges**: `get()`/`getKey()`/`delete()`/`count()` treated a range as its exact
  lower-bound key — `get(lowerBound(5))` missed 6,7…, `delete(bound(2,4))` left 3,4, `count(range)`≤1.
  Now they honor the full range (matching `getAll`/`openCursor`). Also: `count()`/`get()` honor the
  index id; `nextunique`/`prevunique` cursors de-duplicate per index key; `continuePrimaryKey` seeks the
  primary key; binary (ArrayBuffer) keys decode after save/load; an aborted upgrade that deleted a store
  rolls its records back; the transaction rollback snapshot is now **per-store, keyed by (connection,
  txn)** so concurrent disjoint-scope transactions don't clobber each other.
- **libcurl loader**: cross-origin redirects now apply blink's `removed_headers`/`modified_headers`
  (was re-sending `Authorization`/`Cookie` to the redirect target — a credential leak); duplicate
  response headers (multi `Set-Cookie`/`Link`/`Vary`) are comma-joined (`AddHttpHeaderField`) not
  collapsed to the last.
- **WebView**: `~MbWebView` now calls `web_view_->Close()` (was a full page leak + a use-after-free of
  the freed scheduler by a live timer); a failed `GoBack`/`GoForward` rolls back the index/flag instead
  of corrupting history; back/forward to an in-memory `LoadHTML` doc re-commits its cached source;
  `PaintRectToBitmap` no longer applies dsf (its ABI contract is a w×h, dsf-not-applied buffer).
- **Widget**: synthetic `mousedown` carries the pressed-button modifier (so `event.buttons` is correct);
  `Resize` refreshes the screen rects + cc viewport.
- **Frame client**: a page-driven cross-document `history.back()/forward()` no longer double-records
  into both history lists.
- **wke**: `jsDouble` round-trips with full precision (was `%f`/6-digit, mangling `1e-7`→0);
  `jsToInt`/`jsToFloat`/`jsToDouble` parse exponential/boolean forms (was `atoi`/`atof`); a throwing
  side-effecting expression runs **once** (`StoreEval` no longer re-runs on a runtime throw);
  `JsStringLiteral` escapes all control chars; the temp string buffers are thread-local.
- **Script watchdog**: a nested run-loop's `DidProcessTask` no longer disarms the outer task's deadline.

Lesson: capability coverage ≠ edge/adversarial coverage. The suites prove features *work*; they did not
prove the in-process backend re-implementations and the `wke` compat layer were *correct* at their edges.
