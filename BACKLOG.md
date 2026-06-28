# miniblink-modern — Backlog: what is NOT done, and why

> Consolidated reference for everything that is **missing, deferred, low-value/unfixable,
> or accepted by-design**. The numbered gap list (#1–#18) and all high-value follow-ups are
> DONE — see `PROGRESS.md`. This file is *only* the not-done set, with the rationale for each.
> Last reviewed: 2026-06-28.

---

## A. Deferred — real features, but genuinely low value for a headless automation/scraping embedder

These *could* be built; none is worth the effort relative to its value. Listed worst-value first.

### A1. Service Workers — INVESTIGATED + DECLINED
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

### B1. Cache-storage large-blob durability — gap #3
- **Symptom:** `>256 KB` cached response bodies read in *rapid succession* intermittently come back
  empty. Root cause is blink's in-process `BlobBytesProvider` stalling/emptying under load.
- **Why unfixable here:** 4 separate investigations. Found a real sub-bug (`MbBlob::Clone` copied empty
  `data_` before async materialize — fix worked for 3/5 iterations) but the deeper provider stall
  remains, and the candidate fix risks page-blocking **hangs** (worse than the intermittent-empty).
  Not safely fixable from the embedder layer.

### B2. Real audio output — gap #14
- Audio *processing* works (`OfflineAudioContext`, decode, WebAudio graph). Real speaker output is
  meaningless and untestable headless. **N/A by-design.**

### B3. PWA install flow — gap #5
- `getInstalledRelatedApps` works; the Web App Manifest is readable via DOM. The install flow
  (`beforeinstallprompt` → prompt → `appinstalled`) is browser-UI-driven and meaningless headless
  (it doesn't hang). Exposing blink's *parsed* manifest (ManifestManager) is the only possible
  value-add and needs frame-interface plumbing for marginal gain over `fetch()`. **N/A / deferred.**

### B4. Device-emulation visual transform — gap #16
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

### C3. `<meta viewport>` desktop-ignored; color emoji monochrome
- Desktop rendering ignores `<meta viewport>` by design. Color emoji render monochrome (no color-emoji
  font is bundled).

### C4. `<select>` arrow-keys on a CLOSED menulist don't cycle options inline
- macOS-correct: the input opens the native popup (which headless doesn't render). For automation, set
  `.value` or click options directly.

### C5. Native popups don't render (but never crash)
- Clicking `<select>` / `<input type=file|date>` renders no popup but is crash-free. Sub-frame synthetic
  input (click/move/wheel/drag/contextmenu/keyboard) routes correctly and is crash-free. For file
  upload automation use `mbSetFileForSelector` (sets the FileList programmatically).

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

## Summary

| Bucket | Count | Net |
|--------|-------|-----|
| Deferred real features (A) | 3 | Service workers (huge/low-value), WebRTC connectivity (zero headless value), per-entry frame-tree history (complex/low-value) |
| Unfixable / N/A headless (B) | 4 | Cache large-blob (provider stall), audio output, PWA install, device visual transform |
| Accepted by-design (C) | 5 | WebGL renderer string, `window.open`, viewport/emoji, `<select>` keys, native popups |
| Robustness edges (D) | — | All handled |

**There is no remaining work that is both substantive and safe.** Everything actionable is done; the
embedder is validated five ways (feature sweep, functional sweep, real-site, SPA, adversarial-robustness),
all green: mb_smoke 178 · mb_smoke_platform 46 · mb_smoke_render 135 · mb_shot_smoke 66 · wke_smoke 119.
