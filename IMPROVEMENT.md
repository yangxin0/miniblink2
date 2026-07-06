# API improvements for embedders

Lessons from integrating miniblink2 into a real interactive host (Glyph's
dictionary popup), cross-checked against Ultralight 1.4's SDK — the closest
comparable product (embeddable engine, offscreen surface, C API). Every item
maps to a real integration incident, not a style preference; each round is
ordered by leverage-per-effort for the Glyph host.

Round 1 (items 1–6) is fully shipped. Round 2 (items 7–13) is shipped except
zero-copy bodies (deferred by cost) and the inspector's host-side bridge
(stage B, lives in the embedder). Per-item status is inline under each item.

Verified in the Glyph host: engine smoke test + a 720-sample pointer-sweep
harness with a damage-gated blit path and a liveness beacon (0 flicker,
frames stay live).

---

## Round 1

### 1. Bounded update slice — never pump the host's run loop

**Then:** `mbPumpMessages()` and the load path drove a nested Cocoa run loop,
so arbitrary host code (display-link ticks, timers, main-queue blocks) executed
*inside* engine calls. Every embedder had to hand-roll re-entrancy protection:
Glyph's shim wrapped each entry point in an `EngineCall` guard, deferred
engine-entering work scheduled mid-pump, and had a user-visible bug from a pump
tick landing inside the engine's own pump (see §2). The nested pump also only
quit reliably inside a running `NSApp`, forcing test harnesses into
`NSApp.run()` wrappers.

**Ultralight:** `Renderer::Update()` dispatches ready timers/tasks and
*returns*; `RefreshDisplay()`/`Render()` produce frames. The host owns the
loop; nothing re-enters it.

**Shipped** (01805da): `mbUpdate` runs ready engine work with no run-loop
nesting; EngineScope guards all 42 engine-entering exports; mbUpdate no-ops
instead of nesting and drains the `mbDefer` queue. Note: a nested pump in a
PRIVATE run-loop mode is NOT possible on stock Chromium M150 (the pump sources
live in kCFRunLoopCommonModes only), so host code can still run inside blocking
calls — `mbUpdate` + `mbInEngineCall` are the mitigation.

### 2. Damage tracking + explicit frame status on the paint path

**Then:** `mbRepaintToBitmap` copied the full BGRA frame into a caller buffer
every tick — no dirty rects, and (originally) no signal distinguishing "new
frame", "nothing changed", and "dropped, engine busy". Glyph blitted an
all-zero buffer whenever a paint was dropped mid-pump: a visible blank flash on
every pointer move. A 420×360@2x view re-copied ~1.2 MB per tick at 60 fps even
when nothing changed.

**Ultralight:** views render into a `Surface` (host-pluggable via
`SurfaceFactory`) with `LockPixels()/UnlockPixels()` and
`dirty_bounds()/ClearDirtyBounds()`; the host blits only the damaged region and
skips clean frames entirely.

**Shipped, flag-level** (01805da): `mbViewIsDirty` via
ScheduleNonCompositedAnimation, snapshot semantics; `mbRepaintToBitmap`'s
0-return strictly means "buffer untouched". Dirty RECTS and an engine-owned
lockable surface remain open.

**Related paint-purity rule:** the interactive paint used to end its drive tick
with `RunUntilIdle` *after* the lifecycle update — drained tasks (queued input)
re-dirtied style and the paint replayed an empty record (fixed: the lifecycle
pass is now unconditionally last). Keep "advance the world" (`mbUpdate`) and
"snapshot pixels" (paint) separate so this class of bug can't return.

### 3. Async loads with per-frame lifecycle callbacks

**Then:** `mbLoadHTML` pumped until the `load` event and consumed subresources
inline, and `mbOnLoadFinish` fired from inside the caller's own call. Glyph had
to build a concurrent prefetch layer that downloads every referenced resource
*before* calling load, gate the load on that, and add a decline-not-block rule
for unscanned URLs — all to keep the engine's thread off the network during the
synchronous window.

**Ultralight:** `LoadHTML/LoadURL` return immediately; the host receives
`begin-loading` / `finish-loading` / `fail-loading` / `window-object-ready` /
`DOM-ready`, each carrying `frame_id` + `is_main_frame`.

**Shipped** (e047aff): `mbOnBeginLoading` (main-frame commit) +
`mbOnFailLoading` (top-level failure funnel, with `last_error_`).
mbLoadHTML/mbLoadURL already return before the load event, so no separate
Async variants were needed.

### 4. Per-view resource hooks instead of process-wide ones

**Then:** the request-mock hook (and the dynamic request/response hooks) were
process-wide with no view parameter — `ctx` always null. A host with two views
and different resource providers could only demultiplex by URL; Glyph routed
via synthetic per-dictionary hosts baked into the base URL.

