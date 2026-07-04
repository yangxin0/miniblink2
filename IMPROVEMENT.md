# API improvements for embedders

Lessons from integrating miniblink2 into a real interactive host (Glyph's
dictionary popup), cross-checked against Ultralight 1.4's SDK — the closest
comparable product (embeddable engine, offscreen surface, C API). Every item
below maps to a real integration incident, not a style preference. Ordered by
how much host-side pain each would remove.

## 1. Bounded update slice — never pump the host's run loop

**Today:** `mbPumpMessages()` and the load path drive a nested Cocoa run loop,
so arbitrary host code (display-link ticks, timers, main-queue blocks) executes
*inside* engine calls. Every embedder must hand-roll re-entrancy protection:
Glyph's shim wraps each entry point in an `EngineCall` guard, defers
engine-entering work scheduled mid-pump ("until the engine is off the stack"),
and had a user-visible bug from a pump tick landing inside the engine's own
pump (see §2). The nested pump also only quits reliably inside a running
`NSApp`, which forced the test harnesses into `NSApp.run()` wrappers.

**Ultralight:** `Renderer::Update()` dispatches ready timers/tasks and
*returns*; `RefreshDisplay()`/`Render()` produce frames. The host owns the
loop; nothing re-enters it.

**Proposal:** add `mbUpdate(void)` — run ready engine work (timers, async JS,
decodes, posted input) for a bounded slice with NO run-loop nesting — and
migrate interactive hosts off `mbPumpMessages`. Calls that arrive while the
engine is on the stack should be queued and run by the next `mbUpdate`
(engine-owned deferral) instead of being silently dropped or left as each
host's problem.

## 2. Damage tracking + explicit frame status on the paint path

**Today:** `mbRepaintToBitmap` copies the full BGRA frame into a caller buffer
every tick — no dirty rects, and (originally) no signal distinguishing "new
frame", "nothing changed", and "dropped, engine busy". Glyph blitted an
all-zero buffer whenever a paint was dropped mid-pump: a visible blank flash on
every pointer move (fixed host-side by checking the return code, but the API
made the bug easy and the fix crude). A 420×360@2x view re-copies ~1.2 MB per
tick at 60 fps even when nothing changed.

**Ultralight:** views render into a `Surface` (host-pluggable via
`SurfaceFactory`) with `LockPixels()/UnlockPixels()` and
`dirty_bounds()/ClearDirtyBounds()`. The host blits only the damaged region and
skips clean frames entirely.

**Proposal:** `mbGetDirtyBounds(view, int* x, y, w, h)` (empty = nothing to
do) + `mbClearDirtyBounds(view)`, and keep `mbRepaintToBitmap`'s 0-return
strictly meaning "buffer untouched". Longer term: an engine-owned surface the
host locks, instead of a per-frame copy.

**Related paint-purity rule:** the interactive paint used to end its drive tick
with `RunUntilIdle` *after* the lifecycle update — drained tasks (queued input)
re-dirtied style and the paint replayed an empty record (fixed: the lifecycle
pass is now unconditionally last). Keep "advance the world" (`mbUpdate`) and
"snapshot pixels" (paint) separate so this class of bug can't return.

## 3. Async loads with per-frame lifecycle callbacks

**Today:** `mbLoadHTML` is effectively synchronous — it pumps until the `load`
event and consumes subresources inline, and `mbOnLoadFinish` fires from inside
the caller's own call. Glyph had to build a concurrent prefetch layer that
downloads every referenced resource *before* calling load, gate the load on
that, and add a decline-not-block rule for unscanned URLs — all to keep the
engine's thread off the network during the synchronous window.

**Ultralight:** `LoadHTML/LoadURL` return immediately; the host receives
`begin-loading` / `finish-loading` / `fail-loading` / `window-object-ready` /
`DOM-ready`, each carrying `frame_id` + `is_main_frame`.

**Proposal:** an async load mode (`mbLoadHTMLAsync`), plus `mbOnBeginLoading` /
`mbOnFailLoading` to complete the existing finish/DOM-ready pair — with
`frame_id`. Document, per callback, whether it can fire re-entrantly from
inside an mb* call; that context is part of the ABI in practice.

## 4. Per-view resource hooks instead of process-wide ones

**Today:** the request-mock hook (and the dynamic request/response hooks) are
process-wide with no view parameter — `ctx` is always null. A host with two
views and different resource providers can only demultiplex by URL; Glyph
routes via synthetic per-dictionary hosts baked into the base URL.

