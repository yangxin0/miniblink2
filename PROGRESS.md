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
   - [DONE] **dynamic per-request callback** — `mbSetRequestCallback(cb, userdata)`: a
     process-wide hook consulted in the loader's `Deliver` for EVERY request URL (next to
     the static block/mock/rewrite tables); returns nonzero to BLOCK, zero to allow, so an
     embedder inspects + decides at runtime instead of pre-registering substrings. Loader
     side `MbSetRequestHook`/`MbRequestHookBlocks` (main-thread, inside the load). Verified
     (mb_smoke +1=63, offline): two same-origin fetch()es — the hook records both (seen=2),
     allows the mocked one (ok:7), vetoes the "blockme" one (blocked).
   - [DONE] **response-side callback** — `mbSetResponseCallback(cb, userdata)`: a
     process-wide hook invoked in `Deliver` after a successful load (fetch/mock/file/data)
     with an opaque `mbResponse` handle, BEFORE the body reaches the page. Accessors
     `mbResponseURL/Status/Body` (inspect) + `mbResponseSetBody` (replace — inject a
     script, strip content, rewrite a payload; the new length is delivered). Loader side
     `MbSetResponseHook`/`MbInvokeResponseHook` (std::function so the capi binds a lambda).
     Verified (mb_smoke +1=64, offline): a mock serves {"v":1}; the hook records it and
     rewrites to {"v":99}; the page's fetch() observes v=99.
   - [DONE] **per-URL request header injection** — `mbSetRequestHeader(url_substring, name,
     value)` + `mbClearRequestHeaders()`: add an outgoing http(s) header for requests whose
     URL contains the substring (e.g. an Authorization/API key sent ONLY to its host, not
     leaked to every origin; or a per-domain UA) — conditional on the URL, unlike global
     extra-headers. Applied in `FetchHttp` (the shared http chokepoint), so it covers BOTH
     the top-level navigation (MbFetchUrl) and subresources/fetch (Deliver). Verified
     (mb_smoke 32b, MB_NET_TESTS vs httpbin /headers): the header registered for "/headers"
     is echoed; one for a non-matching host is not.
   - [DONE] **wke peers** — `wkeNetOnRequest(cb)` (return true to BLOCK) + `wkeNetOnResponse(cb)`
     (inspect URL+body) forward to the host request/response hooks. Port-pragmatic (process-wide
     under the hood, last registration wins; simpler than miniblink49's job-based wkeOnLoadUrlBegin).
     Verified (wke_smoke +1=103, offline): a file:// CSS subresource — the request cb logs its URL,
     the response cb gets its bytes, the CSS applies; blocking the CSS via the request cb stops it.

   **→ Network interception (#1) is now comprehensive: static block/mock/URL-rewrite +
   per-URL header inject + dynamic request hook + response hook + CLI (--mock/--rewrite).**

2. **Quick-win correctness bugs** (real defects; fast; each independently verifiable —
   NOTE several have *existing tests that assert the stubbed/fake behavior* and must be
   updated):
   - [DONE] `wkeIsLoadingCompleted`/`wkeIsDocumentReady` were hardcoded `true` → wired to
     real state. `wkeIsLoadingCompleted` returns a new `did_load` flag (set on every load
     path via FireLoadCallbacks) — false on a fresh view, distinguishing "never navigated"
     from "loaded". `wkeIsDocumentReady` evals the real `document.readyState`
     (interactive|complete) — false if no frame / still parsing. `wkeIsLoading` stays
     `false` (correct: the synchronous load model never exposes an in-flight window;
     comment clarified). Verified (wke_smoke +1=102): fresh view → not completed; post-load
     → completed + ready.
   - [DONE] `wkePostURL` ignored `postLen` → truncated binary bodies at an embedded NUL.
     Fixed by threading the byte length end-to-end: new `mbPostURLData(url, body, len, ct)`
     (mbPostURL kept as the NUL-terminated text convenience), `MbWebView::PostURL` builds
     the body from (ptr, len), and `wkePostURL` passes `postLen`. The libcurl loader already
     used `POSTFIELDSIZE`/`COPYPOSTFIELDS(.size())`, so binary bodies now post whole.
     Verified (wke_smoke +1 net): POST "AB\0CD" (5 bytes) → httpbin echoes Content-Length 5
     (pre-fix would truncate to "AB" = 2).
   - [DONE] `wkeFireKeyUpEvent` was a no-op → now dispatches a real key RELEASE so page
     `keyup` handlers fire. New `MbWidget::SendKeyUp(vk)` emits a single kKeyUp
     WebKeyboardEvent (dom_key from the shared `kKeys` table for named keys, else the
     unshifted ASCII char; keyCode from the VK), exposed as `mbSendKeyUp(view, vk)`;
     `wkeFireKeyUpEvent` calls it. Press events untouched (no decouple → no regression
     to the Enter-submit path). Verified (wke_smoke +1): VK_RIGHT keyup → page sees
     keyCode 39. (Refactored `kKeys` to file scope; SendKey behavior unchanged, 182 green.)
   - [DONE] `mbShutdown` re-init leak/crash. It did `delete g_runtime`, leaving the
     installed `DiscardableMemoryAllocator`/blink `Platform` pointers dangling and
     making a 2nd `mbInitialize` re-run the one-time globals (mojo::core::Init,
     blink::Initialize + isolate) → crash. Fixed to match Chromium's process model: the
     engine is one-time and stays resident; `mbShutdown` is a safe no-op and
     `mbInitialize` is idempotent (reuses it). Verified (mb_smoke 0 +1=61): shutdown ->
     re-init succeeds AND all 60 subsequent tests run against the post-cycle engine.
   - [DONE] Deleted the dead `widget/mb_sw_frame_sink.{h,cc}` scaffolding (an empty
     `MbSoftwareFrameSink` stub never referenced — the real readback is paint-record
     playback in mb_widget.cc) + its BUILD.gn entries. Builds clean, mb_smoke 61.

   **→ Quick-win cluster (item 2) COMPLETE — all five defects fixed & verified.**

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
4. **Push callback model** — replace poll-only readiness with real engine signals.
   - [DONE] **live console push** — `mbOnConsoleMessage(view, cb, userdata)`: cb fires for each
     page console message (console.log/warn/error) with level + text as it happens (vs polling
     mbDrainConsole) — the mb_capi peer of wke's wkeOnConsole. Verified (mb_smoke 0k): a page's
     console.log('hi')+console.error('boom') → callback logs "log:hi;error:boom;".
   - [DONE] **load-finished push** — `MbFrameClient::DidFinishLoad()` (main frame) →
     `MbWebView::OnDidFinishLoad()` sets a `load_finished_` flag (reset in CommitHtml on
     every navigation) + invokes a registered callback. C ABI: `mbOnLoadFinish(view, cb,
     userdata)` (the first push callback in mb_capi — fires during the synchronous load
     pump on the genuine `load` event, not a poll/timer) + `mbIsLoadFinished(view)`.
     Verified (mb_smoke +1=62): a counting callback fires once per load (fin=2 over 2
     loads), flag set. This is the signal that lets a caller wait on real completion
     rather than a fixed settle — addresses the partial-capture race.
   - [DONE] **navigation policy** — `mbOnNavigation(view, cb, userdata)`: the callback fires
     for each PAGE-initiated main-frame navigation (link/location=/form/JS-redirect) with the
     target URL, BEFORE commit, in `MbFrameClient::BeginNavigation`; returns 1=allow / 0=block
     (stop popups/redirects/leaving the page) — host-driven LoadURL bypasses it. Also made
     `DoCommit` honor mocks so a page navigation is served from the interception layer too.
     Verified (mb_smoke 0g, offline): a callback lets nav.test/ok commit (mock → GOOD) and
     vetoes nav.test/blocked (stays GOOD); the log shows both URLs. [DONE: wke peer]
     `wkeOnNavigation` routes to it (verified wke_smoke +1=105: cancels a location.href nav).
   - [DONE] **URL-changed notification** — `mbOnUrlChanged(view, cb)` fires on EVERY main-frame
     commit (host load / page nav / redirect / reload) with the new URL, from
     `OnDidCommitMainFrame`; + wke peer `wkeOnURLChanged`. Verified (mb_smoke 0l=79, wke_smoke
     +1=106): two loads → callback logs both URLs.
   - [DONE] **new-window notification** — `mbOnNewWindow(view, cb, userdata)`: fires when the
     page calls `window.open()` / activates `target=_blank`, with the requested URL + window
     name, via `MbFrameClient::CreateNewWindow` (which still returns null = popup denied, the
     safe default). The embedder can react (e.g. load the URL in this/a new view). Verified
     (mb_smoke 0h, offline): `window.open('https://popup.test/p','winname')` → callback logs
     `popup.test/p|winname` and window.open returns null.
   - [DONE] **failed-load finish** — a top-level load that never commits (file:// read
     failure or http(s) fetch failure) now still signals completion: `MbWebView::LoadURL`
     calls `NotifyLoadFailed()` on both no-commit paths (sets `load_finished_=true`, fires
     `on_load_finish_`), so a caller awaiting completion isn't stuck on a 404/missing file.
     Verified (mb_smoke 0n=81): load a real page, register the finish cb, load a missing
     file:// URL → callback fires + `mbIsLoadFinished` reads true.

   **→ Tier-1 push-callback model (#4) is now functionally complete: load-finish push,
   navigation policy, AND new-window notification.**
5. **JS dialogs** — [DONE]. `mbSetJsDialogCallback(view, cb, userdata)`: the callback is
   invoked per dialog (type 0/1/2 = alert/confirm/prompt) with the message + prompt default;
   returns accept(1)/dismiss(0) and writes prompt text. No callback → headless-safe defaults
   (confirm=false, prompt=null). **Sidesteps the earlier landmine entirely**: instead of
   re-entering ChromeClient's `[Sync]` RunModal*Dialog (which deadlocks / the reverted
   `mbSetJsDialogPolicy` FATAL'd in `thread_collision_warner` via `ScopedPagePauser`), it
   installs a pure JS-level override of window.alert/confirm/prompt before page scripts
   (in RunDocumentStartScript) that routes to an internal `__mbDlg` native binding → the
   view's handler — synchronous, main-thread, no browser/modal/mojo. Verified (mb_smoke 0f):
   callback captures "0:hi;1:go?;2:name?;", confirm→true, prompt→"REPLY"; no-callback →
   confirm=false, prompt=null. [DONE: wke peers] `wkeOnAlertBox`/`wkeOnConfirmBox`/
   `wkeOnPromptBox` (+ `wkeSetString` for the prompt out-param) route through the host dialog
   handler — the miniblink49 dialog API. Verified (wke_smoke +1=104): alert captured, confirm
   accepted, prompt returns "REPLY".
6. **File upload + download** — [DONE] (both).
   - [DONE] **`mbDownloadURL(url, dest_path)`**: fetch a URL through the engine network stack
     (MbFetchUrl) and write the body to disk WITHOUT rendering it as a document. Honors the
     full interception layer (rewrite / block / mock / request+response hooks) and, for
     http(s), the view's UA + extra/per-URL headers + cookies + proxy. Works for http(s),
     file:// and data:. Verified (mb_smoke 0e, offline): a data: URL decodes to the file
     (DL-DATA-7); a mocked URL downloads with NO network and the response hook rewrites the
     bytes (REWRITTEN).
   - [DONE] **download diversion** (browser-style "a navigation becomes a download"):
     `mbOnDownload(view, cb)` — a top-level mbLoadURL to a response that is a DOWNLOAD
     (Content-Disposition: attachment, or a non-renderable MIME) is handed to the callback
     (url, mime, suggested filename, body bytes) INSTEAD of rendered, ONLY if a callback is
     set (else rendered as before — no regression). Also added data: URLs to mbLoadURL's
     fetch path. Verified (mb_smoke 0m): mbLoadURL("data:application/octet-stream,DLBYTES")
     → callback gets mime+bytes, the prior page stays (not committed). [DONE: wke peer]
     `wkeOnDownload` (URL-only per the miniblink signature; bytes via mbOnDownload) routes
     through it — verified (wke_smoke +1=107).
   - [DONE] **`mbSetFileForSelector(css_selector, paths_newline)`**: the privileged host op a
     page's own script is forbidden to do. Reaches the core `HTMLInputElement`, reads each
     path's bytes into an **in-memory `BlobData` registered with our BlobRegistry** (via
     `BlobDataHandle::Create(BlobData, size)` — the SAME path `new Blob([bytes])` takes, so
     the bytes are genuinely readable), builds a `FileList` of `File(name, time, handle)`,
     `setFiles()` (which stores the list verbatim — no re-wrap), then fires `change`. Verified
     (mb_smoke_render 62-file): set a 17-byte file → `files[0].name`=mb_upload.txt, `.size`=17,
     **FileReader reads the real bytes** "UPLOAD-CONTENT-42" (the form-submit byte path),
     change fires once; a text input and a non-match return 0.
     NOTE: the earlier dead-end was a manual MbBlob+PostTask handle (size 0 / read hung); the
     `BlobData`+registry path is what actually works. `SetFilesFromPaths` (path-backed) does
     NOT work here — no file-reading blob backend — which is why we read the bytes ourselves.

**Tier 2 — web-platform fidelity (host infra; heavier):**
7. Workers (dedicated/shared/service). **RE-SCOPED + STEP 1 STARTED (2026-06):** the earlier
   "big-bang, maybe-infeasible" framing was over-pessimistic. A re-investigation against M150
   source shows an in-process dedicated worker is TRACTABLE and built entirely from blink-PUBLIC
   types (no content/renderer infrastructure required). Key findings:
   - `blink::mojom::DedicatedWorkerHost` is an **EMPTY interface** (zero methods) — the host
     "remote" is a trivial self-owned receiver, not a real mojom stub.
   - `WorkerMainScriptLoadParameters` + the loader-bundle types live in `third_party/blink/public/`
     (host-reachable). The content `DedicatedWorkerHostFactoryClient` is only ~177 lines and its
     content-only deps (ServiceWorkerProviderContext, RenderThreadImpl cors list) are optional/empty.
   - `WebWorkerFetchContext` is **10 pure-virtuals** (not ~21); the concrete giant is
     `WebDedicatedOrSharedWorkerGlobalScopeContext`, which we DON'T need — a minimal context suffices.
   - Script delivery model (`WorkerMainScriptLoader::Start`): blink BINDS the
     `url_loader_client_endpoints` (it is the URLLoaderClient) and reads `response_body` (a data
     pipe) to EOF, then awaits our `OnComplete`. So we synthesize: a 200 `URLResponseHead`, a data
     pipe holding the script bytes (producer closed = EOF), a held inert URLLoader receiver, and a
     URLLoaderClient remote we use to push `OnComplete(net::OK)`.

   **Plan (each step compiles + keeps the render worker-spawn guard green until the last):**
   - [DONE] **Step 1 — subresource fetch context.** `src/miniblink_host/worker/mb_worker_fetch_context.{h,cc}`:
     `MbWorkerFetchContext : WebWorkerFetchContext` (10 methods; `GetURLLoaderFactory()` hands back
     `MbWorkerURLLoaderFactory : blink::URLLoaderFactory`, which creates `mb::MbURLLoader`). Wired via
     `MbFrameClient::CreateWorkerFetchContext`. CRASH-SAFE: the worker thread is still not started
     (CreateWorkerHost stays inert), so `new Worker()` still degrades gracefully (render 81 green).
     KNOWN follow-up: MbURLLoader's body-loader task runner is the creation-thread runner; once the
     worker actually runs, `GetURLLoaderFactory()` is consulted ON the worker thread and the loader
     must use the worker-thread runner. Harmless until Step 2 (factory created but never invoked).
   - [DONE] **Step 2 — drive the worker thread.** `src/miniblink_host/worker/mb_dedicated_worker_host.{h,cc}`
     (`MakeDedicatedWorkerHostFactoryClient`, wired from `MbPlatform::CreateDedicatedWorkerHostFactory-
     Client`). On `CreateWorkerHost`: (a) `worker_->OnWorkerHostCreated(MakeFrameInterfaceBroker(),
     empty-DedicatedWorkerHost self-owned receiver, WebSecurityOrigin::Create(script_url))`; (b) fetch
     the script via `MbFetchUrl` (file/http(s)/data:), synthesize `WorkerMainScriptLoadParameters` —
     200 `URLResponseHead`, script bytes over a data pipe, a `URLLoaderClientEndpoints` whose inert
     `network::mojom::URLLoader` we hold and whose `URLLoaderClient` remote we drive; (c) `OnScript-
     LoadStarted(params, bfcache-host self-owned receiver, NullReceiver, NullReceiver)`. The script
     delivery object (`MbWorkerScript`) writes the body via `mojo::DataPipeProducer`, drops the
     producer (EOF), pushes `OnComplete(net::OK)`, and self-deletes when blink drops the loader remote.
     The risk (worker-thread isolate/cppgc CHECKs) did NOT materialize — the thread starts cleanly.
     VERIFIED (mb_smoke_render 37b=82, stable across repeated runs, no leaked threads): a Worker built
     from a `data:` script runs `onmessage=e=>postMessage(e.data*2)`, the page posts 21 and receives
     42 — a full two-way `postMessage` round-trip. **Dedicated workers now actually RUN in-process.**
   - [DONE] **Step 3 — exercise/​harden (classic workers).** Fixed a real bug surfaced here:
     `MbWorkerFetchContext::TopFrameOrigin()` returned `nullopt`, but `WorkerFetchContext::GetTop-
     FrameOrigin` DCHECKs that only shared/service workers may have a null top-frame origin — so a
     dedicated worker's first subresource load FATAL-crashed. Now the fetch context carries the
     creating document's serialized origin (`MbFrameClient::CreateWorkerFetchContext` reads
     `web_frame_->GetSecurityOrigin()`), rebuilt per call on the worker thread. The Step-1 loader's
     `GetTaskRunnerForBodyLoader()` returns `GetCurrentDefault()`, which resolves to the WORKER
     thread's runner at load time — so no task-runner fix was needed. Verified (mb_smoke_render
     85, stable): 37c three concurrent workers each deliver their own reply (sum=12); 37e
     `importScripts("data:…self.K=7")` loads on the worker thread and the worker replies e.data+K
     — end-to-end proof the worker fetch context works.
   - [DONE] **Module dedicated workers** (`new Worker(url,{type:'module'})`) now RUN their top-level
     script. Root cause: `WorkerModuleScriptFetcher::OnStartLoadingBodyWorkerMainScript` enforces a
     JavaScript MIME via `ResourceResponse::HttpContentType()` — which reads the Content-Type HEADER,
     not the response's `mime_type` field. The synthesized script response only set `mime_type`, so
     modules were rejected (classic workers don't MIME-check, so they were unaffected). Fix: the
     `WorkerMainScriptLoadParameters` response head now includes a `Content-Type: <mime>` header
     (mb_dedicated_worker_host.cc). Verified (mb_smoke_render 37d=85): a `{type:'module'}` worker
     runs `self.onmessage=e=>self.postMessage(e.data+100)`; 5 -> 105 round-trip.
   - [DONE] **Nested workers** (a Worker spawning a sub-Worker). The sub-worker is created ON the
     parent worker's thread, so its fetch context comes from `CloneWorkerFetchContext` (not the
     frame). That returned null → the sub-worker's script load FATAL'd at
     `worker_main_script_loader.cc:45` (null resource-load observer). Fix:
     `MbWorkerHostFactoryClient::CloneWorkerFetchContext` now clones the parent (a new
     `MbWorkerFetchContext` with the same UA/headers/origin via `MbWorkerFetchContext::CloneContext`).
     Verified (mb_smoke_render 37f=86): an outer worker relays 10 to an inner worker that doubles to
     20; the page receives "inner:20".
   - [DONE] **SharedWorker** runs in-process. `worker/mb_shared_worker.{h,cc}`: `MbSharedWorker-
     Connector` (bound via the FRAME broker, mb_frame_broker.cc) implements `SharedWorkerConnector::
     Connect` — binds the page's `SharedWorkerClient` remote (`OnCreated`/`OnConnected`), synthesizes
     the script (shared `MakeWorkerMainScriptParams`), drives `WebSharedWorker::CreateAndStart`, then
     `worker->Connect(0, message_port)` delivers the page's MessagePort to the worker's `onconnect`.
     THREADING: the frame broker runs on the SERVICE thread but `CreateAndStart` is main-thread only,
     so the broker now captures the main runner (in `MakeFrameInterfaceBroker`) and PostTasks the
     connector receiver to the main thread (`BindSharedWorkerConnector`). Two crashes fixed during
     bring-up: (1) needed a bound `PolicyContainerHost` associated remote — `SharedWorkerGlobalScope::
     Initialize`→`UpdateReferrerPolicy` CHECKs an unbound remote (the connection owns an
     `MbSwPolicyContainerHost`); (2) the WorkerContentSettingsProxy `[Sync]` stub answers all-allow.
     Verified (mb_smoke_render 37g=87, stable): a SharedWorker from a non-opaque origin echoes
     through its connect MessagePort (page posts 7 → 14). The script-delivery helper was first
     extracted to `worker/mb_worker_script.{h,cc}` (shared with dedicated).
   - [DONE] **SharedWorker sharing** — a process-wide registry (keyed by url|name|type) makes
     repeated `new SharedWorker(sameUrl)` attach to ONE running worker (the defining behavior of
     the API). `MbSharedWorkerInstance` owns the worker + is its `WebSharedWorkerClient`; each
     Connect either starts a new instance or `AddClient`s to the existing one (a fresh connect event
     + MessagePort to the same `onconnect`); it self-deregisters on `WorkerContextDestroyed`. Verified
     (mb_smoke_render 37h=88): two handles to one url share a worker-global counter (replies 1 then 2).
     Not yet: module shared workers (untested), worker eviction when the last client disconnects.
   - [SUPERSEDED] (prior SharedWorker scoping) `new SharedWorker(url)` →
     `SharedWorkerClientHolder::Connect` → `mojom::SharedWorkerConnector.Connect` requested from the
     FRAME broker (mb_frame_broker.cc — bind it there). Implement `SharedWorkerConnector::Connect(info,
     client_remote, ctx_type, message_port, blob_token)`: bind `client_remote` (`mojom::SharedWorker-
     Client` — call `OnCreated(ctx_type)` then `OnConnected({})`); fetch `info.url`, synthesize the
     same `WorkerMainScriptLoadParameters` (reuse `MbWorkerScript`); `WebSharedWorker::CreateAndStart(
     ~25 params: token, url, script_type, name, origins, UA, default UserAgentMetadata, empty CSPs,
     WebFetchClientSettingsObject, MakeFrameInterfaceBroker(), a WorkerContentSettingsProxy remote
     [HAS [Sync] AllowIndexedDB/CacheStorage/WebLocks → bind on the SERVICE thread like cookies/blob],
     synth load params, WebPolicyContainer, MbWorkerFetchContext, SharedWorkerHost self-owned receiver
     [no-op], WebSharedWorkerClient impl [1 method: WorkerContextDestroyed], NullReceiver coep/dip)`;
     then `worker->Connect(request_id, message_port)` to deliver the port to `onconnect`. RISK: the
     [Sync] content-settings proxy + the WebPolicyContainer/WebFetchClientSettingsObject construction
     are the fiddly bits. Heavier than dedicated (more params, sharing-by-url state) — multi-tick.
   - [NEXT] http(s) worker scripts (flow through `MbFetchUrl`, untested offline); ServiceWorker (heavy).
   8. Broker binds cookies
