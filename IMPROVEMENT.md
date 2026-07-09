# API improvements for embedders

Lessons from integrating miniblink2 into a real interactive host (Glyph's
dictionary popup), cross-checked against comparable embedded-engine SDKs
(offscreen surface, flat C API). Every item
maps to a real integration incident, not a style preference; each round is
ordered by leverage-per-effort for the Glyph host.

Round 1 (items 1–6) is fully shipped. Round 2 (items 7–13) is shipped except
zero-copy bodies (item 9, deferred by cost); the inspector's host-side bridge
(stage B) shipped in the Glyph embedder, where the plan places it. Round 3
(items 14–21, the page → host UI-state channel and the platform-service
corners) is fully shipped. Round 4 (items 22–31, the corners rounds 1–3
skipped: child views/window.open, per-view JS, typed keys, paint-contract
hardening) is fully shipped. Round 5 (items 32–39, the from-scratch fifth
re-read: window-object-ready, structured load errors, the OS-clipboard bridge,
host font bytes, the creation-time view config, the mutable request handle) is
fully shipped except mbAddFontData on Windows (mac shipped; DWrite private
collection pending). Per-item status is inline under each item.

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

**Prior art:** `Renderer::Update()` dispatches ready timers/tasks and
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

**Prior art:** views render into a `Surface` (host-pluggable via
`SurfaceFactory`) with `LockPixels()/UnlockPixels()` and
`dirty_bounds()/ClearDirtyBounds()`; the host blits only the damaged region and
skips clean frames entirely.

**Shipped, flag-level** (01805da): `mbViewIsDirty` via
ScheduleNonCompositedAnimation, snapshot semantics; `mbRepaintToBitmap`'s
0-return strictly means "buffer untouched".

**Shipped, rect-level**: `mbViewGetDirtyRect` — the damaged region of the last
successful repaint (empty = bit-identical frame, skip the blit; full view when
unknown: first paint / resize / SetDirty / force-repaint / composited). Blink's
own `RasterInvalidator` diffs the persistent PaintArtifact across paint cycles
(whole document as one layer in Root space — the space the software paint
replays in); the diff runs inside the paint cycle via the non-composited paint
hook (patch 0041), the only window where the previous artifact's backing store
is still alive. An engine-owned lockable surface remains open.

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

**Prior art:** `LoadHTML/LoadURL` return immediately; the host receives
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

**Prior art:** platform services (FileSystem, FontLoader, Logger, Clipboard,
GPUDriver, SurfaceFactory) are injectable interfaces, and every view callback
carries `user_data`.

**Shipped, all fetch paths**: `mbOnRequestMock(view, cb, userdata)` —
MbFindMock takes an opaque host context, subresource loaders inherit the view
from their frame client, hook self-erases on view destroy (c395cc8, loader
path). The former residuals are closed: view-level fetch helpers (downloads),
page-navigation mock checks, and worker main scripts now carry the view —
dedicated workers via the creating document's window, shared workers via the
starting connection's frame_key. The same context keys the session cookie
jar, so worker script fetches also use the right session's cookies. Remaining
residual: a NESTED worker's script (its context is a WorkerGlobalScope, no
frame) falls back to the process-wide hook — documented in webview.h. The
static block/mock/rewrite tables stay global — they are config, not routing.
Smoke: a per-view mock serving a dedicated worker's http(s) script
(mb_smoke_render 37j2).

### 5. Typed input events

**Then:** Win32 heritage — `message` ints + flag bits for mice, wheel deltas in
120-unit ticks, no gesture phases. Glyph's shim accumulated fractional trackpad
deltas, faked `kPhaseNone`-style wheels (a phased wheel would route to the
absent compositor gesture generator and silently not scroll), and scaled
physical→CSS px by hand.

**Prior art:** `ulViewFireMouseEvent(view, ULMouseEvent)` /
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

**Prior art:** small per-domain headers (mirrored in `CAPI_*`), `Config` vs
`ViewConfig` split, an app layer as an optional convenience tier on top of the
core.

**Shipped**: `include/miniblink2/{webview.h,automation.h}` (117 embedder / 79
automation exports, conservation-checked). No umbrella — every consumer names
its audience; all in-repo consumers repointed. Pumping calls are flagged in
automation.h.

---

## Round 2

