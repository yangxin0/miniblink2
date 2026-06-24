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
- **`mb_capi` C API — 108 functions:** lifecycle, load, JS eval, scraping
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
  `--block`/`--user-agent`/`--set-cookie`/`--load-cookies`/`--save-cookies`.
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
- **Tests:** `mb_smoke` **173/173** (default, network-free), `wke_smoke` **100/100**,
  and the `mb_shot` CLI smoke `mb_shot_smoke.sh` **22/22** (drives the binary, asserts
  stdout) — all deterministic, no survivors. `MB_NET_TESTS=1` adds httpbin/example.com/
  badssl cases (use a generous watchdog ≥180s; cases SKIP when a host is unreachable).
- **Donor patches (`patches/`):** 0001 offscreen-widget-compat, 0002 suppress-js-dialogs,
  0003 enable-blob-Register, 0004 blob-url-loader-bypass.

## Known gaps / deferred (detail in the archive)
- **Heavy / multi-session:** Web Workers (inert — needs a real worker thread + isolate),
  WebGL (needs the GPU/command-buffer pipeline), page-driven `history.back()` (needs a
  ~171-method LocalFrameHost shim), a Cocoa windowed port app.
- **Minor / by-design:** `<meta name=viewport>` directives are ignored (desktop-mode
  WebView — the layout viewport is always the view size); responsive layout still works
  because width media queries track the view size, so mobile screenshots = a narrow
  view (case 14d). Color emoji render monochrome (no color-emoji font bundled, case 56b).
- **Reverted as unsafe:** host-controlled JS dialog responses (`mbSetJsDialogPolicy`) —
  showing real modal dialogs intermittently FATALs in `thread_collision_warner`
  (the modal-dialog / ScopedPagePauser threading needs understanding first). Related:
  `mbEvalJS` aborts if a script opens a modal dialog, so the host runs dialog-triggering
  scripts via `mbRunJS`.

