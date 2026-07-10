# API design log for embedders

Every public `mb*` function traces to a real integration incident from
embedding miniblink2 in an interactive host, cross-checked against comparable
embedded-engine SDKs (offscreen surface, flat C API). Source comments cite
these item numbers as the rationale anchor — **the numbering is stable**.

Rounds 1–6 are shipped. Open items are in the table near the end; features
deliberately *not* built live in [BACKLOG.md](BACKLOG.md).

---

## Round 1 — the interactive tick

1. **Bounded, re-entrancy-safe update** — `mbUpdate` runs ready engine work
   without nesting the host run loop; `EngineScope` guards engine-entering
   exports; `mbInEngineCall`/`mbDefer` handle work scheduled mid-call. (A
   nested pump in a private run-loop mode isn't possible on stock M150, so host
   code can still run inside blocking calls — these are the mitigation.)
2. **Damage + frame status** — `mbViewIsDirty` (snapshot semantics;
   `mbRepaintToBitmap` returning 0 strictly means "buffer untouched") and
   `mbViewGetDirtyRect` (damaged region of the last repaint; empty = skip the
   blit). The rect diff runs blink's `RasterInvalidator` inside the paint cycle
   — the only window where the previous artifact's backing store is still alive
   (patch 0041 + `mb_damage_tracker`). Paint-purity rule: the lifecycle update
   is unconditionally the last step before paint. *Open: an engine-owned
   lockable surface.*
3. **Async loads + lifecycle callbacks** — loads return before the `load`
   event; `mbOnBeginLoading`/`mbOnFailLoading` join finish/DOM-ready.
4. **Per-view resource hooks** — `mbOnRequestMock(view, cb, ud)` on every fetch
   path (subresource, navigation, worker main script); the view context keys
   the session cookie jar. A nested worker's script (no frame) falls back to
   the process-wide hook (documented). Static block/mock/rewrite tables stay
   global — they're config, not routing.
5. **Typed input events** — `mbMouseEvent`/`mbWheelEvent`, struct_size-versioned,
   float deltas, reserved gesture phase.
6. **Header split by audience** — `webview.h` (embedder core) + `automation.h`
   (pumping calls flagged); no umbrella header.

## Round 2 — profiles, budgets, diagnostics

7. **Time-bounded update** — `mbSetMaxUpdateTime` (0 = unbounded);
   `mbPumpMessages` stays run-to-idle for automation.
8. **Engine user stylesheet** — `mbSetUserStylesheet` (user origin; ranks below
   author styles without `!important`).
9. **Zero-copy resource bodies** — *deferred by cost* for the general response
   body (a `std::string` threaded through the whole serve path, against one
   memcpy of already-cached bytes). The self-contained image-source case
   shipped (item 43). Revisit if a host serves large media.
10. **Memory pressure** — `mbPurgeMemory` (critical-pressure broadcast + V8
    low-memory GC) / `mbLogMemoryUsage`.
11. **JS exception channel** — `mbEvalJSCatch` (message + line). Binding
    lifecycle documented: JS state dies with the context on navigation;
    `mbSetInitScript` survives; re-establish computed state at
    window-object-ready (item 32).
12. **Sessions** — browsing profiles as capability handles; see the design note
    below. All three stages shipped (identity + partitioned storage; per-session
    cookie jars; persistence at create/flush). localStorage is blink-internal,
    not persisted per session (documented).
13. **Font defaults / update timestamp / inspector** — `mbSetFontFamilies`;
    `mbUpdateAt` (process-global) plus per-view `mbViewSetFrameTime`; inspector
    Stage A (in-process CDP pipe, item 13c below).

## Round 3 — page → host UI-state channel

14. **Cursor + tooltip** — `mbOnCursorChanged` (the full `MB_CURSOR_*` set) /
    `mbOnTooltipChanged` (empty = hide). Fire only on change.
15. **window.close()** — `mbOnRequestClose` (notification only; host decides).
16. **Input-focus query** — `mbHasInputFocus` (1 when the page would consume a
    keystroke), so hosts with global hotkeys can route keys correctly.
17. **History push** — `mbOnHistoryChanged` (can_go_back/forward), event-driven.
18. **Version handshake** — `mbVersion`/`mbApiVersion`/`mbChromiumVersion`,
    callable before `mbInitialize`.
19. **Host log sink** — `mbOnLogMessage` (level, message); NULL restores stderr.
20. **Per-character font fallback** — `mbSetFontFallbackCallback`, consulted
    before the platform cascade (patch 0029). The named family must actually
    cover the character or the answer is ignored — a wrong host answer falls
    through instead of rendering tofu.