**Ultralight:** platform services (FileSystem, FontLoader, Logger, Clipboard,
GPUDriver, SurfaceFactory) are injectable interfaces, and every view callback
carries `user_data`.

**Shipped, loader path** (c395cc8): `mbOnRequestMock(view, cb, userdata)` —
MbFindMock takes an opaque host context, subresource loaders inherit the view
from their frame client, hook self-erases on view destroy. Worker scripts and
view-level MbFetchUrl helpers still fall back to the process-wide hook. The
static block/mock/rewrite tables stay global — they are config, not routing.

### 5. Typed input events

**Then:** Win32 heritage — `message` ints + flag bits for mice, wheel deltas in
120-unit ticks, no gesture phases. Glyph's shim accumulated fractional trackpad
deltas, faked `kPhaseNone`-style wheels (a phased wheel would route to the
absent compositor gesture generator and silently not scroll), and scaled
physical→CSS px by hand.

**Ultralight:** `ulViewFireMouseEvent(view, ULMouseEvent)` /
`ULScrollEvent{ScrollByPixel|ScrollByPage}` / `ULKeyEvent` — typed structs with
explicit units.

**Shipped** (01805da): `mbMouseEvent` / `mbWheelEvent` with struct_size
versioning, float deltas, reserved phase.

### 6. Split the embedder core from the automation kit

**Then:** one 1065-line header, ~190 exports, embedding primitives interleaved
with automation/testing tools (`mbWaitForSelector`, `mbSavePdf`,
`mbDownloadURL`, proxy config, request log). Several automation calls pump
(`mbWait`, `mbWaitFor*`) — dangerous in an interactive host, and nothing in the
header said so.

**Ultralight:** small per-domain headers (mirrored in `CAPI_*`), `Config` vs
`ViewConfig` split, AppCore as an optional convenience layer on top of the
core.

**Shipped**: `include/miniblink2/{webview.h,automation.h}` (117 embedder / 79
automation exports, conservation-checked). No umbrella — every consumer names
its audience; all in-repo consumers repointed. Pumping calls are flagged in
automation.h.

---

## Round 2

Second pass over Ultralight 1.4's SDK, through the corners round 1 skipped:
Session, platform Config, Buffer ownership, the documented JS lifecycle,
Renderer memory hooks.

### 7. Time-bounded update slice

**Then:** `mbUpdate` (item 1) was re-entrancy-safe but drained until idle — a
busy page (JS timer storms, decode bursts) could hold the host's frame tick
arbitrarily long. For a popup that lives on hover latency, one bad page means
visible jank.

**Ultralight:** `Config::max_update_time = 1/200s` — `Update()` stops
dispatching when the budget is spent; remaining work waits for the next tick.

**Shipped** (871a40b): `mbSetMaxUpdateTime(double ms)` (0 = unbounded); delayed
hard-quit races quit-on-idle. `mbPumpMessages` stays unbounded for the
automation/wait paths, which WANT run-to-idle. Glyph runs an 8 ms budget.
This completes the interactive-tick story: safe (item 1) and bounded (this).

### 8. Engine-level user stylesheet

**Then:** hosts injected presentation CSS by editing the document: Glyph
prepended a `<style>` prelude (scrollbar styling, background) to every
dictionary page and spliced `userCSS` into the HTML before load. The page's own
JS could see the injected markup, and the splice had to be redone every load.

**Ultralight:** `Config::user_stylesheet` — host CSS applied below every
document, engine-side.

**Shipped** (871a40b): `mbSetUserStylesheet(mbView*, const char* css)` via
StyleEngine::InjectSheet, re-applied per commit. ADOPTION CAVEAT: user origin
ranks below author styles without `!important` — Glyph keeps its author-level
prelude until the CSS is hardened (or a kAuthor variant is added).

### 9. Zero-copy resource bodies

**Today:** `mbRequestMockResponse` / the response hooks copy body bytes into
the engine. A content-serving host pays double: Glyph's prefetch cache holds
each dictionary font/image (MBs per page), then the engine copies the same
bytes again on every serve.

**Ultralight:** `Buffer::Create(data, size, user_data, DestroyBufferCallback)`
— the host hands bytes over WITH an owner; the engine calls the destructor when
done. No copy.

**Proposal:** `mbResponseSetBodyOwned(job, const void* data, size_t len,
void (*destroy)(void* ud, const void* data), void* ud)` alongside the copying
setter; the mock/serve path keeps a reference until the load consumes it, then
fires `destroy`.