## Recent log (newest first; full history in the archive)
- test: lock in --mobile + --full composition (full-page mobile screenshot) (2026-06-25). The full-page mobile screenshot is a top real workflow, and --mobile (narrow view) composing with --full (resize to content height) could interact badly. Verified it doesn't: a tall responsive page under --mobile --full keeps innerWidth 390, captures the full height (PNG 1170x4620 = 390x3 wide by 1540x3 tall), and applies the mobile media-query rule (marker rgb(0,255,0)). Locked in as an mb_shot_smoke case (now 29/29) asserting innerWidth 390 + full-height available + mobile rule via --eval. Test-only (git: just the .sh); binaries + lib unchanged, mb_smoke 176/176, wke_smoke 100/100. no survivors. ABI unchanged (108).
- mb_shot: --mobile — one-flag phone emulation preset (2026-06-25). Builds on last tick's verified recipe (mobile = narrow view + mobile UA, since width media queries track the view size). --mobile presets a 390x844 viewport + devicePixelRatio 3 + an iPhone Safari UA. Override semantics: an explicit width/height positional, --scale, or --user-agent each still wins (tracked scale_set so --scale 1 isn't mistaken for the default). No new ABI. VERIFIED: --mobile alone -> innerWidth 390, DPR 3, /iPhone/ UA true, @media(max-width:500px) mobile rule rgb(2,2,2); --mobile 700 500 --scale 1 --user-agent Custom -> 700/DPR1/CustomUA/desktop rule (all overrides win). Added mb_shot_smoke case (now 28/28). mb_smoke 176/176, wke_smoke 100/100, no survivors. ABI unchanged (108).
- test: characterize viewport handling — media queries track view size; <meta viewport> ignored (2026-06-25). Probed mobile-emulation viability. Finding: <meta name=viewport content="width=980"> is IGNORED (innerWidth stays at the view width 400, not 980) — desktop-mode WebView, layout viewport == view size. BUT the practically-important path works: width media queries track the VIEW width, so a narrow view renders the mobile layout. Clean A/B: @media(max-width:500px) -> view 400 gives the mobile rule rgb(2,2,2), view 800 gives the desktop rule rgb(1,1,1). Locked both in as mb_smoke case 14d (narrow/wide media-query flip + innerWidth==400 under a width=980 meta), and recorded the <meta viewport>-ignored limitation under Known gaps. So mobile screenshots = set a narrow view (+ mobile UA); viewport-meta-driven layout is the one caveat. mb_smoke 176/176, wke_smoke 100/100, no survivors. ABI unchanged (108).
- test: lock in IntersectionObserver-driven lazy loading during auto-scroll (2026-06-25). --auto-scroll (mbScrollToBottom) claims to trigger lazy/infinite-scroll content; case 75a2 verified the scroll-EVENT path but not the dominant modern pattern — IntersectionObserver firing as a below-fold element scrolls into view (loading="lazy" / IO libraries), which exercises ForceUpdateViewportIntersections between scrolls. Clean A/B via mb_shot first: a below-fold IO-watched #lz stays "not-seen" WITHOUT --auto-scroll and becomes "LOADED" WITH it. Locked in as mb_smoke case 75a3 (below-fold IO sets a flag; before_unseen=1 -> mbScrollToBottom -> after_seen=1). Note: the page is flag-only (no growth), so grew==0 is expected here — the IO firing is the signal, not page growth (75a2 covers growth). mb_smoke 175/175, wke_smoke 100/100, no survivors. ABI unchanged (108).
- mb_shot: extend missing-file failure to the file:// URL form (consistency) (2026-06-25). Last tick fixed the bare-path branch but left an inconsistency: a missing file:// URL still exited 0 (it goes through mbLoadURL, not the ifstream branch). Added a pre-load existence probe for the file:///abs form (ifstream is_open on the stripped path), guarded to paths starting with '/' and without '%' (no decoder here, so skip encoded paths to avoid a false failure). Now both input forms fail identically. VERIFIED: file:// missing -> exit 1 + "cannot open input file", file:// real -> 0, bare missing -> 1, bare real -> 0, http(s) unaffected (status-based). Added an mb_shot_smoke case (now 27/27). mb_smoke 174/174, wke_smoke 100/100, no survivors. ABI unchanged (108).
- mb_shot: fail (exit 1) on a missing local input file, not silent blank PNG (2026-06-25). Followed last tick's exit-code finding to a concrete default-behavior footgun: a bare-path input that doesn't exist (e.g. a typo'd path) read via ifstream into empty content, committed empty HTML, and "succeeded" with exit 0 + a blank PNG — so `mb_shot typo.html out.png` silently lied. Added an f.is_open() check in the local-file-path branch: a missing/unreadable file -> stderr "cannot open input file" + exit 1; an empty-but-existing file still opens and stays valid (renders blank, exit 0). file:// and http(s) loads unaffected. VERIFIED: missing -> exit 1 + message, empty -> 0, real -> 0, file:// real -> 0. Added mb_shot_smoke cases (now 26/26). mb_smoke 174/174, wke_smoke 100/100, no survivors. ABI unchanged (108).
- mb_shot: --require CSS — assert a scrape target is present (exit 3) for scripting (2026-06-25). Probed exit-code behavior and found a real automation gap: warn-only --wait-* timeouts AND even a failed local-file load all returned exit 0 (load_ok was only computed for is_http), so a pipeline couldn't tell "data is here" from "page didn't load / element never appeared". Added --require CSS (backed by mbCountSelector): after all waits/interaction, assert >=1 match, else exit 3; capture still runs (debugging the miss). Opt-in -> default exit codes unchanged (no breakage); distinct from 1=load/capture-fail, 2=usage. VERIFIED end-to-end: present -> 0, absent -> 3, composes with --wait-selector -> 0, and --require on a missing-file load (which used to exit 0!) -> 3; no --require still 0; PNG still written on a require-miss. Added mb_shot_smoke cases (now 24/24). mb_smoke 174/174, wke_smoke 100/100, no survivors. ABI unchanged (108).
- test: lock in position:fixed correctness in full-page capture (2026-06-25). The --full screenshot path (resize to content height, paint at scroll 0) was tested for below-the-fold content but not for fixed/sticky elements — a known-tricky case where a fixed header can vanish, mis-position, or repeat down a long page. Probed with a fixed top:0 green header over 1500px of white, resized to 1500 tall: it paints ONCE at the top (y=10 rgb(0,255,0)) and does NOT repeat in the content band (y=800 rgb(255,255,255)). Locked in as mb_smoke case 14c (diagnostic message reports both pixels). mb_smoke 174/174, wke_smoke 100/100, no survivors. ABI unchanged (108).
- maint: full-target health-check + trim the recent log (2026-06-25). Health-check (last full one was 13 ticks ago): all 6 targets (miniblink_host + mb_smoke + mb_shot + mb_demo + wke_smoke + wke_demo) link clean — no bit-rot — both demos run end-to-end (mb_demo + wke_demo all steps OK), and all three suites green (mb_smoke 173/173, wke_smoke 100/100, mb_shot_smoke 22/22), no survivors. Then trimmed PROGRESS.md (had regrown to 47KB / 49 dated log entries, loaded every tick): moved the 29 oldest into docs/progress-archive-2026-06.md under a dated batch header, kept the newest 20 + the rollup; conservation-checked (first moved entry present in archive, absent here). PROGRESS.md 47KB -> 27KB. Also refreshed the stale CURRENT STATE Tests line (mb_smoke 163->173, + the mb_shot_smoke 22/22 suite). Verification + docs only; no code change. ABI unchanged (108).
- test: mb_shot_smoke end-to-end integration flow (22 cases); wke surface fully covered (2026-06-25). Confirmed all 127 wke* functions are referenced by wke_smoke (0 untested). The CLI harness tested flags in isolation but not composed; added the canonical scrape as one case — --fill #sq apple --click #go --wait-selector .res --eval JSON.stringify(rows) — which exercises the whole phase pipeline (interact -> synchronize -> extract) end to end and asserts ["apple-1","apple-2","apple-3"]. Catches phase-ordering/composition regressions a single-flag test would miss; verified it fails on a corrupted expectation (21/22, exit 1). Test-only (git: just the .sh); mb_shot binary + lib unchanged, mb_smoke 173/173, wke_smoke 100/100 unaffected. mb_shot_smoke now 22/22, no survivors. ABI unchanged (108).
- test: lock in a real multi-font set (serif/sans/mono distinct) (2026-06-25). The monochrome-emoji finding raised the broader question of whether this trimmed build collapses to one fallback font (which would silently degrade EVERY screenshot of a real site). Probed via canvas measureText of the same string in different families: serif=361, sans=367, mono=480 (all distinct), and Georgia=386 is its own font too — while Times/Arial/Courier alias to the matching generic. So the build ships a genuine serif + sans-serif + monospace, not a single fallback. Locked in as mb_smoke case 56c: asserts the three generic families render the same text at distinct, non-zero widths (true:361,367,480) — a font-config regression collapsing them would make these equal. mb_smoke 173/173, wke_smoke 100/100, no survivors. ABI unchanged (108).
- test: characterize + lock in emoji rendering (monochrome, no color font) (2026-06-25). Probed whether color emoji render in color — they do NOT: a 72px U+1F600 😀 painted via mbPaintToBitmap scans to dark=1215 light=4940 colorful=0, i.e. a real monochrome glyph, no saturated pixels (no color-emoji font bundled in this trimmed build). Honest outcome (not a hidden capability like blob/history): documented the limitation as case 56b, which guards BOTH directions — fails if emoji stop rasterizing (dark/light) AND if a color font ever gets bundled (colorful>0, prompting a comment update). Degrades gracefully (monochrome glyph, not tofu/blank/crash), like the IndexedDB/Worker graceful-fail guards. mb_smoke 172/172, wke_smoke 100/100, no survivors. ABI unchanged (108).
- test: lock in SVG rendering to pixels (2026-06-25). Screenshot rendering was under-tested vs DOM scraping — the only pixel check (case 8) was a full-page red bg fill. SVG is a distinct paint path (icon/chart-heavy pages depend on it) and was unverified at the pixel level. Added case 8b: an inline 100x100 <svg> with a solid-green <rect>, painted via mbPaintToBitmap, asserts pixel (20,20) inside the rect is green — came back EXACT rgb(0,128,0) (no AA/color-mgmt drift; tolerances were conservative). mb_smoke 171/171, wke_smoke 100/100, no survivors. ABI unchanged (108).
- test: lock in legacy CJK charset decoding (Shift_JIS + GBK) (2026-06-25). The only encoding tests were latin-1 (case 81) + UTF-8 auto-detect (82); the common Asian-site legacy charsets — a real international-scraping need — were unverified. Probed empirically: a <meta charset=Shift_JIS> page with bytes 93FA 967B decodes to 日本 (U+65E5,U+672C) and a GBK page with D6D0 CEC4 decodes to 中文 (U+4E2D,U+6587), both exact — so the bundled ICU/codec data covers them. Locked in as mb_smoke case 81b (writes the raw bytes to a temp file, loads, asserts charCodeAt: sjis=2:26085,26412 gbk=2:20013,25991). mb_smoke 170/170, wke_smoke 100/100, no survivors. ABI unchanged (108).
- mb_shot: --html-for CSS (element outerHTML); de-flake blob case 46b (2026-06-25). Added --html-for CSS exposing mbGetHtmlForSelector — extract one fragment's outerHTML (article body / table / card) vs --html's whole document; no new ABI; empty + stderr warning on no-match. VERIFIED: --html-for article#a -> "<article id=\"a\">...</article>", no-match -> empty+warning; mb_shot_smoke case added (now 21/21). SEPARATELY caught + fixed a flake: case 46b (blob: URL fetch, added last tick) intermittently failed in the long-lived suite with r=BAD while passing 13/13 via single-shot mb_shot — diagnosed as a transient blob: URL [Sync] Register/fetch ordering race on the service thread under suite load (NOT a product bug; mb_shot showed t=307200 every time). De-flaked 46b with a bounded in-page retry (up to 25x/40ms) that passes immediately when blob: URLs resolve yet still fails BAD:<lengths> if they truly don't. mb_smoke 169/169 (stable across 3 runs, blob r=OK each), wke_smoke 100/100, no survivors. ABI unchanged (108).
- test: lock in History API (SPA routing) support (2026-06-25). Probed two deferred/untested capabilities. IndexedDB: confirmed it still fails GRACEFULLY (open()->onerror AbortError, no hang/crash) — matches existing case 39, no hidden capability, IDB backend stays correctly deferred. history.pushState/replaceState: found WORKING and untested — pushState/replaceState update location.pathname/search + history.state, and (the embedder-relevant part) mbGetURL reflects the new URL, so a scraper sees the current SPA route. Locked it in: mb_smoke case 39b on an https origin asserts pushState -> mbGetURL "https://spa.test/page/two?q=1" + location "/page/two?q=1", replaceState -> mbGetURL "https://spa.test/page/three", history.state.a==2. mb_smoke 169/169, wke_smoke 100/100, no survivors. ABI unchanged (108).
- test+docs: lock in blob: URL resolution; correct two stale "not supported" notes (2026-06-25). The blob registry header's top comment claimed "larger blobs (BytesProvider) and blob: URL resolution are TODO", and mb_smoke case 46 claimed createObjectURL "won't resolve to data" (the old 0003-skip-blob-url-register behavior). Both are STALE — the code already has an in-process MbBlobURLStore (ResolveAsURLLoaderFactory) and a BytesProvider path (>256 KB). Verified empirically: createObjectURL + fetch round-trips exact bytes for a small blob AND a 4 MB blob (readok/fetchok true, no crash). Case 46 only tested no-hang, not that bytes actually serve, so the working capability was undocumented + unguarded. Added mb_smoke case 46b (fetch a small + a 300 KB>256KB blob: URL, assert content round-trips: ready=1 r=OK), and corrected the header top comment + case 46's comment to match reality. mb_smoke 168/168, wke_smoke 100/100, no survivors. ABI unchanged (108).
- robustness: verify + lock in popup/new-window safety; clear a stale TODO (2026-06-25). mb_view_client carried a TODO to "override CreateView -> nullptr (deny popups) once signature pinned". Investigated: in M150 CreateView has MIGRATED OFF WebViewClient (the factory section is empty), so the override is impossible AND unnecessary — the default already denies popups. Verified empirically a hostile-page scenario is safe: window.open('about:blank') returns null (not an unmanaged view), and clicking a target=_blank anchor neither throws nor crashes the single-process host (exit 0, no FATAL, no survivors). Did NOT write a no-op override. Instead: (1) locked the property in with mb_smoke case 62-popup (window.open->"null", _blank click->no crash, host still responsive: open=null click=1), and (2) replaced the misleading TODO in mb_view_client.{cc,h} with an accurate note of the verified behavior. mb_smoke 167/167, wke_smoke 100/100, no survivors. ABI unchanged (108).
- test: lock in JsEscape robustness to U+2028/U+2029 line terminators (2026-06-25). Audited JsEscape (the helper that embeds selectors/values into generated eval JS): it escapes \ " \n \r but NOT U+2028/U+2029, which terminate a pre-ES2019 string literal. Verified empirically this is NOT a bug here — M150's V8 implements the ES2019 JSON-superset, so a filled value "a<U+2028>b" round-trips intact (mb_shot --fill -> .value length 3, charCodeAt(1)==0x2028). Did NOT add escaping (would be cargo-culting against a working build). Instead locked the behavior in: mb_smoke case 62-sep fills "a<U+2028>b<U+2029>c" and asserts the readback is "5,8232,8233" (5 code points, 0x2028, 0x2029) — a regression guard so a future eval-embedding or V8-parser change can't silently corrupt real text (some PDFs / rich-text carry these separators). mb_smoke 166/166, wke_smoke 100/100, no survivors. ABI unchanged (108).
- test: extend mb_shot_smoke.sh to the pre-harness flags (14 -> 20 cases) (2026-06-25). The CLI harness only covered flags added after it was built; the older extraction flags (--value/--checked/--style/--html/--rect) and --click predated it and were hand-verified only. Added 6 cases — --value (input.value "preset"), --checked (checkbox "1"), --style (#msg display "block"), --html (contains "hello world"), --rect (a new ERE-match helper checkre asserts "x,y,w,h" all-integer shape), and a --click round-trip (button onclick mutates #cr -> read back "clicked") — extending the shared fixture with the needed elements. VERIFIED the two new code paths (checkre + the --click round-trip) actually catch failures: corrupting the --rect regex and the --click expected value -> 18/20, exit 1, with the real rect "8,120,1184,18" shown. Test-only (git: just the .sh); mb_shot binary + lib unchanged, so mb_smoke 165/165 and wke_smoke 100/100 unaffected. mb_shot_smoke now 20/20, no survivors. ABI unchanged (108).
- mb_shot: --wait-eval JS — wait until an arbitrary JS condition is truthy (2026-06-25). The wait set covered selector appear/visible/hidden + networkidle + fixed ms, but not the general "wait until any JS expression is truthy" (window.appReady, items.length>10 — state a selector can't express). Exposes mbWaitForFunction (Puppeteer waitForFunction); no new ABI. Default 5000ms timeout (--wait-ms overrides); a never-truthy condition warns and proceeds (non-fatal, like the other waits). VERIFIED offline both paths: a page with setTimeout(()=>window.appReady=true,300) -> --wait-eval "window.appReady===true" blocks then #r reads "READY" (proves it waited — page settles before the timer); a never-set condition with --wait-ms 600 -> "never became truthy" warning, exit 0. Added a regression case to mb_shot_smoke.sh (now 14/14): a 150ms-deferred flag, --wait-eval blocks until truthy -> "true". mb_smoke 165/165, wke_smoke 100/100, no survivors. ABI unchanged (108).
- mb_shot: --press KEY — trusted key press on the CLI (2026-06-25). The interact phase had --fill/--click/--drag/--dispatch but no way to press a named key, so the canonical "type a query and hit Enter to submit" wasn't expressible. Exposes mbSendKey ("Enter"/"Tab"/"Escape"/arrows/...); no new ABI. Runs last in interact (after --fill/--click) then settles, so --fill q --press Enter works. VERIFIED offline with a form: --fill #q "search terms" --press Enter -> the input's keydown handler saw e.key=="Enter" AND the form onsubmit fired (DOM shows "Enter|SUBMITTED") — proving both real key delivery and the trusted default action. Added a regression case to mb_shot_smoke.sh (now 13/13): --fill #kq x --press ArrowDown -> #krec textContent "ArrowDown". mb_smoke 165/165, wke_smoke 100/100, no survivors. ABI unchanged (108).
- maint: full-target health-check — all 6 targets build, both demos + all 3 suites green (2026-06-25). After a run of mb_shot-focused ticks (building only mb_shot + mb_smoke each time), re-verified the WHOLE set against the staged sources to catch bit-rot in the un-exercised targets: miniblink_host + mb_smoke + mb_shot + mb_demo + wke_smoke + wke_demo all link clean. Ran both reference demos end-to-end — mb_demo all steps OK (incl. deferred-<img> request log, mbSaveElementPng, eval), wke_demo all steps OK (incl. <option> count, runJS-sees-appended-div, screenshot) — exit 0 each. All three suites green: mb_smoke 165/165, wke_smoke 100/100, and the new mb_shot CLI smoke 12/12. No survivors. Verification-only; no code change. ABI unchanged (108).
- test: mb_shot_smoke.sh — automated CLI regression coverage (2026-06-25). The six extraction flags added over recent ticks (--attr/--count/--title/--url/--cookies/--local+session-storage) had NO automated coverage — mb_smoke/wke_smoke test the lib/ABI, not the mb_shot binary's arg-parsing or stdout format; they were only hand-verified per tick. Added a bash harness that drives the real binary against a local fixture (offline) and asserts EXACT stdout for 12 cases incl. the cookie inject->read round-trip and the bad-size guard (exit 2 + message). Each mb_shot run is watchdog-bounded (disowned so no SIGKILL job-noise); temp dir auto-cleaned. Wired into build.sh after the mb_smoke run. VERIFIED the harness is real (not always-green): a missing binary -> exit 1; a deliberately-corrupted expectation -> "[FAIL] --count: expected [999] got [3]", 11/12, exit 1. Live run: 12/12 passed, no survivors. Also refreshed README's stale mb_smoke count (157->165) and documented the new CLI test. No C++ change; mb_smoke 165/165 (clean build with the staged .sh, which GN ignores). ABI unchanged (108).
- mb_shot: --local-storage KEY / --session-storage KEY — Web Storage read-out (2026-06-25). Completes CLI inspection for the third client-side store (after cookies): read an SPA's auth token / app state from localStorage or sessionStorage. Backed by mbGetLocalStorage/mbGetSessionStorage (no new ABI); absent key -> empty line + stderr "not set" warning (getter -1), mirroring --value. Storage needs a non-opaque origin — empirically confirmed file:// works here (about:blank is opaque, per mb_smoke case 102b6 which uses an https base), so it's offline-verifiable. VERIFIED offline: a page whose inline script sets localStorage.auth='tok-99' + sessionStorage.cart='3 items' -> --local-storage auth prints "tok-99", --session-storage cart prints "3 items"; an absent key -> empty + "not set" (exit 0). mb_smoke 165/165, wke_smoke 100/100, no survivors. ABI unchanged (108).
- mb_shot: --cookies URL — print a jar origin's cookies to stdout (2026-06-25). Completes the cookie workflow: --set-cookie/--load-cookies/--save-cookies could inject/persist but nothing printed cookies to stdout for inspection (grab a session token after a login flow). Backed by mbGetCookies (no new ABI). Design note: I first wired it to the current page (mbGetURL) but that can't be verified non-empty offline — a failed offline http load leaves the URL empty, and file:// has no cookies. Switched to an explicit --cookies URL (the jar is origin-keyed in memory, like mb_smoke case 87), which is BOTH end-to-end offline-verifiable AND more flexible (any origin, regardless of how the page loaded); mirrors the --set-cookie URL form. VERIFIED offline round-trip: --set-cookie https://shop.test/ sid=abc123 + theme=dark -> --cookies https://shop.test/ prints "sid=abc123; theme=dark"; an origin with no cookies and a non-http URL -> empty; all exit 0. mb_smoke 165/165, wke_smoke 100/100, no survivors. ABI unchanged (108).
- mb_shot: --title and --url — basic page-metadata extraction (2026-06-25). The CLI scraped text/html/attributes but not the two most basic fields: the page title and the landing URL. Added --title (mbGetTitle) and --url (mbGetURL, the current document URL post-redirect) — both back existing capi getters, no new ABI. --text/--html use eval, so these were genuine gaps. VERIFIED offline: a <title>Hello — Café ☕</title> page -> --title prints the exact UTF-8 (em-dash + accent + emoji intact, also exercising the CopyToBuffer boundary path) and --url prints the file:// URL; an empty-title page -> empty line. mb_smoke 165/165, wke_smoke 100/100, no survivors. ABI unchanged (108).
- mb_shot: --count CSS — number of matching elements on the CLI (2026-06-25). Exposes the existing mbCountSelector (querySelectorAll length) — the common "how many results/rows/items" check, previously needing --eval. No new ABI. Prints the count (>=0; 0 valid) + newline; an invalid/null selector (capi -> -1) prints 0 and warns on stderr. Mirrors --checked's integer-output convention. VERIFIED end-to-end offline (3 .row li + 1 p): --count li.row -> 3, --count p -> 1, --count .none -> 0, --count '>>>bad' -> 0 + "invalid selector" warning, all exit 0. mb_smoke 165/165, wke_smoke 100/100, no survivors. ABI unchanged (108).
- mb_shot: --attr CSS NAME — single first-match attribute, + a bad-size guard (2026-06-25). The CLI exposed --attr-all (JSON array across all matches) but not the far more common "give me the first match's href/src/content" — previously needing --eval gymnastics. Added --attr CSS NAME backed by the existing mbGetAttribute (no new ABI): prints the first match's attribute + newline; empty line + stderr warning when no element matches or the attribute is absent (mbGetAttribute returns -1 for both), mirroring --value's convention. While verifying I tripped a latent crash: a non-numeric width positional (the classic mistake of a "--out path" flag mb_shot doesn't have) -> atoi==0 -> a 0-width SkBitmap aborts deep in Skia's PNG encoder (SIGABRT). Added a fail-fast w<=0||h<=0 guard with a message pointing at the positional-arg usage. VERIFIED end-to-end offline: --attr a href -> "https://example.com/page?q=1", --attr img src -> "/img/logo.png", missing @title -> empty+warning (exit 0), --attr-all unaffected; bad size 0 0 and the stray --out mistake -> clean exit 2 with 0 skia fatals (was SIGABRT); normal capture still (1200x800) OK. mb_smoke 165/165, wke_smoke 100/100, no survivors. ABI unchanged (108).
- earlier (all in the archive): the C-ABI null-arg + UTF-8-truncation hardening, the mb_shot extraction/interaction flags (--set-cookie/--style/--dispatch/--scroll-to-selector and the rest), touch input (tap/swipe), mbGetCookie/mbGetViewSize/mbClearStorage, and the prior log trim. (older still: full bring-up + render + the automation/network surface — see archive.)

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