21. Noted, some later reversed: TLS pinning → item 44; streaming downloads →
    shipped (`mbOnDownloadStream`/`mbDownloadURLStream`/`mbCancelDownload`);
    ImageSource → shipped (item 43). Kept out: thread/allocator override
    (blink bring-up isn't pluggably cheap), RenderOnly (dirty-gating covers it).

## Round 4 — the corners rounds 1–3 skipped

22. **Child views** — `mbOnCreateChildView`: the engine creates the child
    (opener wired, same session + agent cluster); the host returns 1 to adopt
    (live opener/`postMessage`, `window.close()` allowed) or 0 to decline.
    Lifetime: destroy the child before its parent.
23. **Per-view JS toggle** — `mbSetEnableJavascript` (blink reads the live
    setting, so host eval is also refused while off — re-enable to script).
24. **Typed keyboard event** — `mbKeyEvent` → `mbSendKeyEvent` (auto-repeat,
    keypad, unmodified_text). `MB_KEY_DOWN` with text types the character.
25. **Pixel contract** — BGRA, **premultiplied** alpha, sRGB (stated header-top
    and on every paint export).
26. **Host-forced repaint** — `mbViewSetDirty` (for a host that lost its buffer
    on hide/show/resize; the damage gate would otherwise skip forever).
27. **Load/history nuances** — `mbLoadHTMLEx(add_to_history)` (0 = replace),
    `mbGoToOffset`.
28. **Force-repaint switch** — `mbSetForceRepaint` (diagnostic escape hatch when
    the dirty flag is suspect).
29. **Session introspection** — `mbSessionGetName`/`IsPersistent`/`GetPersistPath`.
30. Convention wins: one stated threading contract + logical/physical-px
    contract header-top; `MB_VERSION` string macro beside runtime `mbVersion()`.
31. Anti-patterns rejected: license-tier doc gating, `#pragma pack` on ABI
    structs (struct_size is better), modal in-SDK dialogs, gamepad surface.

## Round 5 — from-scratch re-read

32. **Window-object-ready** — `mbOnWindowObjectReady`: earliest per-document JS
    setup point (after built-in shims + init script, before page script). Host
    eval from inside runs INLINE (routed around the nested pump). Fixed a
    subframe double-run of the main frame's shims + init script.
33. **Structured load errors** — `mbOnFailLoadingEx` (error_domain
    curl/file/network/blocked, error_code, description). One slot with plain
    `mbOnFailLoading`. Design rule recorded here and honored by item 42:
    per-frame load events carry `(frame_id, is_main_frame)` from day one.
34. **OS-clipboard bridge** — `mbSetClipboardHandler`: the engine pulls on paste
    and pushes on copy; the in-process jar remains so `mbGetClipboard` still
    works. Callbacks fire on the broker's service thread — marshal yourself.
35. **Host font bytes** — `mbAddFontData` (macOS via `CTFontManager`; resolves
    in CSS, `mbSetFontFamilies`, and the fallback callback). *Open: Windows —
    DirectWrite private collection pending; the export returns 0 there.*
36. **Creation-time view config** — an opaque `mbViewConfig` builder +
    `mbCreateViewWithConfig`, retiring the `mbSetCompositingEnabled` process
    latch and the "call before first load" ordering traps. New options never
    change a signature (better ABI evolution than even struct_size).
37. **Mutable request handle** — opaque `mbRequest*` + `mbSetRequestHook`:
    `mbRequestSetUrl` (transparent rewrite), `mbRequestSetHeader` (replaces, not
    duplicates), `mbRequestBlock`. Dispatched at both fetch entries after the
    static tables; a blocked top-level load reports domain `"blocked"`.
38. Documentation batch: worked host-loop example, per-host-type wiring matrix,
    numeric version macros, `mbJsNativeFn` return-string lifetime,
    `MB_KEY_DOWN` footgun note, recommended `mbSetMaxUpdateTime`, PNG
    straight-alpha statement (the encoder unpremultiplies).
39. Noted, some reversed: per-display refresh → shipped as `mbViewSetFrameTime`
    (per-view fits the architecture better than display groups); in-engine
    DevTools server → reversed (item 41); console column/category → reversed
    (item 46). Kept out: app convenience layer (served by `samples/`),
    thread-ambient JS contexts (keep contexts explicit).

## Round 6 — the final residue pass

40. **Native-event translators** — header-only `src/compat/webview_{mac,win}.h`
    (NSEvent / Win32 `WM_*` → the mb input structs; the keycode tables the SDK
    should own). Internal platform layer, deliberately NOT staged into the SDK;
    the flat `mb*` ABI stays ObjC/Win32-free.
41. **In-engine DevTools endpoint** — `mbDevToolsStartServer(port)`/`Stop`: an
    opt-in loopback CDP server (`/json/version`+`/json/list`, one WebSocket
    target per view) bridging to the same per-view session `mbDevToolsAttach`
    uses. Reverses the round-5 "sockets stay in the embedder" call by maintainer
    directive; the embedder bridge remains fully supported. Cross-platform over
    the internal `src/compat/mb_socket.h` (BSD sockets / Winsock2). *Still
    single-target-per-view.*
42. **Per-frame load events + stable IDs** — `mbOnFrameLoadEvent` (phase enum
    BEGIN/DOM_READY/FINISH/FAIL, `frame_id`, `is_main_frame`, url) for every
    local frame; `mbGetFrameIds`; `mbEvalJSInFrameById` — the deterministic
    sibling of the racy index-based iframe eval. IDs are LocalFrameToken-derived,
    stable for the frame's life, never reused within a view.
43. **Zero-copy image sources** — `mbRegisterImageSourceBuffer` (borrow pixels,
    lazy PNG on first fetch, `release_cb` on replace/unregister/shutdown)
    alongside the eager-copy `mbRegisterImageSource`. Served in-process ahead of
    mocks/network; re-registering swaps pixels and fires the
    `mbimagesourceupdate` event so pages re-fetch.
44. **TLS pinning** — `mbRequestPinPublicKey` → `CURLOPT_PINNEDPUBLICKEY` on the
    item-37 request handle; applies to both fetch entries.
45. **Memory budgets** — `mbSetMemoryCacheSize` (blink resource MemoryCache; 0
    restores default; applies immediately) and `mbSetJsHeapLimit` (V8 old-space;
    pre-init only). Two levers, not twelve.
46. **Structured console** — `mbOnConsoleMessage2` delivering a struct_size
    `mbConsoleMessageInfo` (level, message, url, line, COLUMN, stack, source
    category). Same one slot as the two legacy console callbacks; the struct is
    the extension point from now on (patch 0043).
47. **Pixel utilities** — `mbConvertToStraightAlpha` / `mbConvertToPremultipliedAlpha`
    / `mbSwapRedBlueChannels` (flat buffer math, the conversions the BGRA
    contract implies).
48. **Custom schemes** — `mbRegisterCustomScheme` (pre-init): registers the
    scheme as standard + secure + fetch-enabled and routes its requests through
    the interception stack instead of the network. `MbWebView::LoadURL` gained a
    custom-scheme navigation branch; unmocked requests fail cleanly.
49. Scope: gamepad still out (game-engine market, not ours); console
    JS-argument access not adopted (a handle table + lifetime rules for a
    debugging convenience — the stack + stringified message cover monitoring).

---

## Open items

| Item | What's left |
|---|---|
| 2 | Engine-owned lockable surface (dirty *rects* shipped: `mbViewGetDirtyRect`) |
| 9 | General zero-copy response bodies (the image-source case shipped, item 43) |
| 13c | Child worker/iframe DevTools targets (single-target-per-view today) |
| 35 | `mbAddFontData` on Windows (DirectWrite private collection) |

## What NOT to copy

- A trimmed WebKit fork — single-dylib real Chromium (M150 Blink + V8) wins on
  web compat by a mile.
- A host FileSystem interface — response mocking is the stronger interception
  primitive for content-serving hosts; custom schemes (item 48) widen what it
  serves without replacing it.
- A C++ RefPtr-style surface — the flat C ABI is right for a dlopen-able engine;
  keep ownership rules explicit in comments.
- Pumping automation helpers bleeding into the interactive path — the header
  split (item 6) keeps them first-class but fenced.

## Sessions design (item 12)

Browsing profiles as capability handles, not name strings in a global registry:

```c
mbSession* mbCreateSession(const char* name, const char* persist_path); // NULL = ephemeral
mbView*    mbCreateViewInSession(int w, int h, mbSession*);
mbSession* mbViewGetSession(mbView*);
mbSession* mbDefaultSession(void);                 // the implicit ephemeral one
void       mbSessionClearStorage(mbSession*);      // logout-everything wipe
void       mbSessionFlush(mbSession*);             // durability barrier
void       mbDestroySession(mbSession*);           // safe with views still open
```

- Handle-scoped; ephemeral-vs-persistent is a creation-time binary, fixed for
  the session's life.
- Everything origin-keyed partitions per session (DOM/session storage,
  IndexedDB, OPFS, CacheStorage, cookies, locks/BroadcastChannel). The HTTP byte
  cache may stay shared — it's content-addressed, not identity-bearing.
- The default session is ephemeral and implicit: plain `mbCreateView` keeps
  working and touches no disk unless a host opts in.

## Inspector design (item 13c)

Blink's CDP backend already compiles into the engine (unexported), and with a
`/json` discovery endpoint **ordinary Chrome is the frontend** — no bundled
frontend ships.

- **Stage A** — engine CDP pipe (`mbDevToolsAttach`/`Send`/`Detach`), one
  session per view, CDP JSON both ways. Shipped, proven against real Chrome.
- **Stage B** — host WebSocket bridge (sockets stay out of the core). Shipped in
  the embedder; also available in-engine via `mbDevToolsStartServer` (item 41).
- `mbOnDevToolsPaused` lets the host stop its frame tick at a breakpoint instead
  of treating it as a hang.
- Open: child worker/iframe targets (`ChildTargetCreated` is a single-target-v1
  no-op).