Second prior-art pass, through the corners round 1 skipped:
Session, platform Config, Buffer ownership, the documented JS lifecycle,
Renderer memory hooks.

### 7. Time-bounded update slice

**Then:** `mbUpdate` (item 1) was re-entrancy-safe but drained until idle — a
busy page (JS timer storms, decode bursts) could hold the host's frame tick
arbitrarily long. For a popup that lives on hover latency, one bad page means
visible jank.

**Prior art:** `Config::max_update_time = 1/200s` — `Update()` stops
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

**Prior art:** `Config::user_stylesheet` — host CSS applied below every
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

**Prior art:** `Buffer::Create(data, size, user_data, DestroyBufferCallback)`
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

**Prior art:** `Renderer::PurgeMemory()`, `Renderer::LogMemoryUsage()`, and
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

**Prior art:** `EvaluateScript(script, String* exception)` returns the
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

**Prior art:** `Renderer::CreateSession(is_persistent, name)`; a view is
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
  sans_serif`, `font_gamma` in the prior art): given the last-resort-font fix this
  engine already needed, CJK-aware per-view fallback defaults are natural
  hardening for dictionary-style content. **Shipped**: `mbSetFontFamilies`.
- **Per-display animation cadence**: a per-display `RefreshDisplay(display_id)`
  ties rAF to a monitor's vsync. **Shipped**: `mbUpdateAt`.
- **Inspector**: **Stage A shipped** (8bc330c) — in-process CDP pipe
  (`mbDevToolsAttach/Send/Detach`) driving blink's frame-owned DevToolsAgent
  over in-process mojo: JSON↔CBOR transcoding with crdtp validation
  (malformed/id-less commands rejected, not DCHECKed), interrupt-class
  commands routed via the IO session (content's ShouldSendOnIO list),
  empty-but-non-null session_id so a directly-connected frontend receives
  events, detach-before-Close teardown. Root causes fixed along the way:
  null blink::String mojo params (validation-dropped), JSON-vs-CBOR command
  encoding, a null LayerTreeDebugState overlay deref (patch 0024), a null
  WidgetInputHandlerManager deref on session detach (patch 0025), a null
  addScriptToEvaluateOnNewDocument identifier from browserless clients
  (patch 0026), and needing a real non-associated primary pipe.
  **Stage B shipped in the Glyph embedder** (not this repo, by design):
  Stage A was verified end-to-end against a real Chrome
  Elements/Console/Sources session over the embedder's WebSocket bridge
  (live DOM updates across reloads), plus Runtime.evaluate round trips,
  detach/re-attach, destroy-while-attached, and a second-view session.
  **Pause/resume notification shipped**: `mbOnDevToolsPaused(view, cb, ud)`
  fires paused=1/0 from blink's MainThreadDebuggerPaused/Resumed — the host
  can stop its frame tick instead of treating a breakpoint as a hang;
  Debugger.resume from inside the callback is safe (IO session). Needed
  patch 0027: blink CHECK-aborts DebuggerPaused on a worker-style-bound
  frame agent (the browserless binding) and would deliver the signal only to
  the absent associated channel. Smoke: mb_smoke 42 (P→R around a `debugger`
  statement). **Remaining engine-side gap**: ChildTargetCreated — child
  worker/iframe targets are not surfaced (single-target v1). See "Inspector:
  the staged plan" below.

---

## Round 3

Third pass (2026-07-07), through the corners rounds 1–2 skipped: Listener.h's
ViewListener (the page → host UI-state channel), the platform/ services,
ViewConfig, CAPI conventions. De-duplicated against the shipped header first —
the prior art's title/URL/console/new-window/download/clipboard/device-scale
equivalents already exist as mb* exports; what follows is only what's
genuinely missing.

### 14. Cursor + tooltip: the page → host pointer-UI channel

**Then:** an offscreen view has no OS window, so nothing tells the host the
page wants an I-beam over selectable text, a hand over a link, or a hover
tooltip. Glyph's popup shows a static arrow everywhere; title/URL changes are
pushed (round 1 era) but pointer UI state is not.

**Prior art:** `ViewListener::OnChangeCursor(View*, Cursor)` (a 40+-value
cursor enum) and `OnChangeTooltip(View*, const String&)`.

**Shipped**: `mbOnCursorChanged(view, cb, ud)` — fires with an MB_CURSOR_*
code (pointer/hand/ibeam/wait/help/resize/zoom/grab…, the full blink
ui::mojom::CursorType set flattened) whenever blink updates the cursor over
the view; fires only on change. `mbOnTooltipChanged(view, cb, ud)` — UTF-8
tooltip text, empty string = hide. Both NULL-clear, both snapshot per commit.

### 15. window.close() surfacing

**Then:** a page calling `window.close()` was silently ignored — the host
popup can't honor "the page dismissed itself" (dictionary pages with a close
button; OAuth flows that close their window).

**Prior art:** `ViewListener::OnRequestClose(View*)`.

**Shipped**: `mbOnRequestClose(view, cb, ud)` — notification only; the host
decides whether to hide/destroy the view.

### 16. Input-focus query for keystroke routing

**Then:** `mbSetFocus` sets window focus, but a host with global hotkeys
(menu-bar app) has no way to ask "does the page actually have a caret?" —
so every keystroke either goes to the page or to the host by static policy.

**Prior art:** `View::HasInputFocus()` — "visible keyboard focus (blinking
caret); use this to decide whether the View should consume keyboard input."

**Shipped**: `mbHasInputFocus(view)` — 1 when the focused element accepts
text input (editable / text control), i.e. the page would consume a
keystroke; the host routes keys to the page only then.

### 17. History-state push notification

**Then:** `mbCanGoBack/Forward` exist but are poll-only — a host enabling
nav buttons re-queries every tick.

**Prior art:** `ViewListener::OnUpdateHistory(View*)`.

**Shipped**: `mbOnHistoryChanged(view, cb, ud)` — fires with
(can_go_back, can_go_forward) whenever the session history changes; hosts
update buttons event-driven.

### 18. Version handshake exports

**Then:** a dlopen-ing host had no way to verify at load time which engine
it bound — mismatched dylib/header pairs fail at first symbol or silently.

**Prior art:** a version string plus numeric Major/Minor/Patch exports and
`WebKitVersionString()` — engine AND upstream-engine versions, exported flat.

**Shipped**: `mbVersion()` (engine version string), `mbApiVersion()`
(header/ABI integer, bump on breaking change), `mbChromiumVersion()`
("150.0.x.y") — all callable before mbInitialize.

### 19. Host log sink

**Then:** engine diagnostics (base logging, mbLogMemoryUsage output) went to
stderr unconditionally — invisible to a GUI host, unroutable to its logs.

**Prior art:** injectable `Logger::LogMessage(LogLevel, const String&)`.

**Shipped**: `mbOnLogMessage(cb, ud)` — process-wide sink receiving
(level, message); NULL restores stderr. Installed via base logging's message
handler, so LOG(ERROR)-class engine output lands in the host's logs.

### 20. Per-character font fallback hook

**Then:** item 13 shipped static per-view family defaults
(`mbSetFontFamilies`) — but fallback for a specific missing glyph
(U+9F98 in a rare-CJK dictionary entry) still walks blink's platform list and
can land on last-resort. The engine already needed a last-resort-font fix once.

**Prior art:** `FontLoader::fallback_font_for_characters(const String&
characters, int weight, bool italic)` — the host answers "what font renders
these characters?" at glyph-resolution time.

**Shipped**: `mbSetFontFallbackCallback(cb, ud)` — process-wide; consulted
with (codepoint, weight, italic) BEFORE the platform cascade (patch 0029 adds
`FontCache::SetHostFallbackFontHook`, consulted at the top of the mac
`PlatformFallbackFontForCharacter`). The named family must actually cover the
character (unicharToGlyph guard) or the answer is ignored — a wrong host
answer falls through instead of rendering tofu. Smoke: mb_smoke 44g (real
family used; bogus family tofu-guarded).

### 21. Noted, not adopted (round-3 counterpart of "what NOT to copy")

- **TLS pinning** (`NetworkRequest::EnforcePinnedPublicKey` →
  `CURLOPT_PINNEDPUBLICKEY`): nearly free in the curl layer, but no host asks
  for it; the per-request allow/deny half already exists (mbOnNavigation +
  request callbacks). Deferred until a host pins.
- **Streaming download lifecycle** (chunked OnReceiveDataForDownload +
  CancelDownload): ~~deferred~~ **shipped** — `mbOnDownloadStream` (begin →
  data chunks with received/expected progress → finish) + `mbDownloadURLStream`
  + `mbCancelDownload`, on an MbSseStream-pattern curl worker. While the sink
  is registered it takes over all page-initiated downloads from mbOnDownload;
  blob/data:/mocked/navigation bodies (already materialized) deliver the same
  lifecycle from memory. The whole-body response hook does not apply to
  streamed bodies (documented); item 9's zero-copy remains separate.
- **ImageSource** (host-registered decoded/GPU images referenced by URL):
  elegant inverse of the mock hook, heavy to build in blink. Noted only.
- **ThreadFactory / Allocator override**: QoS-tagging engine threads is
  attractive for a menu-bar host, but blink's thread bring-up is not
  pluggable at reasonable cost. (Prior art keeping its Allocator flat C even
  inside a C++ SDK re-validates the flat-ABI stance.)
- **RenderOnly(views[])**: pooled hidden views already skip work via
  mbViewIsDirty gating host-side.
- **A `CreateSession(is_persistent, name)` shape** hangs persistence off
  a global cache_path — the shipped per-session persist_path (item 12) is
  strictly better. No revision needed.

---

## Round 4

Fourth pass (2026-07-07), through the headers rounds 1–3 didn't dissect:
Surface/GPUDriver/Bitmap/RenderTarget, View/Renderer corners, KeyEvent,
Session getters, Config diagnostics, the app layer. De-duplicated against
the shipped mb* surface and the open/deferred tables first.

### 22. Child views: window.open() that actually works

**Then:** `mbOnNewWindow` is notification-only — `window.open()` returns null,
severing the opener/`postMessage` relationship. Any login-in-popup / OAuth
flow that opens a child window and posts back to its opener is dead; the host
loading the URL in a fresh view doesn't restore the link.

**Prior art:** `ViewListener::OnCreateChildView(caller, opener_url,
target_url, is_popup, popup_rect)` returns a `RefPtr<View>` — the host
supplies the view, the engine wires it as the opener's child.

**Shipped** (two-phase, the honest C-ABI shape): `mbOnCreateChildView(view,
cb, ud)` — the engine creates the child (opener page passed to
WebView::Create, opener frame to CreateMainFrame, SetOpenedByDOM; it inherits
the parent's session and shares the opener's agent group scheduler — same
agent cluster); the host returns 1 to ADOPT (owns the view) or 0 to decline
(deferred engine teardown off the window.open stack; window.open == null; no
single-window default navigation — a registered mbOnNewWindow still fires).
Adopted children: live window object, working opener/postMessage both ways,
window.close() allowed. LIFETIME: destroy the child before its parent.
Landed along the way: a restore-merge bug in IndexedDB session persistence
(merge retired EVERY live backend but only replaced same-key slots, leaving
null registry entries that crashed the next whole-registry flush) — fixed to
retire only replaced backends. Smoke: mb_smoke (adopt + postMessage-to-opener
+ geometry + decline).

### 23. Per-view JavaScript toggle

**Then:** `mbSetLoadImages` exists, but no way to disable script — the one
ViewConfig boolean with no mb counterpart. Script-off is hardening AND a perf
switch for static dictionary HTML.

**Prior art:** `ViewConfig::enable_javascript`.

**Shipped**: `mbSetEnableJavascript(view, int)` via
`WebSettings::SetJavaScriptEnabled`, call-before-load semantics like
mbSetLoadImages. CAVEAT (documented): blink reads the live setting, so while
disabled HOST eval is refused too — re-enable to script the document.

### 24. Typed keyboard event — the unfinished half of item 5

**Then:** item 5 shipped `mbMouseEvent`/`mbWheelEvent`, but keys are still
string shorthands (`mbSendKey/Ex/Text/KeyUp`) — can't express auto-repeat,
keypad distinction, down-without-up, or unmodified_text for shortcut
resolution.

**Prior art:** `KeyEvent{type(RawKeyDown|KeyUp|Char), modifiers,
virtual_key_code, native_key_code, text, unmodified_text, is_keypad,
is_auto_repeat, is_system_key}` plus an `NSEvent` constructor.

**Shipped**: `mbKeyEvent` struct (struct_size-versioned) →
`mbSendKeyEvent(view, const mbKeyEvent*)`; the header documents the
NSEvent→field mapping (no ObjC types in the C ABI). One WebKeyboardEvent per
call; MB_KEY_DOWN with text types the character (blink splits it into
RawKeyDown+Char itself). Smoke: typed text + auto-repeat flag.

### 25. Pixel-format contract: state the alpha semantics

**Then:** paint docs say "BGRA8888" and never state premultiplied-vs-straight
alpha or color space — with `mbSetTransparentBackground` shipped, a
compositing host MUST know.

**Prior art:** `BGRA8_UNORM_SRGB` ("sRGB gamma with premultiplied linear
alpha") stated on every pixel surface, plus straight↔premultiplied
converters.

**Shipped** (docs): the paint exports and the header-top pixel contract now
state BGRA, PREMULTIPLIED alpha, sRGB.

### 26. Host-forced repaint: the dirty setter

**Then:** `mbViewIsDirty` shipped (item 2) but there's no setter — a host
that loses its buffer (purged CALayer on hide/show, resize) can't say
"repaint even though you think you're clean"; the damage-gated blit skips
forever. Exactly the pooled-hidden-views scenario Glyph has.

**Prior art:** `View::set_needs_paint(bool)`.

**Shipped**: `mbViewSetDirty(view)`.

### 27. Load/history nuances

- `LoadHTML(html, url, bool add_to_history)`. **Shipped**:
  `mbLoadHTMLEx(view, html, base, add_to_history)` — add_to_history=0
  REPLACES the current entry (location.replace semantics).
- `GoToHistoryOffset(int offset)`. **Shipped**: `mbGoToOffset(view, int)`.

### 28. Force-repaint diagnostic switch

**Prior art:** `Config::force_repaint` — "continuously repaint regardless
of dirty; used to diagnose painting issues." Given the damage-gated blit was
the source of a shipped flicker bug (item 2), an escape hatch for "the dirty
flag is lying" is cheap insurance. **Shipped**: `mbSetForceRepaint(view,
int)` — while on, `mbViewIsDirty` reports 1.

### 29. Session introspection

**Then:** `mbSession*` has zero read-back — a host holding several handles
can't ask which is which.

**Prior art:** `Session::is_persistent()/name()/id()/disk_path()`.

**Shipped**: `mbSessionGetName`, `mbSessionIsPersistent`,
`mbSessionGetPersistPath`.

### 30. Convention notes (round 4)

- **One stated threading contract** at the top of webview.h (prior art puts
  it on the class); ours was restated ad hoc per function. Same for the
  logical-vs-physical px / DPR contract. **Shipped** (header-top paragraph).
- **`MB_VERSION` string macro** beside runtime `mbVersion()` so hosts log
  compiled-against vs loaded. **Shipped**.
- **Creation-time config struct** (`mbCreateViewEx(w,h,const mbViewConfig*)`)
  would structurally kill the "call before navigating" footgun class — adopt
  only if the creation surface grows again; not worth churn today.

### 31. Round-4 anti-patterns (what NOT to copy)

- License-tier doc gating (paid-tier `@pre` notes on functional-looking
  exports) — keep every exported symbol functional.
- `#pragma pack(push,1)` on public ABI structs (RenderTarget.h) — the
  struct_size convention is strictly better.
