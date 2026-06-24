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
  paint (`wkePaint`), PDF/PNG export (`wkeSavePdf`/`wkeSavePng`/
  `wkeSavePngRect`/`wkeEncodePng` in-memory), viewport scroll (`wkeScrollTo`),
  mouse (`wkeFireMouseEvent`), keyboard (`wkeFireKey*`),
  scripting (`wkeRunJS` + `jsToInt/Double/Boolean/TempString` + `jsTypeOf` +
  the full jsValue object model — reads `jsGetLength`/`jsGetAt`/`jsGet`/
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
  navigation history,
  page text (`wkeGetText`), rendering accessors
  (`wkeSetTransparent`/`wkeIsTransparent`,
  `wkeSetZoomFactor`/`wkeGetZoomFactor`, `wkeSetEditable`, `wkeSetDarkMode`,
  `wkeSetDeviceScaleFactor`, `wkeGetContentWidth/Height`),
  view-state (`wkeSetName`/`wkeGetName`,
  `wkeSetUserKeyValue`/`wkeGetUserKeyValue`), and the
  async callback model (`wkeOnLoadingFinish`/`wkeOnTitleChanged`/`wkeOnConsole`/
  `wkeOnDocumentReady` + `wkeString`), page source (`wkeGetSource`).
- **Tests:** `mb_smoke` **132/132** (default, network-free), `wke_smoke` **50/50**,
  deterministic, no survivors. `MB_NET_TESTS=1` adds httpbin/example.com cases
  (wke_smoke 54; mb_smoke ~145).
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
- wke DOM READ: wkeGetText (2026-06-24). Page visible text (document.body.innerText) — the text counterpart to wkeGetSource's HTML (wraps mbGetText, size-first into a new text_cache). VERIFIED offline: includes rendered "Title"/"Hello world", excludes <script> contents and raw markup. wke_smoke 50/50, mb_smoke 132/132, no survivors.
- wke DOM ACTIONS: hover/double-click/right-click/focus/blur by selector (2026-06-24). Five more pointer/focus selector actions (wrap mbHover/DoubleClick/RightClick/Focus/BlurSelector); each returns whether it matched. Documented PORT EXTENSIONS — completes the selector-action set. VERIFIED offline: #h onmouseover, #d ondblclick, #r oncontextmenu each fire; focus sets document.activeElement.id=="f", blur clears it; misses return false. wke_smoke 49/49, mb_smoke 132/132, no survivors.
- wke DOM ACTION: wkeScrollIntoView (2026-06-24). Scroll the first selector match into the viewport — trigger lazy loading or frame an element before a screenshot (wraps mbScrollIntoView). Documented PORT EXTENSION. VERIFIED offline: a #t target below a 3000px spacer, with scrollY reset to 0, scrolls in — scrollY rises >0 and the element's box lands within the 600px viewport (via wkeGetElementRect); non-match returns false. wke_smoke 48/48, mb_smoke 132/132, no survivors.
- wke DOM STYLE: wkeGetComputedStyle (2026-06-24). Resolved computed value of a CSS property for the first selector match (getComputedStyle→getPropertyValue: color→"rgb(r, g, b)", display:none→"none"), view-owned temp string, "" on miss — wraps mbGetComputedStyle. For visibility/style assertions without writing JS. New computed_style_cache backs the return. Documented PORT EXTENSION. VERIFIED offline: #d color=="rgb(1, 2, 3)", display=="none", #none=="". wke_smoke 47/47, mb_smoke 132/132, no survivors.
- wke DOM GEOMETRY: wkeGetElementRect (2026-06-24). Viewport-relative bounding box (logical px) of the first selector match into *x/*y/*w/*h (any NULL OK) — wraps mbGetElementRect; compose with wkeSavePngRect (element shot) or wkeFireMouseEvent (precise click). Documented PORT EXTENSION. VERIFIED offline: a position:absolute div at 30,40 sized 120x60 reports exactly that; NULL out-params tolerated; non-match returns false. wke_smoke 46/46, mb_smoke 132/132, no survivors.
- wke WAITS: wkeWaitForSelector + wkeWaitForFunction (2026-06-24). Pump the loop until a condition holds or timeoutMs elapses (wrap mbWaitForSelector/mbWaitForFunction) — the missing piece for SPA/dynamic content. Selector wait = first match exists; function wait = JS expr truthy (exceptions=false). Documented PORT EXTENSIONS. VERIFIED offline: a setTimeout(50ms) adds #ready / sets window.__ready2 and the waits catch them (true), while #never/window.__never time out (false). wke_smoke 45/45, mb_smoke 132/132, no survivors.
- wke DOM ACTIONS: wkeClickSelector + wkeFillSelector + wkeSelectOption (2026-06-24). Drive the page without writing JS (wrap mbClickSelector/mbFillSelector/mbSelectOption); each returns whether it acted. Click resolves the element box + dispatches a real click; fill sets an input value and fires input+change (React-friendly); select picks a <select> option by value/visible text. Documented PORT EXTENSIONS. VERIFIED offline: #go click bumps window.__c to 1, #name fill sets .value=="Ada Lovelace" + fires oninput, #sel select sets .value=="y"; every miss returns false. wke_smoke 44/44, mb_smoke 132/132, no survivors.
- wke DOM QUERY: wkeCountSelector + wkeGetTextForSelector + wkeGetAttribute (2026-06-24). Read-only DOM scraping without writing JS (wrap mbCountSelector/mbGetTextForSelector/mbGetAttribute). Count = querySelectorAll length (0 valid, -1 bad selector); text/attribute return the first match's innerText/attr value as a view-owned temp string ("" on miss/absent). Two new struct caches back the temp returns. Documented PORT EXTENSIONS. VERIFIED offline: .it count==3, .none==0, null==-1; h1.t text=="Hi"; .it:nth-of-type(2)=="b"; #lnk[href]=="https://ex.com/p"; misses (.none / absent attr) yield "". wke_smoke 43/43, mb_smoke 132/132, no survivors.
- wke OUTPUT: wkeEncodePng (2026-06-24). Render the current frame to in-memory PNG bytes (no temp file) for embedders that serve the bytes — wraps mbEncodePng, preserving the view-owned/valid-until-next-call contract. Documented PORT EXTENSION. VERIFIED offline: full 8-byte PNG signature + IHDR width/height match the requested 160×90; null view/out are safe (return 0). wke_smoke 42/42, mb_smoke 132/132, no survivors.
- wke VIEWPORT: wkeScrollTo (2026-06-24). Absolute scroll of the layout viewport to (x,y) in CSS px (wraps mbScrollTo → window.scrollTo); the real viewport moves so fixed/sticky elements render right — pair with wkeSavePng/wkePaint for long-page viewport shots. Documented PORT EXTENSION. VERIFIED offline: a 3000px-tall page scrolls to window.scrollY==250 then back to 0. wke_smoke 41/41, mb_smoke 132/132, no survivors.
- wke RENDERING: wkeSetDeviceScaleFactor (2026-06-24). HiDPI/retina (wraps mbSetDeviceScaleFactor → Blink InspectorDeviceScaleFactorOverride): window.devicePixelRatio reports the scale and paint/PNG rasterizes at scale×, layout stays in CSS px. Documented PORT EXTENSION (modern). VERIFIED offline + doubly: at dsf=2, devicePixelRatio==2 AND a 100×60 logical-rect capture decodes to 200×120 IHDR. Builds on last tick's wkeSavePngRect. wke_smoke 40/40, mb_smoke 132/132, no survivors.
- wke OUTPUT: wkeSavePngRect (2026-06-24). Render just a logical rect (x,y,w,h) to a PNG — element/region screenshot (wraps mbSavePngRect; output w*dsf x h*dsf). Documented PORT EXTENSION. VERIFIED offline + strongly: the saved PNG's IHDR width/height (big-endian at byte offsets 16/20) equal the requested 120x80 (dsf=1), not just the magic; null view/path safe. wke_smoke 39/39, mb_smoke 132/132, no survivors.
- wke OUTPUT: wkeSavePng (2026-06-24). Render the current frame at WxH and save it; format follows the extension (.jpg/.jpeg→JPEG q90, else PNG) — wraps mbSavePng. Documented PORT EXTENSION (classic wke captures via wkePaint then app-encodes). VERIFIED offline: a .png starts with the \x89PNG signature (size>100) and a .jpg starts with the JPEG SOI FF D8; null view/path safe. wke_smoke 38/38, mb_smoke 132/132, no survivors.
- wke OUTPUT: wkeSavePdf (2026-06-24). Print the current document to a multi-page US-Letter PDF (wraps mbSavePdf). Documented PORT EXTENSION (no classic wke print API). VERIFIED offline: prints a real file whose first 5 bytes are "%PDF-" and size > 500, and null view/path are safe. wke_smoke 37/37, mb_smoke 132/132, no survivors.
- wke SCRIPTING: wkeSetInitScript (2026-06-24). evaluateOnNewDocument-style hook — runs a script in each new document BEFORE the page's own scripts (wraps mbSetInitScript → RunDocumentStartScript at document-element-available). Lets an app set globals / stub APIs / install a harness the page then observes. NULL/"" clears. Documented PORT EXTENSION. VERIFIED offline: init sets window.__early, the page's inline <script> reads it into window.__pageSaw=="injected"; after clearing, a reload reads "no". wke_smoke 36/36, mb_smoke 132/132, no survivors.
- wke I18N: wkeSetLocale + wkeSetTimezone (2026-06-24). Emulate the i18n environment for deterministic localized rendering (wrap mbSetLocale/mbSetTimezone). wkeSetLocale drives navigator.language(s); wkeSetTimezone overrides Date/Intl (process-global, IANA id). Documented PORT EXTENSIONS. VERIFIED offline: navigator.language=="fr-FR" + languages=="fr-FR,fr,en"; Intl resolvedOptions().timeZone=="America/New_York" + Date(1609459200000).getHours()==19 (EST). Test restores en-US/UTC after (timezone is process-global). wke_smoke 35/35, mb_smoke 132/132, no survivors.
- wke RENDERING: wkeSetDarkMode (2026-06-24). Emulate prefers-color-scheme dark/light (wraps mbSetDarkMode → Blink SetPreferredColorScheme); persists across loads, set before navigating. Documented PORT EXTENSION (modern, not classic wke). VERIFIED offline + deterministically: a page with a @media(prefers-color-scheme:dark) rule reports matchMedia=false + color rgb(1,1,1) in light and matchMedia=true + rgb(2,2,2) in dark. wke_smoke 34/34, mb_smoke 132/132, no survivors.
- wke NETWORK: wkeSetExtraHeaders (2026-06-24). Per-view request header injection (newline-separated "Name: Value" lines, NULL/"" clears) wrapping mbSetExtraHeaders. Documented PORT EXTENSION — classic wke injects per-request via the net hook; this is the simple global form. Verified offline (set/clear/null safe, local loads still work) and over the network (MB_NET_TESTS): set X-Wke-Test: zzz9, load httpbin.org/headers, response JSON echoes zzz9. wke_smoke 33/33 default, 37/37 net, mb_smoke 132/132, no survivors. Fixed a -Wshadow (renamed a `body` local colliding with the postURL test's).
- wke EDITING: wkeSetEditable (2026-06-24). Whole-document editability modeled as document.designMode, stored per-view and re-applied after each navigation via a new ApplyEditable() in FireLoadCallbacks (same pattern as zoom). VERIFIED offline: designMode "off"→"on"→(persists across a fresh load)→"off", and document.body.isContentEditable reads true while on. wke_smoke 32/32, mb_smoke 132/132, no survivors.
- wke RENDERING: wkeSetZoomFactor/wkeGetZoomFactor (2026-06-24). Page zoom modeled as CSS `zoom` on the document element (scales layout + the rects getBoundingClientRect reports), stored per-view and re-applied after every navigation via a new ApplyZoom() in FireLoadCallbacks. Non-positive factors ignored. VERIFIED observably + offline: a 100px div reports width 100 at 1.0, 200 at 2.0, and stays 200 after loading a fresh document (proves persistence). wke_smoke 31/31, mb_smoke 132/132, no survivors. (Honest deviation documented: real wke uses Blink page zoom; this port approximates via CSS zoom on the current document.)
- wke VIEW-STATE: wkeIsTransparent + wkeSetName/wkeGetName + wkeSetUserKeyValue/wkeGetUserKeyValue (2026-06-24). Pure wke view-state accessors (no engine backing, 100% faithful semantics): wkeSetTransparent now mirrors its flag for wkeIsTransparent; name labels the view (default ""); the per-view std::map<string,void*> user store lets an app thread its own context through wke callbacks (app-owned, unset→NULL). Caught a real test bug — an earlier test left the view transparent, so the new test now asserts both transition directions rather than the initial state. wke_smoke 30/30, mb_smoke 132/132, no survivors.
- wke NETWORK: wkeSetProxy (HTTP/SOCKS + auth) (2026-06-24). Faithful wke proxy API — wkeProxyType enum + wkeProxy struct {type, hostname[100], port, username[50], password[50]}. Builds a curl proxy URL "scheme://[user[:pass]@]host:port" (http/socks4/socks4a/socks5/socks5h) and hands it to mbSetProxy → CURLOPT_PROXY; NULL/WKE_PROXY_NONE forces a direct connection. Fixed-buffer fields copied into +1 NUL-terminated locals before use. Verified offline (null/NONE safe, local loads still work) and over the network (MB_NET_TESTS): a bogus unresolvable proxy makes http://example.com FAIL, clearing it makes the same load SUCCEED — proving the proxy is genuinely applied. wke_smoke 29/29 default, 32/32 net, mb_smoke 132/132, no survivors.
- wke COOKIE PERSISTENCE: wkeSetCookieJarPath + Flush/Reload wired (2026-06-24). Closes last tick's loose end — wkePerformCookieCommand's FlushCookiesToFile/ReloadCookiesFromFile now persist the process-wide jar to/from a file via mbSaveCookies/mbLoadCookies. Path set by wkeSetCookieJarPath (utf8, not the Win wke WCHAR — documented; held in a leaked CookieJarPath() to dodge the exit-time-destructor -Werror). Tested OFFLINE deterministically by inspecting the saved curl/Netscape jar file: inject psid=jar987 → Flush to jar1 (file contains it) → ClearAll → Reload → re-Flush to jar2 (still contains it). wke_smoke 28/28, mb_smoke 132/132, no survivors.
- wke COOKIES: wkeGetCookie + wkeSetCookie + wkePerformCookieCommand (2026-06-24). First non-scripting wke gap closed in a while — wraps the existing mb_capi cookie jar (mbGetCookies/mbSetCookie/mbClearCookies). wkeGetCookie reads the jar for the CURRENT document URL (mbGetURL→mbGetCookies, cached temp string); wkeSetCookie injects a set-cookie string for a url's origin; wkePerformCookieCommand is view-less so it drives the process-wide jar through a tracked g_last_webview (set on create / cleared on destroy) — clear commands → mbClearCookies, file flush/reload are documented no-ops (no jar-path setter yet). Offline default test (about:blank has no cookies; setter+clear null-safe) + a NETWORK round-trip (MB_NET_TESTS) PROVEN against httpbin: /cookies/set?wkeck=ok42 → wkeGetCookie contains it → clear drops it. wke_smoke 27/27 default, 29/29 net, mb_smoke 132/132, no survivors.
- wke jsValue WRITE SIDE: jsEmptyObject/jsEmptyArray + jsSet/jsSetAt/jsSetGlobal (2026-06-24). Builders StoreEval "({})"/"([])" into fresh navigable slots; setters mutate the live slot object in place via a void IIFE eval (window.__mbslots[obj][prop|index]=(LiteralOf(value)) / window[prop]=...), so any jsValue — a constructor result or another handle — can be assigned. Closes the loop: a built object round-trips through jsCall (passed as arg, read back) and through jsGet/jsGetAt/jsGetGlobal. wke_smoke 26/26 (obj{name,n} + arr[10,20] + global + jsCallGlobal(fn,[obj])=="Ada:7"), mb_smoke 132/132, no survivors.
- wke jsValue: jsGetKeys — enumerates an object's own-enumerable property names in Object.keys order (2026-06-24). Parks Object.keys(obj) in a slot, reads each name back via jsGetAt, copies into a thread-local leaked KeysHolder (vector<string> storage + vector<const char*> ptrs) so the returned `jsKeys*` and its strings stay valid until the next call on that thread. Empty list for non-objects. Faithful to the classic wke jsKeys contract. wke_smoke 25/25 (alpha/beta/gamma order + empty for `42`), mb_smoke 132/132, no survivors.
- ✅✅ wke jsValue: CALL + CONSTRUCTORS — jsCall/jsCallGlobal + jsInt/jsDouble/jsBoolean/jsString/jsUndefined/jsNull (2026-06-24). Completes the jsValue object model. Each JsRecord now carries a `literal` (a JS expr reproducing it): slot-backed values use "window.__mbslots[h]", primitives (no jsExecState, no eval) store their JS literal ("5"/"true"/"\"x\"") — so jsCall builds "(func)(a0,a1,...)" / "(func).apply(this,[...])" by inlining each arg's literal, then StoreEval runs it. All via the safe JS-slot pattern (no C++ v8). wke_smoke 24/24, no crash: jsCallGlobal(add,[jsInt(10),jsInt(32)])=42, greet(jsString"Ada")="hi Ada", jsCall(obj.add, this=obj, [jsInt(5)])=105. mb_smoke 132/132.
- wke jsValue: jsGet + jsGetGlobal — object property reads by name + window globals, via the same safe JS-slot pattern (StoreEval of "__mbslots[obj][\"prop\"]" / "window[\"prop\"]", property name escaped as a JS string literal). Completes object+array reads. wke_smoke 23/23 (obj name/age/nested + a global), no crash; mb_smoke 132/132. Remaining jsValue gap: jsCall (invoke a function).
- ✅✅ wke jsValue OBJECT MODEL — jsGetLength + jsGetAt (2026-06-24). Landed the capability that CRASHED two ticks ago, via the SAFE approach the revert's lesson pointed to: the slot store happens IN JS (a wrapper assignment "window.__mbslots[H]=(expr)" through the proven mbEvalJSEx), NEVER from C++. wke.cc-only, no host/v8 changes. StoreEval wraps each result into __mbslots[handle] (handle==slot); the wrapper only parses for an EXPRESSION, so a statement (parse error -> empty type) falls back to a plain eval (no slot, no double-execution — the empty-type check distinguishes a parse error from a valid undefined result). jsGetAt indexes __mbslots[obj][i] into a fresh navigable handle (IIFE try/catch -> undefined out of range); jsGetLength reads .length. VERIFIED in wke_smoke (now 22/22, deterministic x2, NO crash): ['ant','bee','cat'] -> len 3, [1]=="bee"; nested [[10,20],[30,40]] -> jsGetAt(1) is an array, [0]==30. Existing scripting tests still pass through the new wrapper path. mb_smoke 132/132. Deferred: jsGet-by-name + jsCall.
- mb_shot: --post BODY — POST navigation from the CLI (→mbPostURL), completing POST across mb_capi + wke + CLI. Verified vs httpbin/post (doc echoes the body). mb_shot.cc only; mb_smoke 132/132, wke_smoke 21/21 unaffected.
- wke: wkePostURL — POST navigation wrapping mbPostURL (wke parity, no v8 risk). Default wke_smoke stays 21/21 (network-free); MB_NET_TESTS=1 -> 22/22 with the POST case (httpbin echoes the body). Added MB_NET_TESTS gating to wke_smoke.
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
