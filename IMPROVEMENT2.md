# API improvements, round 2

Second pass over Ultralight 1.4's SDK, this time through the corners round 1
skipped: Session, platform Config, Buffer ownership, the documented JS
lifecycle, Renderer memory hooks. Round 1 (IMPROVEMENT.md) is fully shipped;
these are the next candidates, ordered by leverage-per-effort for the Glyph
host. Same discipline as round 1: every item names the concrete miniblink2/
Glyph behavior it would change, not a style preference.

## 1. Time-bounded update slice

**Today:** `mbUpdate` (round 1) is re-entrancy-safe but drains until idle — a
busy page (JS timer storms, decode bursts) can hold the host's frame tick for
arbitrarily long. For a popup that lives on hover latency, one bad page means
visible jank.

**Ultralight:** `Config::max_update_time = 1/200s` — `Update()` stops
dispatching when the budget is spent, remaining work waits for the next tick.

**Proposal:** `mbSetMaxUpdateTime(double ms)` (0 = unbounded, the current
behavior). `mbUpdate` dispatches ready work until the deadline, then returns;
`mbPumpMessages` stays unbounded for the automation/wait paths, which WANT
run-to-idle. This completes the interactive-tick story: safe (round 1) and
bounded (this).

## 2. Engine-level user stylesheet

**Today:** hosts inject presentation CSS by editing the document: Glyph
prepends a `<style>` prelude (scrollbar styling, background) to every
dictionary page and splices `userCSS` into the HTML before load. The page's
own JS can see the injected markup, and the splice must be redone on every
load.

**Ultralight:** `Config::user_stylesheet` — host CSS applied below every
document, engine-side.

**Proposal:** `mbSetUserStylesheet(mbView*, const char* css)` — a per-view
user-agent-level stylesheet, applied on every navigation, invisible to the
document. Blink has first-class support (injected stylesheets); this mostly
plumbs it. Replaces the string surgery in embedders.

## 3. Zero-copy resource bodies

**Today:** `mbRequestMockResponse` / the response hooks copy body bytes into
the engine. A content-serving host pays double: Glyph's prefetch cache holds
each dictionary font/image (MBs per page), then the engine copies the same
bytes again on every serve.

**Ultralight:** `Buffer::Create(data, size, user_data, DestroyBufferCallback)`
— the host hands bytes over WITH an owner; the engine calls the destructor
when done. No copy.

**Proposal:** `mbResponseSetBodyOwned(job, const void* data, size_t len,
void (*destroy)(void* ud, const void* data), void* ud)` alongside the copying
setter. The mock/serve path keeps a reference until the load consumes it, then
fires `destroy`.

## 4. Memory pressure API

**Today:** no way to tell the engine "the UI is hidden, shrink" or to see
where memory went. Pooled hidden views retain decoded images, JS heap, caches
indefinitely — for a menu-bar app the resident-size expectation is small.

**Ultralight:** `Renderer::PurgeMemory()`, `Renderer::LogMemoryUsage()`, and
config budgets (`memory_cache_size`, `page_cache_size`, `override_ram_size`,
`recycle_delay`).

**Proposal:** `mbPurgeMemory(void)` (drop caches, run a full GC, release
decoded image data — blink has the machinery behind memory-pressure signals)
and `mbLogMemoryUsage(void)` for diagnosis. Budget knobs can follow if the
purge call proves insufficient.

## 5. JS exception channel + documented binding lifecycle

**Today, two gaps.** (a) `mbEvalJSEx` returns value+type but swallows errors —
a throwing script is indistinguishable from one returning undefined; embedders
debug blind. (b) Nothing documents when `mbJsBindFunction` bindings and other
JS state die (they die with the context on navigation); round 1's
`mbOnBeginLoading` is the re-establishment point but nobody says so.

**Ultralight:** `EvaluateScript(script, String* exception)` returns the
exception text; the docs state the JSContext resets per navigation and name
window-object-ready as the re-init hook.

**Proposal:** `mbEvalJSCatch(view, script, out_value, cap, out_exception,
exc_cap)` (empty exception = success), and a lifecycle note on
`mbJsBindFunction` / `mbSetInitScript` / `mbOnBeginLoading` documenting the
reset-and-reinject contract (`mbSetInitScript` already survives navigations —
say so).

## 6. Sessions (storage profiles)

**Today:** the pieces exist — per-frame storage keys partition origin-keyed
services, and `mbSave/LoadCookies|LocalStorage|IndexedDB|OPFS` snapshot state
manually — but there is no profile object: all views share one storage world
and persistence is a hand-rolled save/load dance per service.

**Ultralight:** `Renderer::CreateSession(is_persistent, name)`; a view is
created into a session; cookies/storage/IDB isolate per named profile,
in-memory or disk-backed.

**Proposal:** `mbCreateSession(const char* name, const char* persist_path)` +
`mbCreateViewInSession(w, h, session)`. Subsumes the four save/load pairs
(persistent sessions write through) and gives multi-context hosts isolation
without URL tricks. The biggest item here; the storage-key plumbing is the
foundation it builds on.

## 7. Smaller notes

- **Per-view font-family defaults** (`font_family_standard/fixed/serif/
  sans_serif`, `font_gamma` in Ultralight): given the last-resort-font fix
  this engine already needed, CJK-aware per-view fallback defaults are natural
  hardening for dictionary-style content.
- **Per-display animation cadence**: Ultralight's `RefreshDisplay(display_id)`
  ties rAF to a monitor's vsync. Design note for a future `mbUpdate(now_ts)`
  parameter rather than a need — display-link-driven hosts already control
  cadence.
