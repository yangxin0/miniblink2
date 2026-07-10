# Backlog — what is NOT done, and why

The not-done set: deferred features, gaps unfixable from our layer, non-bugs
accepted by design, and binary-size leftovers. The numbered gap list and every
high-value follow-up are done; this file is only the residue, with the rationale
for each. Companion to the [API design log](IMPROVEMENT.md).

---

## A. Deferred features — real, but low value for a headless/automation embedder

- **A1. Service Workers.** `navigator.serviceWorker` exists and `register()`
  **rejects cleanly** (no hang), so feature-detecting sites degrade correctly.
  Full support is the single largest subsystem in the project (28 SW mojom
  interfaces + ~40 renderer files, plus the entire browser-side stack —
  container/registration hosts, embedded-worker startup, install→activate,
  `ControllerServiceWorker` fetch dispatch — reimplemented in-process; multi-week).
  **No safe minimal subset:** a stub that resolves `register()` without running
  the worker is *harmful* (the site assumes SW works, then breaks when fetches
  aren't intercepted). Staged plan in `docs/SERVICE_WORKERS.md`, gated off until
  fetch interception is real. Build only on an explicit, effort-aware request.
- **A2. WebRTC peer connectivity.** SDP/signaling fully works
  (`RTCPeerConnection`, createOffer/Answer, data-channel SDP, two-peer handshake
  to `signalingState=stable`); `getUserMedia` reports no devices (headless).
  Missing: real ICE/DTLS/media-data transport, which needs a P2P UDP socket
  stack. Zero headless value. (`--webrtc` builds the stack in; default off.)
- **A3. Per-entry frame-tree session history.** Main-frame history works
  (back/forward/go, pushState/replaceState, popstate); a child frame's `back()`
  routes to the joint main-frame history (the common "iframe back navigates the
  page" case). Missing: true per-entry frame-tree snapshots (an iframe
  navigation reverting *in isolation*). Complex, low automation value.

## B. Unfixable from our layer / N/A headless

- **B1. PWA install flow.** `beforeinstallprompt`→prompt→`appinstalled` is
  browser-UI-driven and meaningless headless (it doesn't hang). The manifest is
  readable via DOM/`fetch()`; exposing blink's parsed manifest is marginal gain.
- **B2. Device-emulation visual transform.** `mbEmulateDevice` does the valuable
  layout/media-query emulation (pointer/hover/viewport/dpr). The DevTools
  fit-to-window *visual* scale is cosmetic headless (screenshots already render
  at device size via resize+DPR) and crashes on the null LayerTreeHost.

## C. Accepted by-design — NOT bugs; do not "fix"

- **C1. WebGL `UNMASKED_RENDERER` = ANGLE/SwiftShader.** Reports the *real*
  software renderer; deliberately not spoofed (it's truthful, software rendering
  is common on real machines, and faking a hardware string would be inconsistent
  with the actual canvas/WebGL pixels fingerprinters hash — while a JS
  `getParameter` override is itself detectable). Masked `VENDOR`/`RENDERER`
  already report "WebKit"/"WebKit WebGL" like every browser. This is the only
  remaining anti-detection tell; all others are fixed.
- **C2. `window.open()` returns NULL.** The embedder owns view creation; URL +
  name surface via callback. Adopt the child (live opener/`postMessage`) via
  `mbOnCreateChildView` ([IMPROVEMENT.md](IMPROVEMENT.md) item 22).
- **C3. `<meta viewport>` desktop-ignored.** Desktop rendering ignores it by
  design.
- **C4. `<select>` arrow-keys on a closed menulist don't cycle options inline.**
  macOS-correct (the input opens the popup). Interactive hosts receive the popup
  via `mbOnSelectPopup` + `mbSelectPopupCommit`/`Cancel`; for automation, set
  `.value` or click options directly.
- **C5. Native popups don't render (but never crash).** The engine never renders
  OS popups itself; sub-frame synthetic input (click/move/wheel/drag/keyboard)
  routes correctly and is crash-free. `<select>`→`mbOnSelectPopup`;
  `<input type=file>`→`mbSetFileForSelector`.

## D. Robustness edges — all handled

Adversarial-content sweep, every case graceful (no hang, no OOM crash):
infinite loops and microtask floods → the opt-in script watchdog
(`mbSetScriptTimeout` / `mb_shot --script-timeout`); stack overflow → caught
`RangeError`; memory bombs (oversized strings/arrays/buffers, deep JSON) → V8's
own limits; network (proxy, ignore-cert, redirects, slow/never-loading
subresources) → bounded `--wait-*` timeouts.

## E. Binary-size pruning — remaining candidates

Every cut with a supported GN seam is done (the `--wasm` / `--av1-encode` /
`--tracing` / `--swiftshader` / `--icu-full` / `--webrtc` toggles in
`build-lib.sh`; measure with `scripts/sizemap.py`). The leftovers all need
bindings-level surgery (IDL gating), not a component stub:

- **WebRTC** (~3.3 MB) — done via `--webrtc` (default off). Unlike the other
  prunes, SDP/signaling is a *working feature* (A2), so hosts that rely on it
  must build it back in.
- **WebXR + device APIs** (USB/BT/serial/HID/NFC, ~0.5 MB) — dead on macOS
  headless but wired through generated bindings; bundle with any future
  IDL-gating pass.
- **No GN seam yet** — perfetto core ~0.5 MB (the client library is mandatory in
  M150 base; only the `OPTIONAL_TRACE_EVENT` layer is gated, via `--tracing`),
  zstd ~0.33 MB (Content-Encoding), Rust AVIF decode ~1 MB (entangled with the
  unconditional AV1 path), ICU charset converters ~0.9 MB (kept — trimming risks
  mojibake on GBK/Big5/Shift-JIS pages).

---

**Lesson worth keeping:** capability coverage ≠ edge/adversarial coverage. The
feature suites prove features *work*; they did not prove the hand-written
in-process backends were *correct at their edges*. Two later correctness-review
passes found and fixed a tail of real bugs the capability suites never
exercised — IndexedDB key-range/cursor handling and transaction rollback, the
libcurl loader (cross-origin redirect header stripping, non-idempotent-request
retries, 204/304 empty bodies), WebView/history lifetime, and blob
materialization races — each given a regression test.