**Deferred by cost**: the body is a std::string threaded through
MbFindMock/MbFetchUrl/the async deliver path; an owned-buffer type means
retyping that plumbing end to end, against a measured saving of one memcpy of
already-cached bytes per serve (~ms per page). Revisit if a host serves large
media.

### 10. Memory pressure API

**Then:** no way to tell the engine "the UI is hidden, shrink" or to see where
memory went. Pooled hidden views retained decoded images, JS heap, caches
indefinitely — for a menu-bar app the resident-size expectation is small.

**Ultralight:** `Renderer::PurgeMemory()`, `Renderer::LogMemoryUsage()`, and
config budgets (`memory_cache_size`, `page_cache_size`, `override_ram_size`,
`recycle_delay`).

**Shipped** (871a40b): `mbPurgeMemory` — critical pressure broadcast + V8
low-memory GC; `mbLogMemoryUsage` — coarse V8/malloc log. Budget knobs can
follow if the purge call proves insufficient.

### 11. JS exception channel + documented binding lifecycle

**Then, two gaps.** (a) `mbEvalJSEx` returned value+type but swallowed errors —
a throwing script was indistinguishable from one returning undefined. (b)
Nothing documented when `mbJsBindFunction` bindings and other JS state die
(they die with the context on navigation); item 3's `mbOnBeginLoading` is the
re-establishment point but nobody said so.

**Ultralight:** `EvaluateScript(script, String* exception)` returns the
exception text; the docs state the JSContext resets per navigation and name
window-object-ready as the re-init hook.

**Shipped** (871a40b): `mbEvalJSCatch` (message + line; empty exception =
success); binding-lifecycle contract documented in webview.h
(`mbSetInitScript` survives navigations — now said so).

### 12. Sessions (storage profiles)

**Then:** the pieces existed — per-frame storage keys partition origin-keyed
services, and `mbSave/LoadCookies|LocalStorage|IndexedDB|OPFS` snapshot state
manually — but there was no profile object: all views shared one storage world
and persistence was a hand-rolled save/load dance per service.

**Ultralight:** `Renderer::CreateSession(is_persistent, name)`; a view is
created into a session; cookies/storage/IDB isolate per named profile,
in-memory or disk-backed.

**Shipped, all three stages** — THE SESSIONS DESIGN IS COMPLETE:
- Stage 1: MbSession identity + refcount/detach lifetime, default session,
  `mbCreateViewInSession`/`mbViewGetSession`, session-id prefix on the storage
  partition scope (DOM storage/IDB/OPFS/buckets/locks isolate per profile).
- Stage 2: per-session curl cookie jars (keyed shares; fetch paths resolve via
  the view registry, document.cookie via the session-prefixed scope; unknown
  contexts alias the default jar).
- Stage 3: persistent profiles restore at create and flush at
  `mbSessionFlush`/teardown (cookies + prefix-filtered IndexedDB + OPFS under
  persist_dir); `mbSessionClearStorage` wipes the profile. localStorage is
  blink-internal: not persisted per session (documented).

See "Sessions: the agreed design" below for the API contract.

### 13. Smaller notes: font defaults, update timestamp, inspector

- **Per-view font-family defaults** (`font_family_standard/fixed/serif/
  sans_serif`, `font_gamma` in Ultralight): given the last-resort-font fix this
  engine already needed, CJK-aware per-view fallback defaults are natural
  hardening for dictionary-style content. **Shipped**: `mbSetFontFamilies`.
- **Per-display animation cadence**: Ultralight's `RefreshDisplay(display_id)`
  ties rAF to a monitor's vsync. **Shipped**: `mbUpdateAt`.
- **Inspector**: **Stage A shipped (draft)** — in-process CDP pipe
  (`mbDevToolsAttach/Send/Detach`) driving blink's DevToolsAgent standalone,
  verified by a Runtime.evaluate round trip. Root causes fixed along the way:
  null blink::String mojo params (validation-dropped), JSON-vs-CBOR command
  encoding, a null LayerTreeDebugState overlay deref (patch 0024), a null
  WidgetInputHandlerManager deref on session detach (patch 0025), and needing a
  real non-associated primary pipe. **Stage B open**: the host WebSocket +
  /json bridge (embedder-side) is not in this tree — no
  MiniblinkDevToolsBridge/GLYPH_DEVTOOLS code exists here yet. See "Inspector:
  the staged plan" below.

---

## What NOT to copy from Ultralight

- The engine trade: single-dylib real Chromium (M150 Blink + V8) beats a
  trimmed WebKit fork on web compat by a mile; nothing here argues for a
  smaller engine.
- Response mocking as the interception primitive is *stronger* than
  Ultralight's FileSystem for content-serving hosts (dictionary/media apps);
  keep it — just per-view (§4, shipped).
