# miniblink-modern — plan & loop state

> A **standalone, single-process embedder of modern Blink (Chromium M150)**: a
> hand-written tiny "content layer" (`miniblink_host`) that boots the real Blink
> engine in-process and renders HTML/CSS/JS to pixels through a pure-C ABI
> (`mb_capi`), with a `wke` compatibility layer and an `mb_shot` CLI on top.
> **No CEF, no browser process, no cross-process Mojo.** Successor to miniblink49
> (which froze at ~M47/2015).
>
> This file is the single source of truth for the loop. Read it, do the next
> **substantive** step, verify it empirically, update this file, commit, repeat.

---

## ⚠️ Lesson learned (read before acting)
The build-out is **done** (see Current State). Recent work collapsed into
**whack-a-mole**: ~17 of 25 commits were incremental `mb_shot_smoke` / `mb_smoke`
test-padding and micro-flags — manufactured busywork because the core product was
already complete. **Stop that pattern.**

**The rule now:** each tick must move a *substantive* milestone (below) forward, or
fix a *real* bug hit in actual use. **Do NOT** add another test case, micro-flag, or
"characterize + lock in" probe just to have something to commit. If no substantive
step is available and nothing is broken, **say so and stop** — do not invent coverage.

---

## Active refactor — split the mb_smoke monolith
`test/mb_smoke.cc` is a 3660-line / 180-case monolith. Splitting it into small themed
smoke programs (engine / scrape / input / net / platform), each its own executable.
- [DONE] `test/mb_smoke_harness.h` — shared header-only helpers (`Eval`/`EvalIso`/
  `Expect`/counters) + `MB_SMOKE_MAIN(SUITE)`.
- [DONE] `mb_smoke_platform.cc` (cases 87–106, 43) and `mb_smoke_render.cc` (cases 35–86b
  modern CSS / web components / platform crash-safety / blob / paint / fonts / SVG /
  selector automation / navigation, 77). All three in BUILD.gn + build.sh.
  Now: `mb_smoke` 60 + `mb_smoke_render` 77 + `mb_smoke_platform` 43 = **180**. mb_smoke.cc
  is 3660 -> 1194 lines (engine basics 1–34 + the `MB_NET_TESTS` block + case 107 binding).
- [OPTIONAL] Could peel the `MB_NET_TESTS` block into `mb_smoke_net.cc` to leave mb_smoke.cc
  as pure engine basics — but the 3-way split already addresses the monolith; lower
  priority than the #1 net-interception feature work.

## Current State (complete)
- **Engine:** modern M150 Blink renders HTML→pixels in-process. V8/JS, modern + cutting-edge
  CSS (`:has()`, nesting, `@container`, `oklch()`), canvas 2D, SVG, Web Components/Shadow DOM,
  fetch/XHR (method/body/headers/status/cookies/redirects/charset incl. CJK), blob: URLs,
  Web Crypto, Intersection/Resize/Mutation observers, WAAPI, forms + submit-nav, mouse/keyboard
  input, host-side history, iframes (DOM + paint), CSP-strict pages. Multi-font + i18n render.
- **`mb_capi` C ABI — 108 functions.** lifecycle / load / JS eval / selector scraping /
  input / screenshots (PNG·JPEG·PDF, file + in-memory) / cookies (+ jar) / network config /
  emulation (UA·headers·locale·tz·dark·DPR·transparent·images) / host-side history.
- **`wke` compat layer — 127 functions** (full jsValue object model + async callbacks). Done.
- **`mb_shot` CLI — 59 flags** (request-config → interact → synchronize → prepare-view →
  extract → capture → assert), exit codes 0/1/2/3.
- **Tests (all green, no leaked procs):** `mb_smoke` 179, `wke_smoke` 100, `mb_shot_smoke.sh`
  62 offline (+5 `MB_NET_TESTS=1` = 67). This coverage is **sufficient** — do not pad it.

## Forward plan — grounded in the gap audit (priority order)
The headless render/scrape product is complete; these are the genuine, named gaps that
make this a *real* miniblink. Prioritized by value × tractability × verifiability, and
explicitly avoiding the traps (GPU pipeline; mojo `LocalFrameHost` binding crashes —
a null-remote `WebPolicyContainer` already CHECK-failed). Work top-down; one at a time.

