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
3. `wke`/`mb` C API consumers: `mb_shot` (CLI), `mb_smoke`, `wke_smoke`.

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
- **`mb_capi` C API — 69 functions:** lifecycle, load, JS eval, scraping
  (text/attr/computed-style/count by selector), input (mouse/key/text/scroll +
  click/fill/select/focus/hover/scroll-into-view by selector), screenshots
  (PNG/JPEG/PDF, file + in-memory `mbEncodePng`), cookies (+ jar save/load),
  network config (proxy / cert-bypass / follow-redirects / status / response-headers),
  config (UA/headers/locale/tz/dark/DPR/transparent/images), history.
- **`mb_shot` CLI (the deliverable tool):** a full scraper/automator — interact
  (`--fill`/`--click`/`--wait-*`) → extract (`--text`/`--html`/`--eval`) → capture
  (`--full`/`--clip`/`--selector`); plus `--proxy`/`--insecure`/`--no-follow`/
  `--headers`/`--load-cookies`/`--save-cookies`.
- **wke compatibility layer (`src/wke/`):** a faithful subset over `mb_capi` covering
  the full headless-automation surface — lifecycle, load, loading-state polling,
  paint (`wkePaint`), mouse (`wkeFireMouseEvent`), keyboard (`wkeFireKey*`),
  scripting (`wkeRunJS` + `jsToInt/Double/Boolean/TempString` + `jsTypeOf`),
  navigation history,
  rendering accessors (`wkeSetTransparent`, `wkeGetContentWidth/Height`), and the
  async callback model (`wkeOnLoadingFinish`/`wkeOnTitleChanged`/`wkeOnConsole`/
  `wkeOnDocumentReady` + `wkeString`), page source (`wkeGetSource`).
- **Tests:** `mb_smoke` **132/132** (default, network-free), `wke_smoke` **21/21**,
  deterministic, no survivors. `MB_NET_TESTS=1` adds httpbin cases (143 total).
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
- ⚠️ ATTEMPTED + REVERTED: jsValue object model (jsGetLength/jsGetAt) (2026-06-24). Approach: a JS-side "slot store" — extend the host EvalWithType(slot) to ALSO write the live v8 result into window.__mbslots[slot] (so jsGetAt just evals "__mbslots[h][i]" into a new slot; no host-side v8::Global lifetime to manage). It CRASHED (exit 133 / SIGTRAP in wkeRunJS -> the v8 store) on the first wkeRunJS — the host manipulating the global object (Global()->Get/Set, Object::New) from inside the ExecuteScriptAndReturnValue task traps. Reverted all 7 files; baseline restored (wke_smoke 21/21, mb_smoke 132/132, no survivors). LESSON for the future effort: don't write window.__mbslots from C++ inside the eval task; safer to (a) wrap the v8 writes in a v8::TryCatch and check Maybe results, or (b) do the slot store via a separate ExecuteScript("window.__mbslots[H]=...") — but that needs the value in JS scope. The jsValue object model remains a dedicated, careful-v8 effort, NOT a bounded tick.
- docs: README — replaced the stale flat C-ABI list (showed ~40 of 72 fns) with an accurate grouped overview of all 72 mb_capi functions + a canonical flow snippet; verified every documented name exists in mb_capi.h (0 drift). README-only.
- capi: mbPostURL — host-driven POST navigation (MbWebView::PostURL → MbFetchUrl with POST; the loader already supported it, defaults Content-Type to form-urlencoded). Completes GET-only mbLoadURL. Net-gated case 43 (httpbin/post echoes mbk=postval -> status 200; net suite 145/145). Default mb_smoke 132/132.
- capi: mbScrollTo + mb_shot --scroll-to — absolute viewport scroll (window.scrollTo via the eval path), distinct from --full's resize (fixed/sticky render correctly). Applied before extract/capture so --eval + the shot see the scrolled state. mb_smoke 132/132 (scrollY==250); mb_shot --scroll-to 250 -> 250.
- docs: README — added a "wke compatibility layer" section (supported surface, grouped, + a canonical headless usage example) and fixed the architecture diagram (wke is real now, not "future"); noted the ABI has outgrown the listed core. Verified all 44 documented wke symbols exist in wke.h. README-only (no code; tests unchanged).
- wke+capi: jsTypeOf — host EvalWithType captures the result's JS type from the SAME single eval (no re-run/side-effects), exposed as mbEvalJSEx(value+type); wke's jsValue registry now stores {value,type} and jsTypeOf maps it. wke_smoke 21/21 (number/string/boolean/array/object/null/undefined/function). mbEvalJS unchanged (mb_smoke 131/131).
- wke: mouse-wheel input — wkeFireMouseWheelEvent (→mbSendScroll, dy=-delta; Win32 positive=up). Completes mouse+keyboard+wheel. wke_smoke 20/20 (tall page scrolls down, scrollY>0).
- wke: document-ready callback + page source — wkeOnDocumentReady (fires in FireLoadCallbacks) + wkeGetSource (→mbGetHTML, cached). wke_smoke 19/19 (callback fires; source contains the post-JS markup).
- wke: console callback — wkeOnConsole + wkeConsoleLevel; drains mbDrainConsole after load + after wkeRunJS, one msg per "level: text" line, mapping mb levels (log/warn/error/verbose) to the wke enum. wke_smoke 18/18 (console.log/error captured with levels).
- wke: async callbacks — wkeOnLoadingFinish + wkeOnTitleChanged + wkeString/wkeGetString + wkeLoadingResult; fired (synchronously, since load is sync) at the end of wkeLoadURL/LoadHTML. wke_smoke 17/17 (both callbacks fire with the title + SUCCEEDED via the void* param).
- wke: rendering accessors — wkeSetTransparent (→mbSetTransparentBackground) + wkeGetContentWidth/Height (→mbGetContentSize). wke_smoke 16/16 (content size of a tall doc; transparent unpainted alpha 0).
- wke: navigation history — wkeCanGoBack/GoBack/CanGoForward/GoForward over mbGo*. wke_smoke 14/14.
- wke: keyboard — wkeFireKeyDown/Up/PressEvent (charCode→UTF-8→mbSendText; VK→mbSendKey). 13/13.
- wke: scripting — wkeRunJS + jsValue readers (string-backed handle registry). 11/11.
- wke: mouse — wkeFireMouseEvent (WKE_MSG_* → mbSendMouseMove/Click). 6/6.
- wke: STARTED the layer — src/wke/{wke.h,wke.cc,wke_smoke.cc}; lifecycle/load/paint; built into the host lib + a wke_smoke target; build.sh stages src/wke.
- loader: redirect control — mbSetFollowRedirects + mb_shot --no-follow (read 30x + Location without following).
- loader: response headers — mbGetResponseHeaders + mb_shot --headers.
- loader: HTTP load status — mbGetHttpStatus; replaced mb_shot's fragile "<512B = failed" heuristic.
- capi: in-memory PNG — mbEncodePng (no temp file; view-retained bytes).
- loader: cookie-jar persistence — mbSaveCookies/mbLoadCookies + mb_shot --save/--load-cookies.
- loader: TLS cert bypass — mbSetIgnoreCertErrors + mb_shot --insecure.
- loader: proxy — mbSetProxy + mb_shot --proxy.
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