- The wait/shot automation helpers are a genuine differentiator for
  headless/testing users; the split in §6 keeps them first-class without
  endangering interactive hosts.
- A C++ RefPtr-style surface: the flat C ABI is the right call for a
  dlopen-able engine; ownership rules just need to stay explicit in comments.

---

## Sessions: the agreed design (item 12)

Decided 2026-07-04, optimizing for API quality over implementation cost.

```c
typedef struct mbSession mbSession;

// A browsing profile. `name` is its stable identity (and its directory name).
// persist_path == NULL  -> EPHEMERAL: memory only, gone at teardown.
// persist_path != NULL  -> PERSISTENT: durable under <path>/<name>/.
// Same (name, path) later reopens the same profile. The mode is fixed for the
// session's lifetime (no switching - that is how half-migrated profiles happen).
MB_EXPORT mbSession* mbCreateSession(const char* name, const char* persist_path);

// Safe with views still open: the handle detaches; storage tears down when the
// last view in the session closes. No destroy-order footgun.
MB_EXPORT void mbDestroySession(mbSession*);

MB_EXPORT mbView* mbCreateViewInSession(int width, int height, mbSession*);
MB_EXPORT mbSession* mbViewGetSession(mbView*);
MB_EXPORT mbSession* mbDefaultSession(void);      // the implicit ephemeral one

MB_EXPORT void mbSessionClearStorage(mbSession*); // logout-everything wipe
MB_EXPORT void mbSessionFlush(mbSession*);        // durability barrier
```

Design properties:
- Sessions are CAPABILITY HANDLES, not name strings in a global registry —
  whoever holds the handle controls the profile.
- Ephemeral vs persistent is a creation-time binary, visible at the call site.
- COOKIES ARE IN, unconditionally: account isolation is the reason sessions
  exist; per-session cookie jars in the curl layer are an accepted consequence.
- Storage completeness: everything origin-keyed partitions by session
  (cookies, local/session storage, IndexedDB, OPFS, CacheStorage,
  BroadcastChannel/locks). The HTTP byte cache may stay shared — it is
  content-addressed, not identity-bearing.
- Default session is EPHEMERAL and implicit: plain mbCreateView keeps working,
  nothing touches disk unless a host opts in (privacy-safe default).
- The mbSave/Load* snapshot pairs live on in automation.h as export/import
  TOOLS; sessions are the ownership model, snapshots are operations on one.

Implementation staging (the design is the contract; durability matures):
1. Session object + default session + view binding; session id prefixes the
   existing frame-origin partition scope, isolating every origin-keyed
   service in memory; ClearStorage.
2. Per-session cookie jars (curl share handles keyed by session).
3. Persistence: restore at create; durability barriers at mbSessionFlush /
   destroy first, converging to write-through per service.

All three stages shipped — see item 12.

## Inspector: the staged plan (item 13c)

Scoped 2026-07-04. The surprise that makes this tractable: blink's inspector
core (DevToolsAgent / DevToolsSession, renderer/core/inspector) already
compiles into libminiblink2 — the CDP *backend* is in the binary, unexported.
And with a CDP endpoint speaking the standard /json discovery protocol,
ORDINARY CHROME is the frontend (devtools://devtools/bundled/inspector.html
connects to any ws:// CDP target) — bundling/building devtools-frontend is
unnecessary.

- **Stage A — engine CDP pipe.** Bind the main frame's mojo DevToolsAgent
  in-process and expose it flat:
    typedef void (*mbDevToolsMessageCallback)(mbView*, void* userdata,
                                              const char* json, int len);
    MB_EXPORT int  mbDevToolsAttach(mbView*, mbDevToolsMessageCallback, void*);
    MB_EXPORT void mbDevToolsSend(mbView*, const char* json, int len);
    MB_EXPORT void mbDevToolsDetach(mbView*);
  One session per view; messages are CDP JSON both ways. The work is mojo
  plumbing (session channel, IO-vs-main routing) — the protocol itself is
  blink's.

- **Stage B — host WebSocket bridge.** The embedder (not the engine) serves
  ws://127.0.0.1:<port>/ + /json/list, pumping frames to/from the pipe. In
  Glyph: a debug menu item "Inspect dictionary popup" that starts the bridge
  and copies the devtools:// URL. Keeps sockets out of the engine.

- **Stage C — bundled frontend: intentionally skipped.** Chrome is the
  frontend; shipping one inside the SDK adds megabytes and a TypeScript
  build for zero capability.

Status: Stage A shipped (draft). Stage B (the host WebSocket bridge) is the
open item; it lives in the embedder, not this repo's engine sources.
