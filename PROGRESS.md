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
- **`mb_shot` CLI (the deliverable tool) — 59 flags, phase-ordered:** a full
  scraper/automator. Request-config: `--user-agent`(`--ua`)/`--header`/`--proxy`/
  `--insecure`/`--no-follow`/`--no-images`/`--post`/`--block`/`--dark`/`--lang`/
  `--tz`/`--set-cookie`/`--load-cookies`/`--save-cookies`. Interact:
  `--fill`/`--click`/`--drag`/`--dispatch`/`--press`. Synchronize:
  `--wait-selector`/`--wait-visible`/`--wait-hidden`/`--wait-eval`/`--wait-idle`/
  `--wait-ms`. Prepare-view: `--css`/`--auto-scroll`/`--scroll-to`/
  `--scroll-to-selector`. Extract (to stdout): `--text`/`--html`/`--html-for`/
  `--title`/`--url`/`--eval`/`--eval-json`/`--value`/`--checked`/`--count`/`--visible`/`--rect`/
  `--style`/`--attr`/`--text-all`/`--attr-all`/`--cookies`/`--local-storage`/
  `--session-storage`/`--requests`/`--console`/`--headers`. Capture:
  `--full`/`--scale`/`--mobile`/`--clip`/`--selector`/`--transparent` (PNG/JPEG/PDF
  by out extension). Assert (scripting): `--require` (exit 3 if absent). Exit codes:
  0 ok / 1 load+capture fail (incl. a missing local input file) / 2 usage / 3 require.
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
- **Tests:** `mb_smoke` **178/178** (default, network-free), `wke_smoke` **100/100**,
  and the `mb_shot` CLI smoke `mb_shot_smoke.sh` **62/62** (drives the binary, asserts
  stdout — covers every offline-testable flag) — all deterministic, no survivors.
  `MB_NET_TESTS=1` adds 5 reachability-gated network cases (example.com load,
  `--header`/`--post`/`--no-follow` via httpbin, `--insecure` via badssl) -> 67.
- **Donor patches (`patches/`):** 0001 offscreen-widget-compat, 0002 suppress-js-dialogs,
  0003 enable-blob-Register, 0004 blob-url-loader-bypass.

## Known gaps / deferred (detail in the archive)
- **Heavy / multi-session:** Web Workers (inert — needs a real worker thread + isolate),
  WebGL (needs the GPU/command-buffer pipeline), page-driven `history.back()` (needs a
  ~171-method LocalFrameHost shim), a Cocoa windowed port app.