**START HERE — high value, tractable, in code we own, headlessly verifiable:**

1. **Network request/response interception & mocking** — THE signature miniblink feature
   and #1 in the audit. In our libcurl loader (`loader/mb_url_loader.cc`), no mojo.
   - [DONE] **Response mocking** — `mbMockResponse(url_substr, body, content_type, status)`
     + `mbClearMocks()`: a matching URL serves a canned body with NO real fetch (offline
     runs / API substitution). Checked first in `Deliver`. mb_smoke case 75d (mock a
     stylesheet → element turns green; clear/re-mock → red), all offline.
   - [DONE] **URL rewriting** — `mbRewriteUrl(from, to)` + `mbClearUrlRewrites()`:
     transparently redirect a request before fetch (host swap / scheme upgrade / CDN ->
     local mock); the page still sees its original URL as the response URL (Deliver
     fetches `fetch_url`, reports `url`). mb_smoke_render case 75e (rewrite orig.test ->
     mock.test, mocked green; clear -> served by orig's own mock), offline.
   - [DONE] **`mb_shot --mock URL FILE` / `--rewrite FROM TO`** — interception on the
     deliverable CLI: `--mock` serves FILE (content-type by extension) for matching
     requests with no fetch; `--rewrite` redirects before fetch. Verified through the
     fetch() path (mb_shot_smoke: a page fetch()es an API URL served from a local file
     -> GOT:42; a rewrite onto the mock -> GOT:42) — confirms the transparent rewrite
     holds against fetch()'s url_list_ DCHECK.
   - [NEXT] request HEADER rewrite (add/override before fetch); a dynamic per-request
     callback (`wkeNetHookRequest`/`wkeNetOnResponse`) for live inspect/modify; wke peers.

2. **Quick-win correctness bugs** (real defects; fast; each independently verifiable —
   NOTE several have *existing tests that assert the stubbed/fake behavior* and must be
   updated):
   - `wkeIsLoading` hardcoded `false`, `wkeIsLoadingCompleted`/`wkeIsDocumentReady`
     hardcoded `true` (`wke.cc:347–359`) — wire to the real load state.
   - [DONE] `wkePostURL` ignored `postLen` → truncated binary bodies at an embedded NUL.
     Fixed by threading the byte length end-to-end: new `mbPostURLData(url, body, len, ct)`
     (mbPostURL kept as the NUL-terminated text convenience), `MbWebView::PostURL` builds
     the body from (ptr, len), and `wkePostURL` passes `postLen`. The libcurl loader already
     used `POSTFIELDSIZE`/`COPYPOSTFIELDS(.size())`, so binary bodies now post whole.
     Verified (wke_smoke +1 net): POST "AB\0CD" (5 bytes) → httpbin echoes Content-Length 5
     (pre-fix would truncate to "AB" = 2).
   - `wkeFireKeyUpEvent` is a no-op — implement it.
   - `mbShutdown` leaks on repeated init (`runtime/mb_runtime.cc:203`).
   - Delete the dead commented-out `widget/mb_sw_frame_sink.{h,cc}` scaffolding.

**Tier 1 — genuine high-value automation capability (after the above):**
3. **Iframe / sub-frame targeting** (audit #2).
   - [DONE] **per-frame eval** — `mbGetFrameCount()` + `mbEvalJSInFrame(frame_index, js)`:
     evals host-privileged in the Nth child frame's own world, so it reads even a
     CROSS-ORIGIN iframe the parent's `iframe.contentDocument` can't (single-process =
     all iframes are local `WebLocalFrame`s). mb_smoke_render 78b: data: iframe under an
     https parent -> parent read NULLDOC, `mbEvalJSInFrame(0,...)` reads XFRAME-SECRET.
   - [DONE] **`mb_shot --frame N`** — routes `--eval`/`--eval-json` into child frame N
     (host-privileged), so the CLI scrapes iframe content the page can't. mb_shot_smoke:
     parent body=`parent`, `--frame 0 --eval document.body.textContent`=`CHILD-77`.
   - [NEXT] per-frame selector ops (click/fill/text-by-selector in a frame) via the same
     child-frame mechanism + a wke `wkeRunJsByFrame` peer.
4. **Push callback model** — replace poll-only readiness: host `DidFinishLoad`/
   `DidMeaningfulLayout` signals (`mb_frame_client.h:122` TODO — fixes partial-capture
   races), navigation policy (`wkeOnNavigation`), new-window (`wkeOnCreateView`), downloads.
5. **JS dialogs** — alert/confirm/prompt are unhandled (`wke.h:462` even warns about it).
   Note the reverted `mbSetJsDialogPolicy` FATAL'd in `thread_collision_warner` — solve the
   `ScopedPagePauser` threading first.
6. **File download (to disk) + file upload (`input[type=file]`)** — both absent; downloads
   currently commit as documents; `mbFillSelector` can't set a file input.

**Tier 2 — web-platform fidelity (host infra; heavier):**
7. Workers (dedicated worker thread+isolate; shared/service absent). 8. Broker binds cookies
only — IndexedDB / WebSocket / Permissions / geolocation / clipboard / notifications dropped.
9. Storage/cookie persistence across runs + async CookieStore + change events. 10. Blob-from-file
+ ranged blob reads + DataPipeGetter uploads. 11. **GPU content path** (WebGL / accel-2d-canvas /
`<video>` render blank) — the heaviest; needs a GL/media provider. Last.

**Tier 3 — input & rendering refinements:**
12. Input fidelity (modifier flags, right/middle-click, IME, native drag-drop, trusted touch/wheel).
13. PDF options (page size / landscape / margins / scale / print-background — currently hardcoded
US-Letter). 14. Child-frame charset hardcoded UTF-8 → mojibake on non-UTF-8 iframes
(`mb_frame_client.cc:182`). 15. CSP/PolicyContainer leaks across navigations in a reused view.

### Accepted, by-design (NOT tasks):
- `<meta name=viewport>` ignored (mobile = narrow view + UA, works). Color emoji monochrome
  (no color-emoji font bundled).

---

## Operational facts
- **Project:** `/Users/yangxin/dennis/chrome/miniblink-modern/`, include-root `src/`
  (`src/miniblink_host/` = host + `mb_capi`; `src/wke/` = wke layer; `patches/` = donor patches).
- **Donor tree (already builds):** `/Users/yangxin/dennis/chrome/chromium-150.0.7871.24`,
  `out/Release` gen'd, `is_component_build=true`, macOS SDK, gn at `buildtools/mac/gn`.
- **wke parity reference:** `/Users/yangxin/dennis/chrome/miniblink49` (`wke/wke.h`).
- **Build & test** (sources are STAGED into the donor tree, then ninja):
  ```
  TREE=/Users/yangxin/dennis/chrome/chromium-150.0.7871.24
  rm -rf $TREE/third_party/blink/renderer/miniblink_host $TREE/third_party/blink/renderer/wke
  cp -R src/miniblink_host $TREE/third_party/blink/renderer/miniblink_host
  cp -R src/wke            $TREE/third_party/blink/renderer/wke
  ninja -C $TREE/out/Release mb_smoke mb_shot wke_smoke
  ```
  `build.sh <TREE>` does staging + patches + build + the smoke suites.
- **Donor patches (`patches/`):** 0001 offscreen-widget-compat, 0002 suppress-js-dialogs,
  0003 enable-blob-Register, 0004 blob-url-loader-bypass.
- **Discipline:** run binaries bounded (background + watchdog SIGKILL) then `pgrep -x` survivor-
  check — never leak processes. Verify everything empirically; revert anything that doesn't
  verify (end clean). Network features verified against public hosts (example.com / httpbin /
  badssl, `dangerouslyDisableSandbox`), never localhost (times out).
- **Commits:** author `Xin Yang <yangxin0@outlook.com>`, ~72-col body explaining WHY, **NO
  AI / Co-Authored-By trailer**:
  `git -c user.name="Xin Yang" -c user.email="yangxin0@outlook.com" commit --no-verify`
  Commit per-milestone, only at a clean, tested state.