- Modal `ShowMessageBox` in the SDK (Dialogs.h) — re-enters the nested
  run-loop world item 1 eliminated; host-layer concern.
- Gamepad surface — their market (game engines), not ours; scope discipline.

---

## Round 5

Fifth pass (2026-07-09): a from-scratch re-read of the full 1.4 SDK — core
C++ API, platform layer, C-API conventions, app layer — de-duplicated against
rounds 1–4, the open/deferred tables, and the anti-pattern lists. The pass
also *validated* several standing decisions (see item 39) and confirmed one
suspected bug is not one: PNG export already converts premultiplied →
straight alpha (`gfx::PNGCodec::EncodeBGRASkBitmap` unpremultiplies), now
stated in the docs (item 38).

### 32. Window-object-ready hook

**Then:** item 11 documented binding re-establishment as "re-establish from
`mbOnBeginLoading` / `mbOnDOMContentLoaded`" — but BeginLoading fires before
the new document's window object exists, and DOMContentLoaded fires *after*
the page's own scripts ran. `mbSetInitScript` covers the declarative case;
host-*computed* per-document setup (a fresh token, state that isn't known
until the callback) had no sanctioned moment.

**Prior art:** `OnWindowObjectReady` — "called before any scripts are
executed on the page and is the earliest time to setup any initial
JavaScript state or bindings."