- **Minor / by-design:** a document's `<meta>` CSP persists on a reused frame across
  navigations (load a strict-CSP page then another in the *same* view → the second
  page's own scripts stay blocked). Nil impact for `mb_shot` (one page per process);
  an embedder that reuses a view across navigations should create a fresh view. The
  obvious fix — a fresh empty `WebPolicyContainer` per commit — CRASHES Blink (its
  null mojo remote trips a CHECK); a real fix needs a bound PolicyContainerHost
  receiver, more plumbing than the quirk warrants (see the NOTE in CommitHtml).
  Host *extraction* (`mbEvalJS`/selectors) bypasses page CSP regardless (case 62-csp).
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
- deferred: START page-driven history.back/forward — safety anchor + scoping (2026-06-25). Began the heavy deferred item (multi-tick focused effort). Scoped it: page-initiated session history routes through LocalFrameHost.GoToEntryAtOffset (1 of 76 methods on blink::mojom::LocalFrameHost in frame.mojom — the "~171" estimate was high), which the host doesn't bind, so it drops (graceful no-op). First bounded step landed: mb_smoke case 86b anchors the safety invariant — an untrusted page calling history.back()/forward()/go() is crash-safe (returns "ok", host stays alive), which must hold before AND after the wiring. Next steps recorded in the roadmap: scaffold a 76-method MbLocalFrameHost stub bound via the existing MbNavAssociatedInterfaceProvider, then wire GoToEntryAtOffset to the mbGoBack navigation path. Also marked the wke-layer roadmap item DONE (callbacks + jsValue object model all shipped + tested). mb_smoke 179/179, wke_smoke 100/100, no survivors. ABI unchanged (108).
- maint: full health-check + trim the recent log (2026-06-25). After 12 test-only ticks (the mb_shot CLI-coverage sweep), re-verified the whole target set for bit-rot: all 6 targets link clean, both demos run end-to-end (mb_demo + wke_demo all steps OK), all 3 suites green (mb_smoke 178/178, wke_smoke 100/100, mb_shot_smoke 62/62), no survivors. Then trimmed PROGRESS.md (had regrown to 48KB / 47 dated entries): moved the 27 oldest into the archive under a dated batch header, kept the newest 20 + the rollup; conservation-checked. 48KB -> 24KB. Also refreshed the stale CURRENT STATE Tests line (mb_smoke 173->178, mb_shot_smoke 22->62 + the 5 network cases). Verification + docs only; no code change. ABI unchanged (108).
- test: net cases for --no-follow + --insecure (MB_NET_TESTS, now 5 net cases) (2026-06-25). Extended the opt-in network block with the last two CLI flags that need a live host. Verified live: --no-follow on httpbin/redirect/1 stops at the HTTP/2 302 (Location not followed; --headers shows it) and --insecure loads self-signed.badssl.com (control fails with a TLS WARNING). Fixed netok to curl -sk (reachability, not cert validation) so the bad-cert host isn't wrongly skipped. Default offline 62/62 (network skipped), MB_NET_TESTS=1 67/67. Test-only (git: just the .sh); binaries + lib unchanged, mb_smoke 178/178, wke_smoke 100/100, no survivors. ABI unchanged (108).
- test: opt-in network cases for mb_shot's real-network path (MB_NET_TESTS) (2026-06-25). The CLI network request-config flags had no coverage (they need a live echo host). Added an MB_NET_TESTS-gated block (default run stays 62 offline/deterministic — build.sh runs it network-free): each host reachability-gated (SKIP not fail). Verified live (both hosts up today): example.com loads -> --title "Example Domain"; httpbin echoes --header "X-Mb-Test: hello42" and --post "field=val99" back in the response. Default 62/62 (0 net cases run), MB_NET_TESTS=1 65/65. Test-only (git: just the .sh); binaries + lib unchanged, mb_smoke 178/178, wke_smoke 100/100, no survivors. ABI unchanged (108).
- test: cover mb_shot --transparent (60 -> 62) (2026-06-25). The last capture flag. Verified via the PNG color-type byte (IHDR offset 25, no decode/PIL needed): the default shot is opaque RGB (type 2) and --transparent yields RGBA (type 6) — exactly the alpha channel that omitting the background requires. Added a pngctype helper + 2 cases (distinct 2 vs 6 prove it's real). With this, every offline-testable mb_shot flag is covered; the only remaining gaps are network-host request-config (--proxy/--header/--insecure/--no-follow/--post, opt-in by design) and --wait-idle. Test-only (git: just the .sh); binaries + lib unchanged, mb_smoke 178/178, wke_smoke 100/100, mb_shot_smoke 62/62, no survivors. ABI unchanged (108).
- test: cover mb_shot cookie file round-trip + --drag (57 -> 60) (2026-06-25). --set-cookie + --save-cookies writes a Netscape jar file containing "xyz9", then a fresh run --load-cookies + --cookies reads back "tok=xyz9" (the persist/restore-a-login workflow). --drag #from #to mouse-drags onto the target whose mouseup handler fires -> "dropped" (dedicated fixture). Test-only (git: just the .sh); binaries + lib unchanged, mb_smoke 178/178, wke_smoke 100/100, mb_shot_smoke 60/60, no survivors. ABI unchanged (108).
- test: cover mb_shot --block + --wait-visible/--wait-hidden timeouts (54 -> 57) (2026-06-25). --block "pic.png" on a file:// <img> drops just the matching subresource -> naturalWidth 0 (reuses the --no-images real-PNG fixture; selective vs --no-images' drop-all). --wait-visible #never-vis -> "never became visible" and --wait-hidden #msg (always visible) -> "still visible at timeout", both warn-only (exit 0) — the timeout paths, like --wait-selector. Test-only (git: just the .sh); binaries + lib unchanged, mb_smoke 178/178, wke_smoke 100/100, mb_shot_smoke 57/57, no survivors. ABI unchanged (108).
- test: cover mb_shot --user-agent / --scroll-to / --no-images (50 -> 54) (2026-06-25). Three clean flags: --user-agent TestUA/9 -> navigator.userAgent "TestUA/9"; --scroll-to 500 -> window.scrollY 500 (absolute); --no-images on a FILE:// image (python writes a real 2x2 PNG so it routes through the loader) -> naturalWidth 0, vs 2 in the control (data: images are inlined and unaffected, so a file image is required). Test-only (git: just the .sh); binaries + lib unchanged, mb_smoke 178/178, wke_smoke 100/100, mb_shot_smoke 54/54, no survivors. ABI unchanged (108).
- test: cover mb_shot --scroll-to-selector + --full (48 -> 50) (2026-06-25). Added the last two clean capture/view-prep flags via a 2500px-tall fixture: --scroll-to-selector #x brings a below-fold element into view (--eval scrollY>0 -> "true"), and --full grows the PNG to the full content height (checkre ^PNG 400x[0-9]{4,}$ — a 4+-digit height, vs the 300 viewport). Test-only (git: just the .sh); binaries + lib unchanged, mb_smoke 178/178, wke_smoke 100/100, mb_shot_smoke 50/50, no survivors. ABI unchanged (108).
- test: cover mb_shot capture modes — PNG dimensions + formats (42 -> 48) (2026-06-25). The headline screenshot feature had no binary-level coverage. Added a python3 imginfo helper (reads PNG IHDR / JPEG+PDF magic from the file header) and 6 cases: default view size -> PNG 300x200, --scale 2 -> 600x400, --clip 0,0,120,60 -> 120x60, --selector #b (a 120x60 div) -> 120x60, .jpg -> JPEG, .pdf -> PDF. The 6 distinct expected values all passing proves the helper distinguishes (not vacuous). Gated on python3 (SKIP, not fail, if absent — keeps the harness portable). Test-only (git: just the .sh); binaries + lib unchanged, mb_smoke 178/178, wke_smoke 100/100, mb_shot_smoke 48/48, no survivors. ABI unchanged (108).
- test: extend mb_shot_smoke to --wait-selector timeout + --requests (39 -> 42) (2026-06-25). Probed both first: a small data: img is inlined (not logged), and the default settling already catches a 150ms-deferred element (no clean A/B). So chose the robust angles — --wait-selector for a never-appearing element with --wait-ms 300 warns "never appeared" and proceeds (exit 0, warn-only), and --requests with a dedicated fixture that <link>s a file:// CSS (routes through the loader) logs the subresource URL. Test-only (git: just the .sh); binaries + lib unchanged, mb_smoke 178/178, wke_smoke 100/100, mb_shot_smoke 42/42, no survivors. ABI unchanged (108).
- test: extend mb_shot_smoke to --dispatch + --console (37 -> 39) (2026-06-25). Added the fixture bits (a #dt element with a 'myevt' listener; a console.log marker) and two CLI cases: --dispatch #dt myevt fires the custom event -> #dt textContent "dispatched", and --console dumps the page's console to stderr (contains CONSOLE_MARKER_42). The added console.log only surfaces with --console, so the other stderr-grep cases are unaffected (all 39 pass). Test-only (git: just the .sh); binaries + lib unchanged, mb_smoke 178/178, wke_smoke 100/100, mb_shot_smoke 39/39, no survivors. ABI unchanged (108).
- test: extend mb_shot_smoke to the emulation config flags (34 -> 37) (2026-06-25). Added CLI-level coverage for --dark/--lang/--tz, all self-contained via --eval reads: --dark -> matchMedia('(prefers-color-scheme:dark)') true (vs false without), --lang fr-FR -> navigator.language "fr-FR", --tz America/New_York -> Intl.DateTimeFormat().resolvedOptions().timeZone "America/New_York". Test-only (git: just the .sh); binaries + lib unchanged, mb_smoke 178/178, wke_smoke 100/100, mb_shot_smoke 37/37, no survivors. ABI unchanged (108).
- test: extend mb_shot_smoke to list-scrape + CSS-inject flags (31 -> 34) (2026-06-25). The CLI harness covered the single-value extractors but not the list-scraping JSON-array flags or --css. Added 3: --text-all li.row -> ["a","b","c"], --attr-all a href -> ["https://example.com/x"], and a clean A/B for --css (inject "#msg{display:none}" -> --visible #msg flips 1 -> 0, proving the stylesheet lands before extract/capture). Test-only (git: just the .sh); binaries + lib unchanged, mb_smoke 178/178, wke_smoke 100/100, mb_shot_smoke 34/34, no survivors. ABI unchanged (108).
- maint: full-target health-check (2026-06-25). ~12 ticks of feature/test work built only mb_smoke + mb_shot, and the lib changed last tick — re-verified the whole set for bit-rot: all 6 targets (miniblink_host + mb_smoke + mb_shot + mb_demo + wke_smoke + wke_demo) link clean, both reference demos run end-to-end (mb_demo + wke_demo all steps OK, exit 0), and all three suites green (mb_smoke 178/178, wke_smoke 100/100, mb_shot_smoke 31/31). No survivors. Verification-only; no code change. ABI unchanged (108).
- investigate: CSP-frame-reuse fix attempt — reverted (null-remote crash) (2026-06-25). Tried to fix the documented CSP-leak-across-navigation quirk by giving each CommitHtml a fresh empty WebPolicyContainer (so a prior <meta> CSP wouldn't carry over). Hypothesis was right about the cause (null policy_container -> inherited policies) but the fix CRASHES: an empty WebPolicyContainer has a null mojo remote, which trips a CHECK at commit (mb_smoke SIGABRT, exit 134, before any case). A real fix needs a bound PolicyContainerHost receiver — more plumbing than the low-impact quirk warrants. Reverted to the working null-policy_container behavior; left a NOTE in CommitHtml + expanded the Known-gaps entry so the dead end isn't re-attempted. Net: a documented negative result; no behavior change. mb_smoke 178/178, wke_smoke 100/100, no survivors. ABI unchanged (108).
- test: lock in iframe RENDERING into the parent paint (2026-06-25). Cases 77/78 covered iframe DOM (contentDocument), but not whether child-frame content composites into the SCREENSHOT — the distinct concern for capturing pages with ads/embeds/maps/widgets. Added case 77b: a solid-green srcdoc iframe at the top-left, paint to bitmap, read a pixel inside its box -> exact rgb(0,128,0), so cross-frame compositing works. (Caught two -Wshadow build errors first — my px/b vectors shadowed case 8's function-scope names; renamed to ifpx/pr,pg,pb. The build's -Werror is a good guardrail.) mb_smoke 178/178, wke_smoke 100/100, no survivors. ABI unchanged (108).
- test: lock in CSP-strict scraping (host extraction bypasses page CSP) (2026-06-25). Probed a high-stakes, untested scraping concern: does the host's eval-based extraction work on a strict-CSP page? A large fraction of the real web ships script-src without unsafe-eval. Finding: YES — under script-src 'none'; default-src 'none', the page's own inline script is blocked (typeof window.__pageran=='undefined') but mbGetTextForSelector/mbCountSelector/mbEvalJS all still read the DOM (host eval runs in a privileged context like DevTools). So CSP-protected sites scrape fully. Locked in as mb_smoke case 62-csp. Caught + handled a real subtlety in the process: <meta> CSP persists on a reused frame, so the first attempt (on the shared view) leaked script-src 'none' into 14 later cases — isolated it on a dedicated mbCreateView/mbDestroyView, and recorded the frame-reuse quirk under Known gaps (nil impact for mb_shot's one-page-per-process model). mb_smoke 177/177, wke_smoke 100/100, no survivors. ABI unchanged (108).
- mb_shot: --eval-json JS — structured extraction as real JSON (2026-06-25). Probed --eval's return coercion and found the footgun: an array -> lossy comma-join ("1,2,3"), an object -> "[object Object]" — useless for the dominant structured-scraping case. Added --eval-json, which wraps the expression in JSON.stringify((expr)): objects/arrays come out as real JSON. Opt-in, no breakage; a non-serializable value (undefined/function) prints empty. VERIFIED: ({a:1,b:2}) -> {"a":1,"b":2}, [1,2,3] -> [1,2,3] (vs --eval's "1,2,3"), and the killer case mapping <a> rows -> [{"text":"One","href":"/a"},{"text":"Two","href":"/b"}]. Added mb_shot_smoke cases (now 31/31). 59 flags now. mb_smoke 176/176, wke_smoke 100/100, no survivors. ABI unchanged (108).
- docs: refresh the stale CURRENT STATE mb_shot bullet (now all 58 flags) (2026-06-25). The loop-state "read first" doc's CLI section had drifted ~15 flags behind the binary over the recent run (missing --attr/--count/--title/--url/--cookies/--html-for/--press/--wait-eval/--require/--mobile/--local+session-storage/--scroll-to*/--console/--transparent/--scale/--dark/--lang/--tz/--no-images/--post/--header). Rewrote it phase-ordered (request-config / interact / synchronize / prepare-view / extract / capture / assert) covering all 58, plus the exit-code contract (0/1/2/3). Cross-checked programmatically: 0 of 58 flags missing from the bullet. Docs-only (git: just PROGRESS.md); binaries + suites unchanged, mb_smoke 176/176, wke_smoke 100/100, no survivors. ABI unchanged (108).
- test: lock in --mobile + --full composition (full-page mobile screenshot) (2026-06-25). The full-page mobile screenshot is a top real workflow, and --mobile (narrow view) composing with --full (resize to content height) could interact badly. Verified it doesn't: a tall responsive page under --mobile --full keeps innerWidth 390, captures the full height (PNG 1170x4620 = 390x3 wide by 1540x3 tall), and applies the mobile media-query rule (marker rgb(0,255,0)). Locked in as an mb_shot_smoke case (now 29/29) asserting innerWidth 390 + full-height available + mobile rule via --eval. Test-only (git: just the .sh); binaries + lib unchanged, mb_smoke 176/176, wke_smoke 100/100. no survivors. ABI unchanged (108).
- mb_shot: --mobile — one-flag phone emulation preset (2026-06-25). Builds on last tick's verified recipe (mobile = narrow view + mobile UA, since width media queries track the view size). --mobile presets a 390x844 viewport + devicePixelRatio 3 + an iPhone Safari UA. Override semantics: an explicit width/height positional, --scale, or --user-agent each still wins (tracked scale_set so --scale 1 isn't mistaken for the default). No new ABI. VERIFIED: --mobile alone -> innerWidth 390, DPR 3, /iPhone/ UA true, @media(max-width:500px) mobile rule rgb(2,2,2); --mobile 700 500 --scale 1 --user-agent Custom -> 700/DPR1/CustomUA/desktop rule (all overrides win). Added mb_shot_smoke case (now 28/28). mb_smoke 176/176, wke_smoke 100/100, no survivors. ABI unchanged (108).
- earlier (all in the archive): the full mb_shot CLI feature build-out (--attr/--count/--title/--url/--cookies/--html-for/--press/--wait-eval/--require/--mobile/--eval-json/storage), the exit-code contract + missing-file fixes, and the capability/limitation audits (blob/history/SVG/fonts/CJK/iframe/CSP/popup/viewport/emoji). (older still: full bring-up + render + the automation/network surface — see archive.)

## REMAINING ROADMAP
- **wke layer: DONE.** The async callback model (wkeOnLoadingFinish/OnTitleChanged/
  OnConsole/OnDocumentReady + wkeString), accessors (wkeGetCookie/wkeGetSource), and
  the full V8-backed jsValue object model (jsTypeOf, jsIs* predicates, jsGet*/jsSet*,
  jsEmptyObject/jsEmptyArray builders, jsCall/jsCallGlobal) all shipped; all 127 wke*
  functions are tested by wke_smoke (100/100).
- **IN PROGRESS — page-driven history.back/forward.** Scoped: page-initiated session
  history routes through `LocalFrameHost.GoToEntryAtOffset(int32 offset, ...)` (one of
  76 methods on the `blink::mojom::LocalFrameHost` interface in frame.mojom), which the
  host doesn't bind -> the call is dropped (a graceful no-op; host-driven mbGoBack/
  mbGoForward already work, case 86). Plan: (1) [DONE] anchor the safety invariant —
  history.back/forward/go from JS is crash-safe (case 86b); (2) scaffold an
  MbLocalFrameHost stub (76 no-op methods) and bind it via the existing
  MbNavAssociatedInterfaceProvider (the BlobURLStore pattern), compile-verify; (3) wire
  GoToEntryAtOffset to the WebView's navigation controller (the mbGoBack path) + a test
  that a page's history.back() actually navigates. Risk: mojo binding can CHECK-fail
  on a null remote (cf. the reverted policy-container fix) — hence the incremental,
  revert-safe steps.
- **Heavy items (own focused efforts):** Web Workers (worker thread + isolate); WebGL
  (GPU pipeline); a Cocoa windowed port app under `port/mac` (note: the window itself
  can't be empirically verified headlessly here).
- **Mode:** each tick = one bounded, empirically-verified improvement; build green,
  smoke green, no survivors; commit per-milestone with the author/no-trailer convention.
