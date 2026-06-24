# miniblink-modern — PROGRESS (loop state, read this first every iteration)

> Autonomous `/loop` is upgrading miniblink from M47 Blink to **modern M150
> Blink** via a hand-written in-process host (`miniblink_host`) exposed through a
> pure-C ABI (`mb_capi`), with a `wke` compatibility layer on top. This file is
> the single source of truth for loop continuity — read it, do the next bounded +
> empirically-verified action, update it, commit, repeat.
>
> **History through 2026-06-24 is archived in `docs/progress-archive-2026-06.md`**
> (the day-0..1 bring-up milestones, the full per-tick log, and resolved
> debugging notes). This file keeps only current state + recent log + roadmap.

## Git / commit convention (MUST follow)
- Author `Xin Yang <yangxin0@outlook.com>`, ~72-col body explaining WHY, **NO
  AI / Co-Authored-By trailer**. Commit with:
  `git -c user.name="Xin Yang" -c user.email="yangxin0@outlook.com" commit --no-verify`
- Commit per-milestone, only at a clean, tested state.
- `patches/*.patch` are tracked here but apply to the DONOR chromium tree.

## Fixed facts
- **Goal:** modern Blink (M150), single-process, libcurl networking, driven by
  the `mb` C API (`mb_capi`) + a `wke` compatibility layer. NOT CEF.
- **Donor tree (GN/Ninja, already builds):**
  `/Users/yangxin/dennis/chrome/chromium-150.0.7871.24` — `out/Release` gen'd,
  `is_component_build=true`, macOS SDK, gn at `buildtools/mac/gn`.
- **This project:** `/Users/yangxin/dennis/chrome/miniblink-modern/`, include-root `src/`.
  - `src/miniblink_host/` = the content-layer host (GN `component` + the `mb_capi` C ABI).
  - `src/wke/` = the wke compatibility layer (compiled into the host lib + a `wke_smoke` exe).
  - `patches/` = donor-tree patches. `docs/` = specs + the history archive.
- **wke parity reference (original API):** `/Users/yangxin/dennis/chrome/miniblink49` (`wke/wke.h`).