**Ultralight:** platform services (FileSystem, FontLoader, Logger, Clipboard,
GPUDriver, SurfaceFactory) are injectable interfaces, and every view callback
carries `user_data`.

**Proposal:** `mbOnRequestMock(view, cb, userdata)` (falling back to the
process-wide hook when unset), and the same treatment for the request/response
observers. The static block/mock/rewrite tables can stay global — they are
config, not routing.

## 5. Typed input events

**Today:** Win32 heritage: `message` ints + flag bits for mice, wheel deltas in
120-unit ticks, no gesture phases. The embedder converts modern input down to
1995: Glyph's shim accumulates fractional trackpad deltas, fakes
`kPhaseNone`-style wheels (a phased wheel would route to the absent compositor
gesture generator and silently not scroll), and scales physical→CSS px by hand.

**Ultralight:** `ulViewFireMouseEvent(view, ULMouseEvent)` /
`ULScrollEvent{ScrollByPixel|ScrollByPage}` / `ULKeyEvent` — typed structs with
explicit units.

**Proposal:** `mbMouseEvent` / `mbWheelEvent` / `mbKeyEvent` structs (type,
position, buttons, modifiers, precise deltas, phase, click count) alongside the
legacy calls. Room to grow beats another flags parameter.

## 6. Split the embedder core from the automation kit

**Today:** one 1065-line header, ~190 exports, embedding primitives interleaved
with automation/testing tools (`mbWaitForSelector`, `mbSavePdf`,
`mbDownloadURL`, proxy config, request log). Several automation calls pump
(`mbWait`, `mbWaitFor*`) — dangerous in an interactive host, and nothing in the
header says so.

**Ultralight:** small per-domain headers (mirrored in `CAPI_*`), `Config` vs
`ViewConfig` split, AppCore as an optional convenience layer on top of the
core.

**Proposal:** split into `miniblink2_view.h` (create/resize/load/paint/input/
resource hooks — the interactive-host surface) and `miniblink2_auto.h` (waits,
shots, PDF, download, request log). Mark every pumping/blocking call in its doc
comment. One header per concern also keeps the ABI reviewable per release.

## What NOT to copy from Ultralight

- The engine trade: single-dylib real Chromium (M150 Blink + V8) beats a
  trimmed WebKit fork on web compat by a mile; nothing here argues for a
  smaller engine.
- Response mocking as the interception primitive is *stronger* than
  Ultralight's FileSystem for content-serving hosts (dictionary/media apps);
  keep it — just make it per-view (§4).
- The wait/shot automation helpers are a genuine differentiator for
  headless/testing users; the split in §6 keeps them first-class without
  endangering interactive hosts.
- A C++ RefPtr-style surface: the flat C ABI is the right call for a
  dlopen-able engine; ownership rules just need to stay explicit in comments.

---

## Status (2026-07-04)

| # | Proposal | State |
|---|----------|-------|
| 1 | mbUpdate / mbInEngineCall / mbDefer | **Shipped** (01805da). EngineScope guards all 42 engine-entering exports; mbUpdate no-ops instead of nesting and drains the mbDefer queue. Note: a nested pump in a PRIVATE run-loop mode is NOT possible on stock Chromium M150 (the pump sources live in kCFRunLoopCommonModes only), so host code can still run inside blocking calls — mbUpdate + mbInEngineCall are the mitigation. |
| 2 | Damage tracking | **Shipped, flag-level** (01805da): mbViewIsDirty via ScheduleNonCompositedAnimation, snapshot semantics. Dirty RECTS and an engine-owned lockable surface remain open. |
| 3 | Load lifecycle callbacks | **Shipped** (e047aff): mbOnBeginLoading (main-frame commit) + mbOnFailLoading (top-level failure funnel, with last_error_). mbLoadHTML/mbLoadURL already return before the load event, so no separate Async variants were needed. |
| 4 | Per-view request hooks | **Shipped, loader path** (c395cc8): mbOnRequestMock(view, cb, userdata) - MbFindMock takes an opaque host context, subresource loaders inherit the view from their frame client, hook self-erases on view destroy. Worker scripts and view-level MbFetchUrl helpers still fall back to the process-wide hook. |
| 5 | Typed input events | **Shipped** (01805da): mbMouseEvent / mbWheelEvent with struct_size versioning, float deltas, reserved phase. |
| 6 | view/auto header split | Open. Mechanical; touches package.sh staging. |

Verified in the Glyph host: engine smoke test + a 720-sample pointer-sweep
harness with a damage-gated blit path and a liveness beacon (0 flicker,
frames stay live).