**Shipped**: `mbOnWindowObjectReady(view, cb, ud)` — fires per committed
main-frame document from the engine's document-start point
(`RunScriptsAtDocumentElementAvailable`), after the built-in shims and the
init script, before any page script. `mbRunJS`/`mbEvalJS` from inside the
callback execute INLINE (an `in_document_start_` flag routes RunInFrameTask
around its nested pump — pumping mid-commit would drain load-machinery
tasks). Fixed along the way: a subframe's document-element-available used to
re-run the MAIN frame's shims + init script into its current document (once
per iframe document — a double-run for non-idempotent init scripts); the
call site is now main-frame-only. Smoke: mb_smoke R5a (order
window-object-ready → DOMContentLoaded; callback-set global visible to the
page's first script).

### 33. Structured load-failure info

**Then:** `mbOnFailLoading` delivers a prose string (`curl_easy_strerror`
output); `mbGetLastError` the same. A host cannot branch on failure *type*
(retry on timeout, report on DNS, ignore on blocked) without string-matching
English.

**Prior art:** `OnFailLoading(..., const String& description,
const String& error_domain, int error_code)`.

**Shipped**: `mbOnFailLoadingEx(view, cb, ud)` delivering `(url,
error_domain, error_code, description)` — domain `"curl"` with the CURLcode
for transport failures, `"file"` for unreadable file: loads, `"network"` for
no-response-at-all, `"blocked"` when the item-37 request hook vetoed the
load. One slot with plain `mbOnFailLoading` (either replaces the other), per
the console-callback precedent; `MbFetchUrl` gained an `out_error_code`
out-param feeding the view's `last_error_domain_/code_`. Recorded design
rule alongside: if per-frame load events ever land, they carry
`(uint64_t frame_id, int is_main_frame)` from day one — the prior art carries
it on every load event; retrofitting it breeds Ex variants. Smoke: mb_smoke R5b
(file + curl domains, Ex-replaces-plain).

### 34. OS-clipboard bridge

**Then:** the clipboard is an in-process jar (`mbSetClipboard` /
`mbGetClipboard`). An interactive host must manually sync the real
pasteboard around every copy/paste — user copies in another app, pastes
into the view, and sees stale text unless the host polled.

**Prior art:** the `Clipboard` platform interface — the engine *pulls*
from the host on paste (`ReadPlainText`) and *pushes* on copy
(`WritePlainText`); the OS clipboard is the host's to own.

**Shipped**: `mbSetClipboardHandler(read_cb, write_cb, ud)` — process-wide
(like the jar it wraps). Page reads (paste, `navigator.clipboard.readText`)
consult `read_cb` (authoritative while installed); page writes (copy/cut,
`writeText`) fire `write_cb` AND still land in the jar, so `mbGetClipboard`
keeps working. Either may be NULL — that direction keeps the in-process
jar. Documented contract: the callbacks fire on the broker's SERVICE thread
(like mbOnLogMessage's) — thread-safe and cheap, marshal to a UI thread
yourself. Smoke: mb_smoke R5c (host-read shadows the jar; page write reaches
both; clearing restores the jar).

### 35. Host font registration (font bytes, not just names)

**Then:** the font story has two of three pieces — static family defaults
(item 13, `mbSetFontFamilies`) and the per-character fallback hook
(item 20) — but both can only *name* families the OS font library already
has. A host that bundles a font (a dictionary app guaranteeing a specific
CJK face regardless of the user's system) has no way to serve the bytes.

**Prior art:** `FontLoader::Load(family, weight, italic) →
RefPtr<FontFile>`, where `FontFile::Create` accepts an in-memory buffer —
the host serves font *data*.

**Shipped (macOS)**: `mbAddFontData(const void* data, int len,
char* out_family, int family_cap)` — skia validates the bytes and reports
the family name; `CTFontManagerRegisterGraphicsFont` registers at process
scope, so the family resolves in CSS, `mbSetFontFamilies`, and the fallback
callback (re-registering the same face is a success no-op). Simpler than
a pull-model host font loader (no blink FontCache surgery) while covering
the bundled-font case. WINDOWS GAP (open): blink's font stack there is
DirectWrite-backed and `AddFontMemResourceEx` fonts are invisible to it —
the export returns 0 honestly; a private DWrite collection is the eventual
answer. Smoke: mb_smoke R5f (registers the tree's Ahem.ttf; a 5-char 20px
run measures exactly 100px — only Ahem's 1em-square glyphs do that).

### 36. Creation-time view config — the trigger fired

**Then:** round 4 (item 30) deferred a creation config "until the creation
surface grows again." It has: `mbSetCompositingEnabled` is a process-global
latch consumed by the NEXT `mbCreateView` — exactly the
action-at-a-distance a creation struct exists to kill — plus session
binding and a family of "call before first load" setters whose timing
contract is prose.

**Prior art (C-API shape):** not a packed struct but an *opaque builder* —
create-config + one setter per field + pass to create + destroy.
New options never change any signature: strictly better ABI evolution than
`struct_size` structs (which version reads, but not the "when does it
apply" question).

**Shipped**: `mbViewConfig* mbCreateViewConfig(void)` with
`mbViewConfigSetSession / SetCompositing / SetTransparentBackground /
SetDeviceScaleFactor / SetEnableJavascript / SetLoadImages /
SetFontFamilies / SetUserAgent / SetDarkMode / SetLocale`, then
`mbCreateViewWithConfig(int w, int h, const mbViewConfig*)` +
`mbDestroyViewConfig`. Compositing is genuinely per-view at creation
(`MbWebView::Create` gained a `compositing_override` that bypasses the
process latch, and the widget attach now uses the same resolved value);
`mbSetCompositingEnabled` stays as a documented legacy latch. Configs are
reusable and destroyable independently of their views. Smoke: mb_smoke R5d
(session/UA/dark/device-scale all applied before the first document).

### 37. Mutable request handle

**Then:** the *response* side already has the right pattern — opaque
`mbResponse*` with Get/Set accessors — but the request side is flat strings
that can only allow/block, plus a static process-wide substring header
table (`mbSetRequestHeader`). Dynamic per-request header injection
(compute an Authorization header per URL) or per-request redirection isn't
expressible.

**Prior art:** `OnNetworkRequest(View*, NetworkRequest&)` — a mutable
request object per fetch.

**Shipped**: opaque `mbRequest*` + `mbSetRequestHook(cb, ud)`: accessors
`mbRequestURL/Method/Headers/Body`; mutators `mbRequestSetUrl` (transparent
rewrite, like `mbRewriteUrl`), `mbRequestSetHeader(name, value)` (replaces a
same-name header rather than duplicating — curl would send both), and
`mbRequestBlock()`. Dispatched on the main thread at BOTH request entries —
the subresource loader (`Deliver`) and the top-level fetch (`MbFetchUrl`) —
after the static block/rewrite/header tables; a blocked top-level load
reports `error_domain "blocked"` through mbOnFailLoadingEx. One slot shared
with `mbSetRequestCallback`/`Ex` (setting any of the three replaces the
others), so the three generations can't fire inconsistently. Smoke:
mb_smoke R5e (SetUrl redirect served by a mock while the page keeps its
original URL; Block → domain "blocked").

### 38. Documentation batch (round-5 convention wins) — **all shipped**

- **Worked host-loop example** at the top of webview.h — the prior art's
  headers open with a compilable create → update → render loop; Glyph's
  contract paragraphs are strong but there is no 15-line "interactive host
  frame tick" (`mbUpdateAt` → `mbViewIsDirty` → `mbRepaintToBitmap` → blit)
  to copy-paste.
- **Per-host-type wiring matrix** — a required/optional/provided
  platform table is its single best doc artifact. Glyph analog: which
  callbacks/setters matter for a screenshot/scrape host vs an interactive
  embedder.
- **Numeric version macros** — `MB_VERSION_MAJOR/MINOR/PATCH` beside the
  string, so hosts can `#if` on them (prior art ships all three plus the
  string).
- **`mbJsNativeFn` return-string lifetime** — currently undocumented (the
  one real ownership gap this pass found). The engine converts the returned
  string before the call returns; a static or per-binding buffer the host
  overwrites on the next call is fine. Say so.
- **`MB_KEY_DOWN` footgun comment** — the prior art annotates its equivalent
  enum value with "you should probably use RawKeyDown instead"; the warning
  belongs *on the value*.
- **Recommended `mbSetMaxUpdateTime`** — the prior art defaults to a bounded
  slice (1/200 s); Glyph defaults to drain-to-idle. Document the
  recommended interactive budget (Glyph runs 8 ms) at the declaration.
- **`mbOnCreateChildView` opener URL** — the prior art passes `opener_url`;
  Glyph's callback already receives the parent view, so `mbGetURL(parent)`
  *is* the opener URL. Document that instead of growing the signature.
- **PNG alpha statement** — verified: `mbSavePng`/`mbEncodePng` write
  straight-alpha PNGs (the encoder unpremultiplies). State it where the
  paint exports warn about premultiplied buffers.

### 39. Noted, not adopted (round 5)

- **Per-display refresh routing** (`ViewConfig::display_id` +
  `RefreshDisplay(id)`): the multi-monitor-correct generalization of
  `mbUpdateAt` — a 60 Hz + 120 Hz host can't drive two views at their real
  cadences today. Defer until a host actually runs mixed-refresh displays;
  the shape is recorded (per-view display group + per-group refresh call).
- **App convenience layer** (App/Window/Overlay as a
  separate optional library): the structural lesson is real (windowless
  core, windowed sugar, distinct export macros per layer), but the
  audience is served by `samples/`; if demand grows, it becomes a samples
  template, not an SDK library.
- **In-engine DevTools WebSocket server** (`StartRemoteInspectorServer`):
  acknowledged as the ergonomic benchmark, but it contradicts the
  deliberate Stage-B decision — sockets live in the embedder; the engine
  stays socket-free. A documented sample bridge is the answer.
- **Console column / source-category enum**: real but requires breaking
  `mbConsoleCallbackEx`'s signature; queued for the next `MB_API_VERSION`
  bump rather than a third console slot.
- **JSHelpers' thread-ambient `SetJSContext`**: recorded as an
  anti-pattern — every JSValue class in that header carries a lifetime
  trap ("must set_context before escaping the callback") that explicit
  context parameters would have avoided. If Glyph ever grows a richer JS
  bridge, contexts stay explicit.
- **FileSystem-style content provider**: re-validated the standing
  decision — response mocking is the stronger interception primitive for
  content-serving hosts. One detail absorbed: the prior art makes *charset*
  an explicit host output alongside MIME; Glyph's mock path already takes
  a full content type (charset included).

---

## What's left (summary)

Everything not listed here is shipped or a documented decision (nested-worker
fallback, localStorage persistence limit, inspector Stage C, the user-origin
stylesheet caveat).

| Item | What's left | Status |
|---|---|---|
| 2 | Engine-owned lockable surface (dirty RECTS shipped: `mbViewGetDirtyRect` diffs the persistent paint artifact via blink's RasterInvalidator — patch 0041 + mb_damage_tracker) | Surface still open; rect level shipped |
| 9 | Zero-copy resource bodies (`mbResponseSetBodyOwned`) | Deferred by cost — revisit if a host serves large media |
| 10 | Memory budget knobs (cache sizes) | Only if `mbPurgeMemory` proves insufficient |
| 13c | Child worker/iframe DevTools targets (multi-session bridge) | Open, unscheduled |
| 21 | TLS pinning / ImageSource (streaming downloads shipped: `mbOnDownloadStream` + `mbDownloadURLStream` + `mbCancelDownload`, chunked with progress) | Deferred by trigger (no host needs them yet) |
| 35 | `mbAddFontData` on Windows (DirectWrite private collection) | Open — mac shipped; the export returns 0 on Windows |

---

## What NOT to copy

- The engine trade: single-dylib real Chromium (M150 Blink + V8) beats a
  trimmed WebKit fork on web compat by a mile; nothing here argues for a
  smaller engine.
- Response mocking as the interception primitive is *stronger* than
  a host FileSystem interface for content-serving hosts (dictionary/media apps);
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

Status: Stage A shipped (8bc330c) and proven against a real Chrome frontend.
Stage B shipped where it belongs — in the Glyph embedder (this repo's engine
sources stay socket-free). Debugger pause/resume host notification shipped
(mbOnDevToolsPaused + patch 0027). Engine-side follow-up, not scheduled:
surface child worker/iframe targets (ChildTargetCreated is a
single-target-v1 no-op).