only [+ Permissions, this tick]. [DONE: Permissions] `MbPermissionService` in the FRAME
broker (mb_frame_broker.cc — the one navigator.* uses, not the platform thread broker)
answers navigator.permissions.query/.request as DENIED, so the promise resolves instead
of HANGING (it was dropped → a permission-gated page stalled forever). Verified (mb_smoke
23c): query({name:'geolocation'}) → state "denied". [DONE: geolocation] `mbSetGeolocation(lat, lng, accuracy)` / `mbClearGeolocation()` + an
in-process `MbGeolocationService`/`MbGeolocation` in the frame broker: once a fix is set,
`navigator.geolocation.getCurrentPosition` resolves to it (the service GRANTS); unset = the
default PERMISSION_DENIED. Thread-safe (base::Lock; broker on the service thread, API on
main). Verified (mb_smoke 23d): default → err code 1; after mbSetGeolocation(37.42,-122.08,5)
→ coords 37.42,-122.08@5. [DONE: clipboard] `MbClipboardHost` (all 24 ClipboardHost methods; plain-text store, other
formats empty) + PermissionService now GRANTS clipboard-read/write → `navigator.clipboard`
writeText/readText work (secure origin + document.hasFocus()==true, which the host reports).
Host shares the store via `mbSetClipboard(text)` / `mbGetClipboard(out)`. Verified (mb_smoke
23e): page writeText 'copied-from-page' → mbGetClipboard reads it; mbSetClipboard 'set-by-
host' → page readText returns it. [DONE: Web Locks] `frame/mb_lock_manager.{h,cc}` (`MbLock-
Manager`, bound from the frame broker) implements `navigator.locks` with REAL serialization:
`RequestLock` grants exclusive/shared with per-name conflict checks, queues WAIT requests,
fails NO_WAIT (`{ifAvailable:true}`) requests, and PREEMPT-steals; a held lock releases when
its `LockHandle` pipe closes (posted, never deleting the receiver inside its own disconnect
handler), then the queue is reprocessed. The `LockHandle` sent to `Granted` is left an
UNassociated pending endpoint so it associates with the `LockRequest` pipe (a dedicated one
DCHECKs). Verified (mb_smoke 23f/23g): two exclusive requests on one name serialize ("AaB" —
2nd waits for 1st's async release), and `{ifAvailable:true}` on a held lock yields null.
[DONE: BroadcastChannel] `frame/mb_broadcast_channel.{h,cc}`. A WINDOW's BroadcastChannel does
NOT use the broker — it requests an ASSOCIATED `BroadcastChannelProvider` from the frame's
navigation-associated interface provider. The host already serves that provider on the SERVICE
thread (`MbNavAssociatedInterfaceProvider` in mb_blob_registry.cc, for blob URLs); a new branch
there binds an in-process `MbBroadcastChannelProvider`. `ConnectToChannel` registers each channel
in a process-wide name→channels map; a page's `postMessage` (the channel's `connection` receiver)
fans out to every OTHER same-name channel's `client` remote (sender excluded). The blink-variant
`OnMessage` carries a move-only `BlinkCloneableMessage`, shallow-cloned field-wise per recipient
(the SerializedScriptValue is immutable+refcounted). Verified (mb_smoke 23h): two channels named
'ch' in one window — `a.postMessage('ping')` → `b` receives 'ping', `a` does NOT. WORKER channels
are also wired: a worker's BroadcastChannel asks its broker (the frame broker, service thread),
where `BindBroadcastChannelProviderPipe` binds the SAME provider into the SAME registry — so
window and worker channels of one name interoperate. Verified (mb_smoke 23i): a dedicated worker
posts on 'xch' and the window's same-name channel receives "from-worker" (cross-thread delivery).
[DONE: Notifications] `frame/mb_notification_service.{h,cc}` (`MbNotificationService`, bound from
the frame broker). `[Sync] GetPermissionStatus` returns GRANTED (so `Notification.permission`
== "granted" without hanging — it's a sync getter); `DisplayNonPersistentNotification` fires the
listener's `OnShow()` so a page's `Notification.onshow` runs (headless: no OS toast, but the API
is live + scriptable), keeping the listener alive by token for a later `OnClose`. Persistent (SW)
notifications + GetNotifications are accepted-but-empty stubs. The permission service also grants
NOTIFICATIONS so `Notification.requestPermission()` resolves "granted". Verified (mb_smoke 23j):
`Notification.permission`=="granted", `new Notification('hi')` fires onshow, requestPermission()
-> "granted". [DONE: WebSocket — loopback echo] `frame/mb_websocket.{h,cc}` (`MbWebSocketConnector`/
`MbWebSocket`, bound from the frame broker). The connector establishes the connection
(`OnConnectionEstablished` with a fully-populated `WebSocketHandshakeResponse` — all
non-nullable fields incl. `http_version`/`remote_endpoint` must be set or mojo FATALs) so
`onopen` fires (readyState OPEN). The full data plane is wired: `MbWebSocket` reads the page's
outgoing messages off the WRITABLE pipe (framed by each `SendMessage(type,len)` announcement)
and echoes them straight back via `OnDataFrame` + the READABLE pipe (two `SimpleWatcher`s,
backpressure-honest), and `StartClosingHandshake` -> `OnDropChannel` drives `onclose`. Verified
(mb_smoke 23k + render 40): `new WebSocket('wss://…')` -> onopen (readyState 1), `send('hello-ws')`
-> onmessage 'hello-ws', `close()` -> onclose. This is a LOOPBACK echo (proves the entire mojo
data plane offline); a real network backend over libcurl's WebSocket support can replace the echo
later with identical plumbing.
[DONE: Wake Lock] `navigator.wakeLock.request('screen')` — `MbWakeLockService.GetWakeLock` binds a
no-op `device::mojom::WakeLock` (headless: no real screen) and the permission service grants
SCREEN_WAKE_LOCK, so request('screen') resolves with a live sentinel (mb_smoke 23u: released==false).
[DONE: Battery] `navigator.getBattery()` — `MbBatteryMonitor` (device.mojom.BatteryMonitor, bound from
the frame broker) reports a static plugged-in/full battery (level 1, charging true, chargingTime 0).
QueryNextStatus long-polls for changes, so the first call answers and later calls are held open forever
(headless value never changes). mb_smoke 23z.
[DONE: Cookie Store API] `cookieStore.get/getAll/set/delete` — `MbCookieManager` (the
RestrictedCookieManager already serving document.cookie) gained real `GetAllForUrl` (returns the
origin's cookies as net::CanonicalCookies via CreateSanitizedCookie, honoring the options name filter:
EQUALS exact / STARTS_WITH prefix) and `SetCanonicalCookie` (writes name/value, past-expiry = delete,
bridges to the HTTP jar). cookieStore shares document.cookie's in-memory jar, so the two stay
consistent. mb_smoke 23aa (set 2, get 1 by name, getAll 2, document.cookie reflects them).
[DONE: MediaDevices] `navigator.mediaDevices.enumerateDevices()` — `MbMediaDevicesDispatcherHost`
(blink.mojom.MediaDevicesDispatcherHost, bound from the frame broker) returns empty device lists
(no cameras/mics/speakers headless), so enumerateDevices() resolves to []. BUG fixed: without the
host bound, blink's disconnect handler REJECTED the promise with AbortError, breaking feature probes.
The outer list carries kNumMediaDeviceTypes empty per-type lists (a blink DCHECK); capability getters
return empty; output-selection methods are unreached headless. mb_smoke 23ab.
[IN PROGRESS: OPFS — slice 1 DONE] `frame/mb_opfs.{h,cc}` — a real in-memory Origin Private File
System behind `blink.mojom.FileSystemAccessManager`. SLICE 1 (directory tree): `navigator.storage.
getDirectory()` resolves to a usable root over a process-wide in-memory tree (FsNode = dir children
or file bytes); directory handles support `getDirectoryHandle`/`getFileHandle` (create + navigate),
`keys()`/`values()`/`entries()` enumeration (GetEntries binds a fresh handle per child), and
`removeEntry`; a missing entry without `{create}` rejects with NotFoundError (kFileError +
FILE_ERROR_NOT_FOUND). NOTE: FileSystemAccessError.message must be a NON-NULL empty String (a default
WTF::String is null and fails mojo validation). Verified (mb_smoke 23ac: create docs/ + a.txt,b.txt,
enumerate, not-found). SLICE 2 (DONE): file CONTENT read/write — `createWritable()` returns an
`MbFsFileWriter` whose `Write(offset, data_pipe)` drains the pipe (mojo::DataPipeDrainer) and splices
bytes into a working buffer (a copy of the file when keep_existing_data); `Truncate` resizes, `Close`
commits the buffer to the node, `Abort` discards. `getFile()` -> `AsBlob` mints a BlobDataHandle
serving the bytes via a new blob-registry helper `MbCreateInlineBlob` (self-owned in-process Blob).
OPFS now round-trips create -> write -> close -> getFile().text(). Verified (mb_smoke 23ad: write
'hello opfs', read back, size 10). SLICE 3 (DONE): `OpenAccessHandle` (createSyncAccessHandle,
Worker-only) — an in-memory `FileSystemAccessFileDelegateHost` (`Read` returns a BigBuffer slice;
`GetLength`/`SetLength` report/resize; the `[Sync] Write` drains its pipe with
`mojo::BlockingCopyToString` — safe because blink feeds the pipe from another thread and closes it,
per file_system_access_incognito_file_delegate.cc — then splices at offset), returned as the union's
`incognito_file_delegate` + a no-op `FileSystemAccessAccessHandleHost`. Verified (mb_smoke 23ah) via a
mocked SAME-ORIGIN worker (a data: worker is opaque-origin -> OPFS SecurityError; see the MbFetchUrl
mock support below). STILL DEFERRED: per-origin isolation (single process-wide root).
[DONE: mockable worker scripts] `MbFetchUrl` now consults the response-mock table (MbFindMock) before
any scheme fetch, matching the async loader. So worker scripts / iframes / top-level navs can be
served by `mbMockResponse` — and a worker from a mocked https URL is SAME-ORIGIN with the page (a
data: worker is opaque), the route used to test OPFS sync access handles. mb_smoke 23ag.
[DONE: Storage Buckets] `frame/mb_storage_buckets.{h,cc}` (`MbBucketManagerHost` + `MbBucketHost`,
bound from the frame broker). `navigator.storageBuckets.open/keys/delete` track bucket names; each
bucket re-exposes the existing in-process backends — `GetIdbFactory`/`GetLockManager`/`GetCaches`
delegate to BindIDBFactory/BindLockManager/BindCacheStorage, `GetDirectory` to the OPFS root
(`MbBindOpfsRootDirectory`), with persist/estimate(2GB)/durability(relaxed)/expiry metadata. So
`navigator.storageBuckets.open('x')` gives a working bucket with indexedDB/caches/locks/getDirectory.
Verified (mb_smoke 23ae: open + keys + bucket.caches round-trip). NOTE: buckets are NOT yet isolated
from the default partition (the backing IDB/Cache/OPFS stores are process-wide, keyed by name).
[DONE: Cache Storage] `frame/mb_cache_storage.{h,cc}` (`MbCacheStorage` + `MbCacheStorageCache`,
bound from the frame broker). `caches.open/has/delete/keys`, `caches.match`, `cache.put`/`delete`
(via `Batch`), and `cache.match`. Stores Request URL -> FetchAPIResponse in a process-wide
per-cache-name registry. KEY SIMPLIFICATION vs the original plan: in the blink variant the response
body is a refcounted `scoped_refptr<BlobDataHandle>` (NOT a move-only SerializedBlob remote), so the
whole response just `.Clone()`s — the blob is shared by refcount and a cached response matches any
number of times, no blob-remote plumbing needed. `Open`/`Match` use `base::expected<Success,
CacheStorageError>` callbacks (success = the value; miss = `base::unexpected(kErrorNotFound)`).
Verified (mb_smoke 23v): `caches.open('v1')` -> `cache.put('/data',new Response('cached-body'))` ->
`cache.put('/data2',...)` -> `cache.match('/data')` -> text 'cached-body'; `cache.keys().length`==2;
`caches.has('v1')` true. `cache.matchAll(req?)` returns matching/all responses; `cache.keys(req?)`
rebuilds minimal GET requests from the stored URLs (FetchAPIRequest has NO Clone — its
`ResourceRequestBody body` field is non-clonable — so the cache keeps only URL->Response and
reconstructs requests for keys()). Query options honored: `{ignoreSearch}` drops the query/fragment
before comparison across Match/MatchAll/Keys/Batch-delete + caches.match (which also applies the
cache_name filter); ignoreMethod/ignoreVary are inherently satisfied (we store neither method nor
Vary). mb_smoke 23af. GetAllMatchedEntries still an empty stub.
[IN PROGRESS: IndexedDB — step 1 DONE] `frame/mb_indexeddb.{h,cc}` (`MbIDBFactory`, bound from
the frame broker) — an in-memory IDB backend. STEP 1 (open + schema): `indexedDB.open(name,ver)`
opens a database keyed by name in a process-wide registry; a new version fires the OPEN handshake
— `IDBFactoryClient.UpgradeNeeded` (carrying the IDBDatabase handle + current `blink::IDBDatabase-
Metadata`) so `onupgradeneeded` runs, the page's `createObjectStore` is recorded into the metadata
via the version-change `IDBTransaction.CreateObjectStore`, and the vc transaction's `Commit` fires
`IDBDatabaseCallbacks.Complete` + `IDBFactoryClient.OpenSuccess` -> `onsuccess`. Reopen at the same
version succeeds immediately. Verified (mb_smoke 23m + render 39): `open('mbdb',1)` ->
onupgradeneeded createObjectStore('items') -> onsuccess with `db.version==1` and
`objectStoreNames==['items']`. STEP 2 (DONE): the value/key data plane.
`IDBDatabase.CreateTransaction` binds a working transaction; `IDBTransaction.Put` stores the
serialized value bytes (`blink::IDBValue::Data()`) under the record's key in a per-object-store
map (the backend now holds `store_id -> {encoded_key -> bytes}`); `IDBTransaction.Commit` fires
`IDBDatabaseCallbacks.Complete` (so `tx.oncomplete` runs); `IDBDatabase.Get` looks the value up by
the range's only-key and returns an `IDBReturnValue` (the bytes + primary key + the store's key
path — which MUST match or `idb_request.cc` DCHECKs). Keys are encoded to a comparable string
(number/date/string/binary; arrays/none unsupported). Verified (mb_smoke 23m + render 39):
open -> createObjectStore -> readwrite put({id:7,name:'widget',qty:3}) -> tx.oncomplete -> get(7)
returns the structured-cloned object intact ("widgetx3"). STEP 3 (DONE): object-store CRUD rounded out
— `IDBDatabase.Count` (single-key 0/1, else whole-store size), `DeleteRange` (single-key erase,
unbounded = clear) and `Clear` operate on the backend record maps. Verified (mb_smoke 23n): put
3 -> delete(2) -> count 2 -> clear -> count 0. STEP 4 (DONE): key ordering + getAll +
ranges. Records are now keyed by an ORDER-PRESERVING encoding (a type-rank byte — number <
date < string < binary — then big-endian sign-flipped doubles / raw string-or-binary bytes), so
the `std::map` iterates them in IndexedDB key order. Each record also stores a clone of its
primary key. `IDBDatabase.GetAll` emits records (Keys/Values/Records result types) in key order,
honoring the key range (encoded-key `lower_bound`/`upper_bound` with open/closed ends), max_count,
and Next/Prev direction — and `Count`/`DeleteRange` use the same encoding. Verified (mb_smoke
23o): insert id 3,1,2 -> `getAll()` returns them ordered 1,2,3. (Values-only getAll must NOT carry
record primary keys — blink CHECKs that.) STEP 5 (DONE): cursors. `IDBDatabase.OpenCursor`
snapshots the in-range encoded keys in iteration order (forward or reverse) and hands back a
`MbIDBCursor` (self-owned `IDBCursor` receiver) + the first record (or empty). `IDBCursor.Continue`
(plain or to-a-key), `Advance`, and `Prefetch`/`PrefetchReset` walk that snapshot, looking each
record up live by key and returning it as an `IDBCursorValue`. Verified (mb_smoke 23p): insert id
3,1,2 -> `openCursor()` + `continue()` visits 1,2,3. The object-store read/write surface is now
broad: open+schema, put/get, count/delete/clear, getAll/ranges, AND cursors. STEP 6 (DONE): autoincrement key
generation. An `{autoIncrement:true}` store generates keys on a keyless `put` — blink sends a
None-typed key for those, so the backend assigns the next per-store counter value (and an explicit
numeric key bumps the counter). `GetKeyGeneratorCurrentNumber` reports it. Two blink invariants
handled in `BuildReturnValue`: `IDBReturnValue.primary_key` is non-nullable, and the key injector
DCHECKs a String key path — so in-line (keyPath) stores send the real key + path while out-of-line
stores send a None key (which blink ignores, skipping injection). Verified (mb_smoke 23q): two
keyless puts on an `{autoIncrement:true}` store get keys 1 and 2, retrievable by those keys. STEP 7 (DONE): secondary indexes.
`IDBDatabase.CreateIndex`/`DeleteIndex` register index metadata on the store; `IDBTransaction.Put`
populates a per-store/per-index map (encoded index key -> set of encoded primary keys) from the
`index_keys` blink computes, and `SetIndexKeys` does the same for createIndex on an existing store;
`IDBDatabase.Get` with `index_id != kInvalidId` resolves the index key to a primary key and returns
the record (delete/clear/re-put shed stale index entries). Verified (mb_smoke 23r): a 'by_author'
index on a books store -> `index('by_author').get('bob')` returns the matching record. STEP 8 (DONE): index cursors.
`OpenCursor` with `index_id != kInvalidId` iterates the index entries in index-key order
(reversed for prev); the cursor snapshots (cursor-key, primary-key) encoded pairs, and reports the
cursor key by DECODING the encoded index key (a new `DecodeKey`/`DecodeDouble`, the inverse of the
order-preserving encoding) while the primary key + value come from the live record. Verified
(mb_smoke 23s): books inserted by isbn C,A,B walk in author order alice,bob,carl with correct
index keys. STEP 9 (DONE): unique-index constraints —
a `{unique:true}` index rejects a `put` whose index key already maps to a different record
(`ConstraintError`). Verified (mb_smoke 23t): a duplicate email on a unique 'email' index is
rejected. STEP 10 (DONE): multiEntry indexes + index getAll — `GetAll` ignored `index_id`
(applied the range to primary keys), so `index.getAll(key)`/`getAllKeys(key)` found nothing;
fixed to walk `index_data[store][index]` in index-key order and resolve each key's primary-key
set to records. multiEntry then works end to end (blink expands an array key path into one
`IDBIndexKeys` list renderer-side; the backend inserts each element; `index.get(element)` +
`index.getAll(element)` both resolve). Verified (mb_smoke 23w). STEP 11 (DONE): transaction
atomicity/rollback — a lazy per-transaction snapshot (deep-cloned data + key generators +
indexes, captured on the first `Put`/`DeleteRange`/`Clear`, keyed by txn id) lets
`IDBDatabase.Abort` restore pre-transaction state and fire `IDBDatabaseCallbacks.Abort(kAbortError)`
-> `onabort`; `Commit` discards it. Read-your-writes preserved (writes go live). Verified
(mb_smoke 23x: abort undoes a modify + an insert). STEP 12 (DONE): compound (array) keys —
the key encoder rejected Array keys (DataError), so a store with keyPath ['a','b'] was unusable.
Added an order-preserving array encoding (each element escaped 0x00->0x00 0x01 + terminated
0x00 0x00; array type-rank 0x50 sorts after scalars; element-wise compare, shorter prefix first)
+ the DecodeKey inverse for compound index-cursor keys. Verified (mb_smoke 23y: get([1,2]) +
ordered getAll). NOT yet: persistence is in-memory (per-process, by db name).
IndexedDB now covers open/schema, put/get, count/delete/clear, getAll/ranges, object-store +
index cursors, autoincrement, index lookups (incl. multiEntry + index getAll), unique
constraints, atomic abort, and compound keys — the whole object-store/index API real apps use.]
9. Storage/cookie persistence across runs — cookies already persist (mbSaveCookies/Load,
   Netscape jar). [DONE: localStorage] `mbSaveLocalStorage(out)` snapshots the whole
   localStorage for the origin as a JSON string + `mbLoadLocalStorage(json)` restores it —
   save to disk after login, reload next run (the localStorage peer of the cookie jar).
   Verified (mb_smoke 23b): set keys (incl. a quote needing escaping) → snapshot → clear
   (fresh run) → restore → keys back. [REMAINING: async CookieStore API + storage change
   events; IndexedDB persistence (needs the IndexedDB broker, see #8).] 10. Blob-from-file
+ ranged blob reads + DataPipeGetter uploads. 11. **GPU content path** (WebGL / accel-2d-canvas /
`<video>` render blank) — the heaviest; needs a GL/media provider. Last.

**Tier 3 — input & rendering refinements:**
12. Input fidelity. [DONE: button + modifier clicks] `mbSendMouseClickEx(x, y, button, modifiers)`
    — button 0=left/1=middle/2=right, modifiers bitmask 1=ctrl 2=shift 4=alt 8=meta — so the
    page sees e.button + e.ctrlKey/shiftKey/altKey/metaKey (left→click, middle→auxclick,
    right→contextmenu). Verified (mb_smoke 0i): shift+alt left → "0,true,true", middle →
    auxclick button 1, right → contextmenu. (NB ctrl+click = macOS secondary click.)
    [DONE: IME] `mbSendIme(composing, committed)` drives the focused editable through the
    widget's SetComposition (compositionstart/update preview) + CommitText (compositionend +
    input, inserts) — CJK/accented input via an input method. Verified (mb_smoke 0i2): focus
    input, mbSendIme("にほ","日本") → value 日本, compositionstart+end each fired once.
    [REMAINING: native HTML5 drag-drop (hard — drag controller + DataTransfer), trusted
    touch/wheel.]
13. [DONE] PDF options. `mbSavePdfEx(path, width_pt, height_pt, landscape, scale, margin_pt)`
(mbSavePdf kept = Letter default) + `mb_shot --pdf-size letter|a4|legal|a3|tabloid|WxH
--landscape --pdf-scale N --pdf-margin PT`. Page size in points; landscape swaps w/h; content
scale clamped 0.1–5; uniform margin. Verified (mb_smoke 39b): A4 → MediaBox [0 0 595 842],
landscape → [0 0 842 595]; CLI `--pdf-size a4 --landscape` → [0 0 842 595].
(print-background isn't a separate toggle — Blink's print path paints backgrounds already.) 14. [DONE] Child-frame charset was hardcoded UTF-8 → mojibake on non-UTF-8
iframes; `DoCommit` now honors an explicit `charset=` from the fetched Content-Type
(default still UTF-8, so UTF-8/srcdoc/data: cases are unchanged). Verified (mb_smoke_render
78c): a `charset=shift_jis` iframe serving the Shift-JIS bytes for 日本 decodes to U+65E5
U+672C, not U+FFFD. 15. [DONE] CSP/PolicyContainer no longer leaks across navigations in a reused view.
`CommitHtml` now attaches a FRESH empty `WebPolicyContainer` with a BOUND remote
(`MbPolicyContainerHost`, moved to the header + shared with `DoCommit`) — the earlier
crash was an *unbound null* remote (a CHECK), not the policy container itself. Verified
(mb_smoke 0j): a `script-src 'none'` page blocks its own script, then a normal page in
the SAME view runs its script (CSP shed). Every-load path; no crash, all suites green.

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