- **Inspector**: `CreateLocalInspectorView()` + inspector assets over the
  FileSystem is Ultralight's debugging story; miniblink2 has console drain and
  the AX tree but no devtools. A real devtools frontend against this Chromium
  is a large project — recorded as the known gap, not scheduled.

## Status

| # | Proposal | State |
|---|----------|-------|
| 1 | Bounded update slice | **Shipped** (871a40b): mbSetMaxUpdateTime; delayed hard-quit races quit-on-idle. Glyph runs an 8 ms budget. |
| 2 | Per-view user stylesheet | **Shipped** (871a40b): mbSetUserStylesheet via StyleEngine::InjectSheet, re-applied per commit. ADOPTION CAVEAT: user origin ranks below author styles without !important - Glyph keeps its author-level prelude until the CSS is hardened (or a kAuthor variant is added). |
| 3 | Zero-copy response bodies | **Deferred by cost**: the body is a std::string threaded through MbFindMock/MbFetchUrl/the async deliver path; an owned-buffer type means retyping that plumbing end to end, against a measured saving of one memcpy of already-cached bytes per serve (~ms per page). Revisit if a host serves large media. |
| 4 | mbPurgeMemory / mbLogMemoryUsage | **Shipped** (871a40b): critical pressure broadcast + V8 low-memory GC; coarse V8/malloc log. |
| 5 | JS exception channel + lifecycle doc | **Shipped** (871a40b): mbEvalJSCatch (message + line); binding-lifecycle contract documented in webview.h. |
| 6 | Sessions | **Stage 1 shipped**: MbSession identity + refcount/detach lifetime, default session, mbCreateViewInSession/mbViewGetSession, session-id prefix on the storage partition scope (DOM storage/IDB/OPFS/buckets/locks isolate per profile). Stage 2 shipped: per-session curl cookie jars (keyed shares; fetch paths resolve via the view registry, document.cookie via the session-prefixed scope; unknown contexts alias the default jar). Stage 3 shipped: persistent profiles restore at create and flush at mbSessionFlush/teardown (cookies + prefix-filtered IndexedDB + OPFS under persist_dir); mbSessionClearStorage wipes the profile. localStorage is blink-internal: not persisted per session (documented). THE SESSIONS DESIGN IS COMPLETE. |
| 7 | Font defaults / update timestamp / inspector | **7a shipped**: mbSetFontFamilies. **7b shipped**: mbUpdateAt. **7c Stage A shipped (draft)**: in-process CDP pipe (mbDevToolsAttach/Send/Detach) driving blink's DevToolsAgent standalone - verified by a Runtime.evaluate round trip. Root causes: null blink::String mojo params (validation-dropped), JSON-vs-CBOR command encoding, a null LayerTreeDebugState overlay deref (patch 0024), a null WidgetInputHandlerManager deref on session detach (patch 0025), and needing a real non-associated primary pipe. **7c Stage B open**: the host WebSocket + /json bridge (embedder-side, per the staged plan below) is not in this tree - no MiniblinkDevToolsBridge/GLYPH_DEVTOOLS code exists here yet. |

## Sessions: the agreed design (item 6)

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
- Sessions are CAPABILITY HANDLES, not name strings in a global registry -
  whoever holds the handle controls the profile.
- Ephemeral vs persistent is a creation-time binary, visible at the call site.
- COOKIES ARE IN, unconditionally: account isolation is the reason sessions
  exist; per-session cookie jars in the curl layer are an accepted consequence.
- Storage completeness: everything origin-keyed partitions by session
  (cookies, local/session storage, IndexedDB, OPFS, CacheStorage,
  BroadcastChannel/locks). The HTTP byte cache may stay shared - it is
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

## Inspector: the staged plan (item 7c)

Scoped 2026-07-04. The surprise that makes this tractable: blink's inspector
core (DevToolsAgent / DevToolsSession, renderer/core/inspector) already
compiles into libminiblink2 - the CDP *backend* is in the binary, unexported.
And with a CDP endpoint speaking the standard /json discovery protocol,
ORDINARY CHROME is the frontend (devtools://devtools/bundled/inspector.html
connects to any ws:// CDP target) - bundling/building devtools-frontend is
unnecessary.

- **Stage A - engine CDP pipe.** Bind the main frame's mojo DevToolsAgent
  in-process and expose it flat:
    typedef void (*mbDevToolsMessageCallback)(mbView*, void* userdata,
                                              const char* json, int len);
    MB_EXPORT int  mbDevToolsAttach(mbView*, mbDevToolsMessageCallback, void*);
    MB_EXPORT void mbDevToolsSend(mbView*, const char* json, int len);
    MB_EXPORT void mbDevToolsDetach(mbView*);
  One session per view; messages are CDP JSON both ways. The work is mojo
  plumbing (session channel, IO-vs-main routing) - the protocol itself is
  blink's.

- **Stage B - host WebSocket bridge.** The embedder (not the engine) serves
  ws://127.0.0.1:<port>/ + /json/list, pumping frames to/from the pipe. In
  Glyph: a debug menu item "Inspect dictionary popup" that starts the bridge
  and copies the devtools:// URL. Keeps sockets out of the engine.

- **Stage C - bundled frontend: intentionally skipped.** Chrome is the
  frontend; shipping one inside the SDK adds megabytes and a TypeScript
  build for zero capability.

Status: Stage A shipped (draft) - see item 7 in the table above. Stage B (the
host WebSocket bridge) is the open item; it lives in the embedder, not this
repo's engine sources.