## Architecture
3 layers; a pure-C ABI seam dissolves the GN↔CMake mismatch:
1. Blink + substrate — built AS-IS by GN in the donor tree.
2. `miniblink_host` — implements `blink::Platform`, the WebView/LocalFrame/Widget
   handshake, an in-process mojo service host (cookies, blobs, BlobURLStore bound
   on a service thread so `[Sync]` calls don't deadlock), a libcurl loader, and
   viz→SkBitmap paint; exposes `extern "C"` `mb_capi`.
3. `wke`/`mb` C API consumers: `mb_shot` (CLI), `mb_smoke`, `wke_smoke`,
   `wke_demo` (runnable automation example).

## Build & test (sources are STAGED into the donor tree, then ninja)
```
TREE=/Users/yangxin/dennis/chrome/chromium-150.0.7871.24
rm -rf $TREE/third_party/blink/renderer/miniblink_host $TREE/third_party/blink/renderer/wke
cp -R src/miniblink_host $TREE/third_party/blink/renderer/miniblink_host
cp -R src/wke            $TREE/third_party/blink/renderer/wke
ninja -C $TREE/out/Release mb_smoke mb_shot wke_smoke
```
- `build.sh <TREE>` does the full staging + patch-apply + build.
- ALWAYS run binaries bounded (background + a watchdog that SIGKILLs) and then
  `pgrep -x mb_smoke|mb_shot|wke_smoke` survivor-check — NEVER leave leaked processes.
- Default `mb_smoke` is network-free + deterministic. Network cases are opt-in via
  `MB_NET_TESTS=1` (httpbin). libcurl reaches public hosts but TIMES OUT on
  localhost — verify network features against public hosts (dangerouslyDisableSandbox),
  never a local server.

## CURRENT STATE (2026-06-24)
The engine is **feature-complete for common headless web**; work is now growing
the deliverable surface (C API, CLI, wke layer).
- **Engine:** modern M150 Blink renders HTML→pixels in-process; V8/JS, modern CSS,
  canvas, SVG, fetch/XHR (method/body/headers/status/cookies/redirects/charset),
  blob: URLs (`fetch` + `<img>`), Intersection/Resize/Mutation observers, WAAPI,
  forms + submit-navigation, mouse/keyboard input, host-side history. CJK/i18n +
  system web fonts render.
- **`mb_capi` C API — 110 functions:** lifecycle, load, JS eval, scraping
  (text/attr/computed-style/count by selector), input (mouse/key/text/scroll +
  click/fill/select/focus/hover/scroll-into-view by selector), screenshots
  (PNG/JPEG/PDF, file + in-memory `mbEncodePng`), cookies (+ jar save/load),
  network config (proxy / cert-bypass / follow-redirects / status / response-headers),
  config (UA/headers/locale/tz/dark/DPR/transparent/images), history.
- **`mb_shot` CLI (the deliverable tool):** a full scraper/automator — interact
  (`--fill`/`--click`/`--drag`/`--dispatch`/`--wait-selector`/`--wait-visible`/
  `--wait-hidden`/`--wait-idle`/`--css`/`--auto-scroll`/`--wait-ms`) → extract (`--text`/`--html`/`--eval`/`--value`/`--checked`/
  `--visible`/`--rect`/`--style`/`--text-all`/`--attr-all`/`--requests`) → capture (`--full`/`--clip`/
  `--selector`); plus `--proxy`/`--insecure`/`--no-follow`/`--headers`/
  `--block`/`--user-agent`/`--load-cookies`/`--save-cookies`.
- **wke compatibility layer (`src/wke/`):** a faithful subset over `mb_capi` covering
  the full headless-automation surface — lifecycle, load, loading-state polling,
  paint (`wkePaint`), PDF/PNG export (`wkeSavePdf`/`wkeSavePng`/
  `wkeSavePngRect`/`wkeEncodePng` in-memory), viewport scroll (`wkeScrollTo`),
  mouse (`wkeFireMouseEvent`), keyboard (`wkeFireKey*`),
  scripting (`wkeRunJS` + `jsToInt/Double/Boolean/TempString` + `jsTypeOf` +
  the full jsValue object model — `jsIs*` type predicates, reads
  `jsGetLength`/`jsGetAt`/`jsGet`/
  `jsGetGlobal` + `jsGetKeys`, constructors `jsInt`/`jsString`/…, builders
  `jsEmptyObject`/`jsEmptyArray` + setters `jsSet`/`jsSetAt`/`jsSetGlobal`, and
  `jsCall`/`jsCallGlobal`, plus `wkeSetInitScript` evaluateOnNewDocument),
  DOM query (`wkeCountSelector`/`wkeGetTextForSelector`/`wkeGetAttribute`/
  `wkeGetElementRect`/`wkeGetComputedStyle`) + actions (`wkeClickSelector`/
  `wkeFillSelector`/`wkeSelectOption`/`wkeScrollIntoView`/`wkeHoverSelector`/
  `wkeFocusSelector`/`wkeBlurSelector`/`wkeDoubleClickSelector`/
  `wkeRightClickSelector`) + waits (`wkeWaitForSelector`/`wkeWaitForFunction`),
  POST (`wkePostURL`), cookies (`wkeGetCookie`/`wkeSetCookie`/
  `wkePerformCookieCommand` + jar persistence via `wkeSetCookieJarPath`),
  proxy (`wkeSetProxy`, HTTP/SOCKS + auth), request headers
  (`wkeSetExtraHeaders`), i18n emulation (`wkeSetLocale`/`wkeSetTimezone`),
  HTTP introspection (`wkeGetHttpStatusCode`/`wkeGetResponseHeaders`),
  redirect control (`wkeSetFollowRedirects`), TLS-error bypass
  (`wkeSetIgnoreCertErrors`), image-loading toggle (`wkeSetLoadImages`),
  navigation history,
  page text (`wkeGetText`), rendering accessors
  (`wkeSetTransparent`/`wkeIsTransparent`,
  `wkeSetZoomFactor`/`wkeGetZoomFactor`, `wkeSetEditable`, `wkeSetDarkMode`,
  `wkeSetDeviceScaleFactor`, `wkeGetContentWidth/Height`),
  view-state (`wkeSetName`/`wkeGetName`,
  `wkeSetUserKeyValue`/`wkeGetUserKeyValue`), and the
  async callback model (`wkeOnLoadingFinish`/`wkeOnTitleChanged`/`wkeOnConsole`/
  `wkeOnDocumentReady` + `wkeString`), page source (`wkeGetSource`).
- **Tests:** `mb_smoke` **162/162** (default, network-free), `wke_smoke` **100/100**,
  deterministic, no survivors. `MB_NET_TESTS=1` adds httpbin/example.com/badssl
  cases (wke_smoke up to 65; use a generous watchdog ≥180s — cumulative loads +
  the 15s failing-proxy connect; cases SKIP when a host is unreachable).
- **Donor patches (`patches/`):** 0001 offscreen-widget-compat, 0002 suppress-js-dialogs,
  0003 enable-blob-Register, 0004 blob-url-loader-bypass.

## Known gaps / deferred (detail in the archive)
- **Heavy / multi-session:** Web Workers (inert — needs a real worker thread + isolate),
  WebGL (needs the GPU/command-buffer pipeline), page-driven `history.back()` (needs a
  ~171-method LocalFrameHost shim), a Cocoa windowed port app.
- **Reverted as unsafe:** host-controlled JS dialog responses (`mbSetJsDialogPolicy`) —
  showing real modal dialogs intermittently FATALs in `thread_collision_warner`
  (the modal-dialog / ScopedPagePauser threading needs understanding first). Related:
  `mbEvalJS` aborts if a script opens a modal dialog, so the host runs dialog-triggering
  scripts via `mbRunJS`.

## Recent log (newest first; full history in the archive)
- maint: full health-check — all 6 targets build + both demos run (2026-06-25). After ~15 functions landed across the widget/host/loader since the last all-targets build, re-verified the whole set: miniblink_host + mb_smoke + mb_shot + mb_demo + wke_smoke + wke_demo all link clean (no bit-rot). Ran both reference demos end-to-end: mb_demo all 9 steps OK, wke_demo all 11 steps OK, exit 0 each. Suites green (wke_smoke 100/100, mb_smoke 162/162), no survivors. Verification-only; no code change.
- capi+wke: mbGetCookie / wkeGetCookieValue — read one cookie by name (2026-06-25). Convenience over mbGetCookies' whole-jar string for the common "read the session/auth cookie to check login" check — host-side parse of the "n=v; n2=v2" jar; -1 (capi) / "" (wke) when the name is absent. wke variant uses the current document URL (like wkeGetCookie). VERIFIED offline (in-memory jar): set sid/theme -> mbGetCookie("sid")=="abc123", missing->-1; wke set+navigate to the origin -> wkeGetCookieValue("auth")=="tok9". wke_smoke 100/100 (milestone), mb_smoke 162/162, no survivors. ABI now 110 fns.
- capi: mbGetViewSize — viewport size read-back (2026-06-25). The C ABI could SET the view size (mbCreateView/mbResize) but not read it (wke already had wkeGetWidth/Height; mb_capi had no peer — a gap I hit building SaveElementPng). Reads window.innerWidth/Height (logical px, DPR-independent), the read-back peer of mbResize, distinct from mbGetContentSize (full scrollable doc). VERIFIED: mbResize(640,480) -> mbGetViewSize reads 640x480. No wke change (it has the getters). wke_smoke 99/99, mb_smoke 161/161, no survivors. ABI now 109 fns.
- capi+wke: mbClearStorage / wkeClearStorage — reset Web Storage (2026-06-25). Completes the storage surface (get/set local+session, now clear): empties BOTH localStorage and sessionStorage for the document origin — test isolation between scrapes, or a logout. Best-effort per store (opaque-origin throw ignored); the cookie-jar peer mbClearCookies + this = a full session reset. Safe eval-based (no native-API risk). VERIFIED in both suites: after setting local 'auth'/'pref' + session 'sk', mbClearStorage -> localStorage.length==0 && sessionStorage.length==0. wke_smoke 99/99, mb_smoke 160/160, no survivors. ABI now 108 fns.
- maint: trim the Recent log into the archive (2026-06-25). PROGRESS.md had grown to 93KB / 164 log entries — loaded into context every iteration. Moved the 122 oldest recent-log entries (the bulk of the 2026-06-24 per-tick log) into docs/progress-archive-2026-06.md under a dated "archived 2026-06-25" header, keeping the ~20 newest + the "earlier:" rollup here. Conservation checked (21 kept + 122 moved = 143; spot-checked a moved entry is in the archive and gone from here). PROGRESS.md now 24KB. Docs-only — no code, suites unchanged (mb_smoke 159/159, wke_smoke 98/98).
- capi+wke: mbSendTouchSwipe / wkeFireTouchSwipe — touch swipe gesture (2026-06-25). Completes touch input (tap + swipe): touchstart + 6 interpolated touchmoves + touchend, same synthetic-TouchEvent approach as the tap (the moves stay on the start element via touch capture). Drives touch scroll / swipe (carousels, pull-to-refresh, mobile drawers) — distinct from mouse drag (touch handlers != mouse handlers). VERIFIED in both suites: a touchmove handler sees the moves and the final touches[0].clientX == the swipe end x (mb 200 / wke 220), touchend fires. wke_smoke 98/98, mb_smoke 159/159, no survivors. ABI now 107 fns.
- capi+wke: mbSendTouchTap / wkeFireTouchTap — touch input (2026-06-25). A distinct capability (mobile/touch-only handlers, which mouse events don't reach; mbDispatchEvent can't shape a TouchEvent with touch-point coords). First probed that the engine supports touch (TouchEvent type, ontouchstart in window, JS-dispatched touch fires). Tried a native WebTouchEvent via the widget's HandleInputEvent -> SIGABRT: WebFrameWidgetImpl DCHECKs !IsTouchEventType (Blink routes real touch through an async touch queue the offscreen widget doesn't wire). Reverted that and reimplemented in MbWebView via a correctly-shaped synthetic TouchEvent (elementFromPoint -> new Touch -> dispatch touchstart[touches:[t]]+touchend[changedTouches:[t]]). VERIFIED in both suites: a touch-only handler fires and reads touches[0].clientX == the tap x (mb 50, wke 60), touchend fires. wke_smoke 97/97, mb_smoke 158/158, no survivors. ABI now 106 fns. (Synthetic, not native input — documented; native touch needs the full touch-event pipeline.)
- docs: re-sync the README mb_shot CLI section (all 45 flags) (2026-06-25). The deliverable tool's docs had badly drifted — the usage block + prose listed ~25 flags but the binary accepts 45 (~20 undocumented: value/checked/visible/rect/style/text-all/attr-all/requests, drag/dispatch, wait-visible/hidden/idle, css/auto-scroll/scroll-to-selector, user-agent/ua/block). Rewrote the usage block grouped by phase (request-config / interact / synchronize / prepare-view / extract / capture) and added concise prose for the new interaction, wait, view-prep, and extraction flags. Cross-checked all 45 flags now appear in README (0 missing; --ua noted as the --user-agent alias). Docs-only — no code, suites unchanged (wke 96/96, mb 157/157).
- mb_shot: --scroll-to-selector CSS — position the viewport at an element (2026-06-25). Exposes mbScrollIntoView on the CLI: bring a specific element into view before a viewport-mode capture (show it in context — distinct from --selector, which clips just its box; and from --scroll-to Y, which is absolute). VERIFIED end-to-end offline with a clean A/B (600x400 viewport): a #x below a 2000px spacer reads scrollY==0 WITHOUT the flag, and WITH --scroll-to-selector '#x' scrollY>0 AND #x's rect.top is within [0,400) (in-viewport) -> "true,true". No survivors. mb_shot.cc + usage only; both suites unchanged (wke 96/96, mb 157/157).
- mb_shot: --dispatch CSS EVT — synthetic DOM event on the CLI (2026-06-25). Exposes mbDispatchEvent in the interact phase (after --drag), completing CLI interaction parity (--fill/--click/--drag/--dispatch): fire a hover/custom/framework event that click/fill don't, before capture. VERIFIED end-to-end offline with a clean A/B: a #m whose 'reveal' listener rewrites its text reads "hidden" WITHOUT the flag and "REVEALED" WITH --dispatch '#m' reveal; no survivors. mb_shot.cc + usage only; both suites unchanged (wke 96/96, mb 157/157).
- docs: re-sync the README wke section + correct stale test counts (2026-06-25). The wke list had drifted again — 7 of 123 wke exports undocumented (wkeGetHtmlForSelector/Set, GetAllValueForSelector, DispatchEvent, DragSelector, SaveElementPng, WaitForNetworkIdle). Added them under the right groups (DOM-automation query/act/wait, Capture) and cross-checked all 123 now appear (0 missing). Also fixed three stale suite-count claims: wke_smoke "88"/"63" -> 96, and mb_smoke "~107-case (133-check)" -> "157-check". Docs-only — no code, suites unchanged (wke 96/96, mb 157/157).
- mb_shot: --drag FROM TO — drag-by-selector on the CLI (2026-06-25). Exposes mbDragSelector in the interact phase (after --click): mouse-drag one element's center onto another's before extract/capture (slider thumb, sortable item, map pan). VERIFIED end-to-end offline: --drag '#h' '#t' on a page whose mouseup records clientX -> --eval window.__dropx prints 220 (the target center), no warning, no survivors. mb_shot.cc + usage only; both suites unchanged (wke 96/96, mb 157/157).
- capi+wke: mbDragSelector / wkeDragSelector — drag one element onto another (2026-06-25). The high-level convenience over last tick's drag primitives (Puppeteer dragAndDrop): resolve both selectors' centers, then press on `from`, glide through 6 interpolated moves (carrying the held button), release on `to` — removes the rect-math + interpolation boilerplate. Mouse-based (sliders/sortables/map-pan), not HTML5 native DnD; both elements must be in view. VERIFIED in both suites: a #handle that follows the cursor dropped exactly at #target's center x (dropx==220), moves observed, no-match -> 0/false. Caught a -Wshadow build error (a top-of-main `nomatch` collided with later block-scoped ones) -> wrapped the test in its own block. wke_smoke 96/96, mb_smoke 157/157, no survivors. ABI now 105 fns.
- capi+wke: mbSendMouseDown / mbSendMouseUp — drag support (2026-06-25). Only combined mbSendMouseClick existed, so DRAG (sliders, canvas drawing, drag-reorder) was impossible. Added separate press/release in the widget with a held-button state: while pressed, SendMouseMove carries kLeftButtonDown so drag moves report e.buttons==1 (backward-compatible — a hover move when not pressed is unchanged). Down->move(s)->up drags; down+up at one point clicks. Also upgraded wke: wkeFireMouseEvent LBUTTONDOWN/UP now map to real down/up (was: DOWN no-op, UP=full click) — preserves the click (down+up) AND enables drag through the classic wke API. VERIFIED: mb_smoke drag on a pad -> dx==150, e.buttons==1 during the move, and a down+up still fires onclick (dx=150 btn=1 click=1); wke drag delta+buttons match; the existing wke click test still passes. wke_smoke 95/95, mb_smoke 156/156, no survivors. ABI now 104 fns (2 new).
- maint: re-certified build.sh end-to-end (the standalone-build claim) (2026-06-24). Ran the documented one-command build `./build.sh <tree>` from current sources after last tick's new mb_demo target + ~50 functions since the prior cert: stages host+wke, patches idempotent (all 4 already-applied), gn gen clean, builds all SIX targets (miniblink_host, mb_smoke, mb_shot, mb_demo, wke_smoke, wke_demo — mb_demo links via the canonical path, not just direct ninja), vendors both resource paks, runs mb_smoke -> 155/155, exit 0, no survivors. No WARN/NOTE (root gn_all dep still wired — no manual step). Confirms the user's headline requirement ("it's a standalone project") holds with the new target. Verification-only; no code change.
- example: mb_demo — a runnable pure-C mb_capi end-to-end sample (2026-06-24). The primary deliverable seam (the C ABI) had no runnable example — only the wke layer did (wke_demo). Added src/miniblink_host/tools/mb_demo.cc + an executable("mb_demo") GN target (and to build.sh's ninja line); it drives the C ABI: fill #name -> read live value -> dispatch a custom 'refresh' event -> wait for network idle (a deferred <img>) -> scrape text + outerHTML -> check the request log -> element screenshot -> eval, asserting each step (so it doubles as an integration check + C-ABI doc). VERIFIED: all 9 steps OK, exit 0, no survivors; the other targets + suites unregressed (wke_smoke 94/94, mb_smoke 155/155). Example-only — no engine/API change (ABI stays 102). NOTE: a new GN target needs the staged BUILD.gn + gn gen (ninja auto-reran gn here; build.sh does both).
- mb_shot: --wait-idle — network-idle wait on the CLI (2026-06-24). Threads last tick's mbWaitForNetworkIdle into the deliverable (bare flag; clears the request log pre-nav, then waits in the interact phase, idle 500ms / timeout --wait-ms or 10s). The canonical "let the SPA's fetches settle, then screenshot". VERIFIED end-to-end offline: a page that injects an <img> + #late div after 200ms -> with --wait-idle the request log holds the deferred image AND --eval sees #late="LATE" (the deferred content settled). No survivors. mb_shot.cc + usage only; both suites unchanged (wke 94/94, mb 155/155).
- capi+wke: mbWaitForNetworkIdle / wkeWaitForNetworkIdle — Puppeteer networkidle (2026-06-24). A substantive wait built on the request-log infra (observability -> a new synchronization primitive): pump until no NEW subresource request has been recorded for idle_ms, so an SPA's deferred fetches/lazy images settle before scraping/capturing. Reads MbRequestCount() (new loader accessor); each new request resets the idle window; returns 1 idle / 0 at timeout. VERIFIED in both suites: a page that injects an <img> after 150ms -> the wait returns idle ONLY after that request lands (log holds it: fetched=1), and a quiet page is idle without false-timeout (quiet=1). Caught that fetch('file://') is blocked before the loader (fetched=0 on first try) -> switched the deferred request to an injected <img> (image loads do route through MbURLLoader). wke_smoke 94/94, mb_smoke 155/155, no survivors. ABI now 102 fns.
- capi+wke: mbSaveElementPng / wkeSaveElementPng — screenshot one element by selector (2026-06-24). Puppeteer's elementHandle.screenshot, completing the capture family (full page / clip rect / element). Scrolls the first match into view and clips its viewport box (no destructive resize — side-effect-light vs the mb_shot --selector full-page-resize dance); oversized elements capture to the visible extent. Returns 1/0. VERIFIED in both suites with a STRONG check: a below-the-fold 120x40 (wke 90x35) div -> the saved PNG's IHDR width/height equal the element box exactly (dsf 1); no-match -> 0. wke_smoke 93/93, mb_smoke 154/154, no survivors. ABI now 101 fns.
- maint: 100-function milestone health-check — all targets build + demo runs (2026-06-24). After a long run of feature ticks (building only mb_smoke/wke_smoke/mb_shot), verified the WHOLE target set still builds against the ~50 newer signatures: miniblink_host + mb_smoke + mb_shot + wke_smoke + **wke_demo** all link clean (no bit-rot). Ran wke_demo end-to-end: all 11 steps green — fill/select/click/wait, page→host bridge (window.mbBridge), scrape text, computed color rgb(0,128,0), native binding hostDouble(21)==42, option count, runJS, screenshot (writes then self-cleans via std::remove — the "missing PNG" was demo cleanup, not a wkeSavePng bug). No survivors. Verification-only; no code change. (Default suites remain mb_smoke 153/153, wke_smoke 92/92.)
- capi+wke: mbDispatchEvent / wkeDispatchEvent — fire arbitrary synthetic DOM events (2026-06-24). The action surface only fired fixed events (click; input+change via fill/select); this dispatches a bubbling, cancelable Event of any type on the first match — trigger handlers click/fill don't (mouseover/mouseenter hover menus, focus/blur, submit, custom framework events). Synchronous DOM dispatch (no compositor). Returns 1/0 (matched). 🎉 ABI crosses 100 functions. VERIFIED in both suites: a #d with mouseover + custom 'ping' listeners — dispatch each -> the handler ran (counter==1); no-match -> 0. wke_smoke 92/92, mb_smoke 153/153, no survivors. ABI now 100 fns.
- capi+wke: mbGetAllValueForSelector / wkeGetAllValueForSelector — serialize a form's live values (2026-06-24). Completes the all-matches family (text/attr/value): the live .value of EVERY match as a JSON array — capture a whole form's current state in one call, distinct from GetAllAttribute(...,"value") which gives the static initial attribute. Absent value -> null; "[]" none; -1 invalid selector. VERIFIED in both suites: three inputs (a,b,c) with the 2nd filled to "B2" -> ["a","B2","c"]; no-match -> []. (Considered mbSetNetworkTimeout this tick but deferred it — the curl timeout path is http-only and untestable offline, and the net suite is down with httpbin out, so it couldn't be cleanly verified.) wke_smoke 91/91, mb_smoke 152/152, no survivors. ABI now 99 fns.
- test: end-to-end integration case on a real https page (MB_NET_TESTS, 2026-06-24). Added mb_smoke case 44: one real-TLS load of https://example.com that cross-checks the recent scraping readers AGREE on the same <h1> — mbGetTextForSelector ("Example Domain"), mbGetHtmlForSelector (<h1>…Example Domain…), mbGetElementRect (w>0/h>0), mbGetComputedStyle(display=="block"). Gated + skips when status!=200. Default suite unaffected (mb_smoke 151/151, network-free). NOTE: the full MB_NET_TESTS suite currently can't run to completion — httpbin.org is DOWN (all its cases SKIP "host unhealthy/unreachable") and the stacked connect-timeouts exceed the 280s watchdog (exit 137) before reaching case 44. So case 44's LOGIC was verified live via the mb_shot CLI instead (one fast load, no httpbin): example.com -> textContent "Example Domain", rect 240,120,720,28, display block, PNG OK. The test compiles, mirrors existing verified net-case buffer handling, and skips gracefully — committed on that basis. wke_smoke 90/90, no survivors.
- capi+wke: mbSetHtmlForSelector / wkeSetHtmlForSelector — set an element's innerHTML (2026-06-24). The DOM-write peer of GetHtmlForSelector (and a sibling of SetAttribute): replace the first match's innerHTML to template or redact a fragment before a capture. A normal page-context property assignment via eval — not a C++ v8 [[Set]] on the global — so safe in this build. Returns 1/0 (matched). VERIFIED in both suites: #x innerHTML <b>old</b> -> set to <i>new</i>, then textContent=="new" AND GetHtmlForSelector shows <i>new</i>; no-match -> 0. wke_smoke 90/90, mb_smoke 151/151, no survivors. ABI now 98 fns.
- mb_shot: --style SELECTOR PROP — print a computed style value on the CLI (2026-06-24). Wraps mbGetComputedStyle: prints the resolved value of CSS property PROP on the first match (color -> rgb(...), display:none -> none) — CSS/visual assertions from the command line, completing the element-inspection family (text/html/value/checked/visible/rect/style). VERIFIED end-to-end offline: --style '#t' color -> "rgb(9, 8, 7)", --style '#t' display -> "none". No survivors. mb_shot.cc + usage only; both suites unchanged (wke 89/89, mb 150/150).
- earlier: mbGetComputedStyle, mbCountSelector, mbGetTextForSelector/mbGetAttribute, mbScrollIntoView (below-fold click auto-scroll), fetch(blob:) + <img src=blob:>, mb_shot --eval/--fill, text-render + form-nav smoke guards. (older still: full bring-up + render + the automation/network surface — see archive.)

## REMAINING ROADMAP
- **wke layer (active):** the async callback model is started (wkeString +
  wkeOnLoadingFinish/wkeOnTitleChanged). Next: more callbacks (wkeOnConsoleMessage/
  wkeOnURLChanged/wkeOnDocumentReady), more accessors (wkeGetCookie/wkeGetSource),
  and the full V8-backed `jsValue` object model (`jsObject`/`jsArray`/`jsCall`,
  `jsTypeOf`). Each an incremental, testable slice.
- **Heavy items (own focused efforts):** page-driven history.back/forward
  (LocalFrameHost shim); Web Workers; WebGL (GPU pipeline); a Cocoa windowed port
  app under `port/mac`.
- **Mode:** each tick = one bounded, empirically-verified improvement; build green,
  smoke green, no survivors; commit per-milestone with the author/no-trailer convention.
