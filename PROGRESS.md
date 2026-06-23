# miniblink-modern — PROGRESS (loop state, read this first every iteration)

> Autonomous `/loop` (every 5 min) is upgrading miniblink from M47 Blink to
> **modern M150 Blink** via a hand-written minimal in-process host. This file is
> the single source of truth for loop continuity. **Read it, do the next action,
> update it, repeat.**

## Git
- miniblink-modern is a git repo (user-init'd). Initial commit 64f098b (2026-06-23).
- COMMIT CONVENTION (parent CLAUDE.md, OVERRIDES the harness default): author
  `Xin Yang <yangxin0@outlook.com>`, ~72-col wrapped body explaining WHY, NO AI/Claude
  trailer (no Co-Authored-By). .gitignore excludes vendor/reference/ (verbatim Chromium copy).
- patches/ are tracked here but apply to the DONOR chromium tree, not this repo.
- Commit per-milestone going forward (only when it's a clean, tested state).

## Fixed facts
- **Goal:** modern Blink (M150) running, driven by the `wke`/`mb` C API. NOT CEF.
  Single-process, libcurl networking, small outer shell.
- **Donor tree (GN/Ninja, already builds):** `/Users/yangxin/dennis/chrome/chromium-150.0.7871.24`
  - `out/Release/` already `gn gen`'d + ~46k objects / 540 dylibs compiled.
  - `is_component_build = true`, macOS SDK 26, system Xcode. gn at `buildtools/mac/gn`.
- **This standalone project:** `/Users/yangxin/dennis/chrome/miniblink-modern/`
  - **Mirrors the MODERN Chromium root layout** (user-directed) so copied code's
    root-relative includes resolve. Include-root = `src/`.
    - `src/third_party/blink/...`, `src/base/...` etc. = copied/vendored modern code,
      kept under their ORIGINAL chromium paths (so `#include "third_party/blink/..."`
      and `#include "base/..."` compile unchanged).
    - `src/miniblink_host/` = our content-layer replacement (top-level peer to where
      `content/` would be). Our includes: `#include "miniblink_host/..."`.
    - `src/wke/`, `src/mb/` = public C API. `src/port/` = platform hosts. `docs/` = specs.
  - ⚠️ Location was chosen by the loop. If the user wants it elsewhere
    (e.g. inside miniblink49), MOVE IT on the next iteration when they say so.
- **Old target (for API parity reference):** `/Users/yangxin/dennis/chrome/miniblink49`

## Architecture (APPROVED by user)
3 layers, C-ABI seam dissolves the GN↔CMake mismatch:
1. Blink + substrate — built AS-IS by GN in the donor tree (never ported to CMake).
2. `miniblink_host` (NEW, GN target) — implements `blink::Platform`, registers
   browser-side Mojo into the `BinderMap` (LocalFrameHost, PageBroadcast, Widget…),
   libcurl `URLLoaderFactory`, viz/cc compositor→bitmap, does the
   `WebView::Create` + `WebLocalFrame::CreateMainFrame` handshake, exposes `extern "C"`.
3. `wke`/`mb` C API + platform hosts — link the C ABI. (CMake only here.)

**De-risk blueprint:** `third_party/blink/renderer/core/testing/sim/` (SimTest) +
`core/frame/frame_test_helpers.{h,cc}` already drive Blink WITHOUT `//content`.
Every interface they MOCK, `miniblink_host` must IMPLEMENT for real. Seam entry:
`blink::CreateMainThreadAndInitialize(Platform*, mojo::BinderMap*)`.

## Phases (each = own spec→plan→build)
- **P0 — build + "Blink renders" spike** ← CURRENT. Build `blink_unittests` (SimTest),
  prove Blink renders to a bitmap, enumerate the real browser-side Mojo/Platform surface.
- P1 — minimal host + libcurl loader → fetched static page → bitmap, C ABI v0.
- P2 — wke/mb C ABI + macOS host → real site in Cocoa window (JS free via cppgc V8).
- P3 — input/events/navigation → baidu interactive.
- P4 — GPU compositing + perf.
- P5 — Windows host + API parity.

## CURRENT STATE (update every iteration)
- [in progress] P0.1: building `blink_unittests` in background.
  - bg task id: **boqyrpe51**  (output: tasks/boqyrpe51.output in scratchpad)
  - HEALTHY + PROGRESSING: **796 objs compiled** since 10:40; mem 48% free (no swap);
    now grinding Blink's heaviest core/ TUs (local_frame, local_dom_window,
    web_local_frame_impl — 5-8 min each w/o PCH). NOT stuck. ETA maybe 20-40 more min.
    NOTE: "0 .o in last N min" is a FALSE alarm during heavy-TU clusters — check
    "total objs since 10:40" instead. SPEED LEVER: enable PCH / smaller target.
- [ ] P0.2: run a SimTest render case, dump pixels, confirm non-blank. (blocked on build)
- [x] **P0.3 DONE: `docs/interface-surface.md` written** — minimal static-render surface
      fully enumerated from frame_test_helpers.cc (file:line cited). HEADLINE: browser-side
      Mojo is ~all null/no-op (PageBroadcast=NullAssociatedReceiver @:778,
      BrowserInterfaceBroker=NullRemote @:489). Minimal host = Platform + WebViewClient +
      WebLocalFrameClient + software WebFrameWidget/compositor + libcurl URLLoader hook.
      Loading plugs in at URLLoader level (URLLoaderTestDelegate), NOT mojom URLLoaderFactory.
- [x] **P0.4 DONE: `docs/phase-1-spec.md` written** — full host file layout under
      `src/miniblink_host/`, C ABI v0, build strategy (iterate in donor tree as
      `//miniblink_host`, then copy+vendor into standalone). Each file maps to an
      interface-surface row.

## DONE so far in P1
- vendor/reference/ has the blueprint files (frame_test_helpers, testing_platform_support, sim_*).
- Header scaffolds under src/miniblink_host/: capi/mb_capi.h (FULL C ABI),
  platform/mb_platform.h, runtime/mb_runtime.h, frame/mb_frame_client.h,
  loader/mb_url_loader.h, widget/mb_widget.h, view/mb_webview.h, + DRAFT BUILD.gn.
- Pixel path understood: MbWidget subclasses WebFrameWidgetImpl (like TestWebFrameWidget),
  overrides AllocateNewLayerTreeFrameSink -> software sink -> SkBitmap; drive sync
  BeginFrame like SimCompositor. WidgetHost/FrameWidgetHost overrides = no-op.
- ALL structural headers DONE: + frame/mb_view_client.h, widget/mb_sw_frame_sink.h.
- **DEEPEST RISK RESOLVED — pixel readback:** real pixels = replay Blink's
  `LocalFrameView::GetPaintRecord()` (a cc::PaintRecord) into an SkBitmap-backed SkCanvas
  (per SimCompositor::BeginFrame, sim_compositor.cc:50,61). NO viz SoftwareRenderer. The
  cc LayerTreeFrameSink is a STUB to satisfy CompositeForTest, not the pixel source.
- **FULL SOURCE SET COMPLETE: 19 files** (9 .h, 9 .cc, BUILD.gn) under src/miniblink_host/.
  capi/mb_capi.cc = COMPLETE dispatcher. mb_runtime/mb_platform/mb_webview = substantive
  scaffolds w/ real structure + commented real blink calls. 5 others = ctor/dtor stubs.
  All bodies have TODO(mb) for the parts the COMPILER must pin.
- ⚠️ ALL current diagnostics = unset include-root ("file not found" for miniblink_host/*).
  NOT bugs. They vanish once built as the GN target with include-root=the host's parent.

## ✅✅ MILESTONE (2026-06-23 11:32): libminiblink_host.dylib BUILDS + LINKS vs modern Blink
All 9 host objects compile; `[3/3] SOLINK libminiblink_host.dylib`. At
`chromium-150.../out/Release/libminiblink_host.dylib`. The GN↔CMake seam + the whole
host-subclasses-modern-Blink architecture is PROVEN. Only fixes needed were 3 trivial
-Werror nits (unused private fields → [[maybe_unused]]; commented /* override */ removed).
Bodies are still stubs (return false / no-op) — NEXT is fleshing them.

## ✅✅✅ MILESTONE (2026-06-23 11:42): mbInitialize() == 1 — modern Blink RUNS in-process
mb_smoke prints "OK: modern Blink main thread + V8 isolate are up", clean exit. Full
bring-up works. The recipe that works (mb_runtime.cc), in order:
  1 AtExitManager, 2 CommandLine::Init(0,nullptr), 3 base::i18n::InitializeICU,
  4 FeatureList::SetInstance(empty), 5 mojo::core::Init, 6 SingleThreadTaskExecutor(UI),
  6b ThreadPoolInstance::CreateAndStartWithDefaultParams, 7 new MbPlatform (with EMPTY
  broker — non-null!), 7b gin::V8Initializer::LoadV8Snapshot(kWithAdditionalContext),
  8 blink::CreateMainThreadAndInitialize(platform,&empty_binder_map),
  9 blink::CreateMainThreadIsolate. Needs in out/Release: icudtl.dat,
  v8_context_snapshot.arm64.bin (both present).
TWO config lessons baked into BUILD.gn:
  - configs += "//v8:external_startup_data"  (defines V8_USE_EXTERNAL_STARTUP_DATA so the
    gin snapshot API is available — without it the header #errors).
  - deps += "//gin".
Crashes solved this session (both via .ips crash reports, no lldb): (a) null
GetBrowserInterfaceBroker → empty broker; (b) missing V8 context snapshot load.

## ✅✅✅✅ MILESTONE (2026-06-23 12:04): mbCreateView SUCCEEDS — WebView + main LocalFrame live
mb_smoke: mbInitialize->1, mbCreateView->non-null, "WebView + main LocalFrame created",
CLEAN EXIT (no crash). A real modern-Blink WebViewImpl + LocalFrame, created by our host.
ROOT CAUSES fixed for the cppgc heap mismatch + teardown:
  1. **DOUBLE ISOLATE**: blink::Initialize() ALREADY creates the main-thread isolate
     internally (blink_initializer.cc:191 V8Initializer::InitializeMainThread, consuming
     ThreadState's cppgc heap). Calling blink::CreateMainThreadIsolate() AGAIN made a 2nd
     isolate w/ a fresh heap -> AttachToIsolate cpp_heap mismatch. FIX: call blink::Initialize
     ONLY; grab isolate via v8::Isolate::GetCurrent(). (Reference for the whole sequence:
     content/test/test_blink_web_unit_test_support.cc ctor + blink_test_environment.cc.)
  2. **TEARDOWN**: MainThreadSchedulerImpl dtor DCHECKs was_shutdown_. FIX:
     main_thread_scheduler_->Shutdown() in ~MbRuntime.
WORKING bring-up recipe is now mb_runtime.cc steps 1-10 (no step 11). KEEP this.

## ✅ WIDGET ATTACHED + LAYOUT RUNS (2026-06-23 12:11)
mbCreateView now: WebView + main LocalFrame + NON-COMPOSITING frame widget + Resize +
DidAttachLocalMainFrame. Clean RC=0. Layout pipeline executes.
Fixes/decisions this step:
- WIDGET = non-compositing path: main_frame->InitializeFrameWidget(4 no-op associated
  channels [drop the browser-side ends] + viz::FrameSinkId(1,1)) then
  widget->InitializeNonCompositing(this) then widget->Resize(gfx::Size). (mb_widget.cc)
  No LayerTreeHost/frame-sink/settings needed. WebNonCompositedWidgetClient = 1 no-op vtbl.
- WebView::Create MUST use compositing_enabled=FALSE + widgets_never_composited=TRUE
  (InitializeNonCompositing DCHECKs !View()->does_composite()).
- MbPlatform::DefaultLocale() MUST be non-empty ("en-US") — layout/font selection reads
  DefaultLanguage() and segfaults on a null locale.
- WebString factory is FromUtf8 (NOT FromUTF8).
- mojom widget headers: public/mojom/page/widget.mojom-blink.h (FrameWidget/Host),
  public/mojom/widget/platform_widget.mojom-blink.h (Widget/Host). viz FrameSinkId:
  components/viz/common/surfaces/frame_sink_id.h. All reachable transitively (no new deps).

## 🎉🎉🎉🎉🎉 P1 COMPLETE (2026-06-23 12:20): MODERN BLINK RENDERS HTML→PIXELS
mb_smoke: mbInitialize -> create view -> mbLoadHTML -> mbPaintToBitmap -> RENDER OK.
480000/480000 non-white pixels; /tmp/mb_out.png = solid RED (the inline CSS
background:#ff0000), confirmed visually. **Full pipeline works: HTML parse -> CSS ->
layout -> paint -> readback**, driven entirely by the hand-written in-process host. NOT CEF.
Working bits:
- LoadHTML: To<WebLocalFrameImpl>(frame)->CommitNavigation(
    WebNavigationParams::CreateWithHTMLStringForTesting(base::span<const char>(html),
    WebURL{KURL(base_url)}), nullptr); then base::RunLoop().RunUntilIdle().
    NOTE: INSIDE_BLINK => WebURL is built from KURL (GURL ctor is non-INSIDE_BLINK only).
- PaintToBitmap: widget()->UpdateAllLifecyclePhases(DocumentUpdateReason::kTest); then
    To<WebLocalFrameImpl>(frame)->GetFrame()->View()->GetPaintRecord().Playback(SkCanvas
    over an SkBitmap installPixels'd on the caller's BGRA8888 buffer). NO compositor.
- WebString factory = FromUtf8 (lowercase tf).

### ✅ TEXT/FONTS NOW RENDER (2026-06-23 12:27): "hello modern blink" green-on-red, bold
/tmp/mb_out.png shows the h1 text rendered. FIXES:
1. RESOURCES: built //third_party/blink/public:resources -> blink_resources.pak (in
   out/Release/gen/.../). Copied next to the binary (out/Release/blink_resources.pak).
   mb_runtime: ui::ResourceBundle::InitSharedInstanceWithPakPath(DIR_ASSETS/blink_resources.pak)
   (step 4b, before blink init). MbPlatform::GetDataResource ->
   ResourceBundle::GetSharedInstance().GetRawDataResourceForScale(id, scale);
   GetDataResourceString -> LoadDataResourceString. (+//ui/base dep.) This loads the UA
   stylesheet (IDR_UASTYLE_HTML_CSS) -> h1 gets default bold/size/margins.
2. FONTS: generic_font_family_settings_ is EMPTY by default -> no font resolves -> no glyphs.
   In MbWebView::Create after WebView::Create, on web_view_->GetSettings():
   SetStandardFontFamily("Times")/Serif("Times")/SansSerif("Helvetica")/Fixed("Courier")
   (USCRIPT_COMMON) + SetDefaultFontSize(16) + SetDefaultFixedFontSize(13). macOS CoreText
   provides these. NOTE deploy: blink_resources.pak must sit next to the binary.

### ✅✅ JAVASCRIPT WORKS (2026-06-23 12:31): V8 runs, DOM mutates, re-renders
/tmp/mb_out.png shows "JS WORKS" green-on-BLUE: the inline <script> ran
document.body.style.background='#0000ff' (blue, 478108 px) AND
getElementById('t').textContent='JS WORKS'. Full reactive loop: V8 exec -> DOM mutation ->
style recalc -> relayout -> repaint. ONE fix needed: WebSettings::SetJavaScriptEnabled(true)
(OFF by default). Inline scripts run during CommitNavigation parse + RunLoop().RunUntilIdle().
=> The engine is a functioning modern browser engine: HTML+CSS+UA sheet+fonts+JS+DOM+layout+paint.

### ✅✅✅ MODERN CSS RENDERS (2026-06-23 12:37) — the upgrade payoff, visually proven
/tmp/mb_out.png: CSS Grid (3-col) + Flexbox(gap) + linear-gradient bg + border-radius +
box-shadow + 2D transform(rotate) + JS text — ALL features M47 miniblink could NOT do.
ONE fix: needed a base::DiscardableMemoryAllocator (gradients/images/shaders allocate it).
Couldn't use //base/test:test_support (testonly -> non-testonly dep error); wrote a trivial
inline heap-backed MbDiscardableAllocator in mb_runtime.cc (Lock()=true, never discards).
=> The "why upgrade" is now demonstrated, not just asserted.

### ✅ FILE LOADING + IMAGE/SVG DECODE (2026-06-23 12:43)
/tmp/mb_out.png: an SVG data-URI <img> (concentric circles) rendered in a flex row, page
loaded from disk. NEW capabilities:
- mbLoadURL("file://PATH"): MbWebView::LoadURL reads the file (base::ReadFileToString) and
  CommitNavigations it. (http(s) still needs the libcurl factory; data:/self-contained ok.)
- Image decoding + SVG rendering work (no extra wiring needed beyond the discardable allocator).
KEY (confirmed last tick): for EXTERNAL subresources, override
MbFrameClient::CreateURLLoaderForTesting() -> a blink::URLLoader; it IS called in the
PRODUCTION path (loader_factory_for_frame.cc:151), not gated to web-test mode. blink::URLLoader
is concrete (override LoadAsynchronously; body streams via a mojo data pipe to URLLoaderClient)
-> that's the multi-tick piece for real HTTP (+libcurl in the standalone build).

### ✅ STANDALONE PROJECT PACKAGED (2026-06-23 12:50)
miniblink-modern is now a coherent self-contained deliverable:
- README.md (architecture, capability matrix, C ABI, embedded demo screenshots).
- build.sh /path/to/chromium-150 — stages host -> donor tree, gn gen, ninja
  miniblink_host+mb_smoke, vendors blink_resources.pak, runs smoke. VERIFIED working.
- docs/demos/*.png (modern-css, javascript, file-and-image, text) — saved proofs.
- src/miniblink_host/ (20 files) = canonical source of truth; build.sh stages it.
NOTE: mb_smoke's "blue(JS)" check is stale (current page uses a gradient) — cosmetic log
only; render is correct per screenshots. "standalone" = this source + GN-built dylib + pak.

## ✅ SUBRESOURCE LOADING WORKS — external IMAGE renders (2026-06-23 13:18)
External SVG `<img src=file.svg>` loads via MbURLLoader and RENDERS exact colors
(green rect + pink circle) — /tmp/mb_out.png verified. The subresource URLLoader is
FULLY FUNCTIONAL: fetch + body + response + decode + paint.
KEY FIXES this round:
- WebSettings::SetLoadsImagesAutomatically(true) — OFF by default; without it no <img> fetch.
- Body delivery: client_->DidReceiveResponse(resp, ScopedDataPipeConsumerHandle()/*empty*/,
  nullopt) + client_->DidReceiveDataForTesting(span) + DidFinishLoading (matches
  url_loader_test_delegate.cc; both that and SegmentedBuffer funnel to DidReceiveDataImpl).
- PaintToBitmap now interleaves 5x (UpdateAllLifecyclePhases + RunUntilIdle) so lazily-loaded
  subresources (images requested during layout) settle before the final paint.
- Response: set Content-Type header too (not just SetMimeType).

### ✅ EXTERNAL <link> CSS NOW APPLIES (2026-06-23 13:32) — root cause was MimeRegistry
/tmp/mb_out.png: dark-blue bg + cyan Helvetica h1, all from the external stylesheet.
ROOT CAUSE (deep!): CSSStyleSheetResource::CanUseSheet (css_style_sheet_resource.cc:188-208)
enforces, for file: URLs, that the file EXTENSION maps to text/css via
MIMETypeRegistry::GetMIMETypeForExtension — which PROXIES to a browser-process Mojo
**MimeRegistry** service (mime_type_registry.cc:71). My empty broker dropped it -> ext->mime
returned empty -> sheet REJECTED. (Images skip CanUseSheet, so they worked; <style> has no
file URL, so it worked.)
FIX: implemented MbMimeRegistry : blink::mojom::blink::MimeRegistry (maps css/js/svg/png/...)
and bound it in MbEmptyBroker::GetInterfaceImpl via
  if (auto r = receiver.As<blink::mojom::blink::MimeRegistry>())
    mojo::MakeSelfOwnedReceiver(make_unique<MbMimeRegistry>(), std::move(r));
=> The browser-interface broker is now the place to add any in-process service blink needs.
NIT: remove the [mb_url_loader] debug fprintf eventually. Also LoadHTML 20x / PaintToBitmap
5x RunUntilIdle is generous — can tune later.

## (resolved-fetch, see above) SUBRESOURCE URLLoader: fetches files
- Implemented MbURLLoader : blink::URLLoader (src/.../loader/mb_url_loader.{h,cc}),
  returned by MbFrameClient::CreateURLLoaderForTesting(). CONFIRMED CALLED by blink's
  production loader path: stderr shows `[mb_url_loader] file:///tmp/mb_style.css -> OK (207
  bytes)`. Reads file:// from disk; delivers body via SegmentedBuffer in DidReceiveResponse
  (ResourceLoader consumes it via DidReceiveDataImpl, resource_loader.cc:43-44).
- BUT: external CSS does NOT apply (bg stays white). NOT timing (8x RunUntilIdle before
  paint), NOT file:// security (SetAllowFileAccessFromFileURLs(true) didn't help).
- >>> NEXT TICK diagnostic:
  1. ISOLATE: test an external <img src="file.svg"> instead of CSS. If the image renders,
     body delivery works and it's CSS-specific (mime/stylesheet-activation). If not, the
     body isn't reaching blink -> the WebURLResponse is likely under-specified.
  2. SUSPECT: the bare WebURLResponse (status+mime+url only) may be missing fields blink
     needs to treat the load as a valid same-origin success — try SetType(
     network::mojom::FetchResponseType::kBasic), set HTTP headers (Content-Type via
     AddHttpHeaderField), SetConnectionID, or model the response EXACTLY on what
     URLLoaderTestDelegate/url_loader_mock builds (read url_loader_test_delegate.cc +
     how the mock factory fills WebURLResponse).
  3. Consider: maybe need client_->DidReceiveData() in addition, or DidFinishLoading args.
- Keep the `[mb_url_loader]` debug fprintf until this is solved.

### ✅ CANVAS 2D works (2026-06-23 13:40)
docs/demos/canvas-2d.png: <canvas>.getContext('2d') + fillRect/arc/createLinearGradient/
fillText all render (CanvasRenderingContext2D -> skia software raster, no GPU). First try,
no new wiring needed. Another major subsystem validated.

### ✅ mb_shot CLI + PNG output (2026-06-23 13:52)
The deliverable tool: `mb_shot <input.html|file://URL> <out.png> [w h]` renders headlessly
to a PNG. Added C ABI `mbSavePng(view, path, w, h)`; host encodes via
gfx::PNGCodec::EncodeBGRASkBitmap + base::WriteFile (NOT SkPngEncoder — that symbol isn't
in the linked skia; +dep //ui/gfx/codec). Refactored paint into MbWebView::PaintInto(SkCanvas&)
shared by PaintToBitmap + SavePng. GOTCHA fixed: forward-declare `class SkCanvas;` at GLOBAL
scope — `PaintInto(class SkCanvas&)` inside namespace mb declared mb::SkCanvas (incomplete).
tools/mb_shot.cc + executable("mb_shot") in BUILD.gn; build.sh builds it. Hero demo:
docs/demos/mb_shot.png.

### ✅✅✅✅ HTTP(S) — LIVE WEBSITES RENDER (2026-06-23 14:24)
`mb_shot https://example.com out.png` → docs/demos/live-website.png (real page rendered).
UNBLOCKED via **system libcurl** (macOS SDK has curl.h; `libs=["curl"]` links it; HTTPS via
SecureTransport) — NOT chromium's network service, honoring the libcurl design intent.
- mb_url_loader.cc: FetchHttp() (curl_easy: follow redirects, gzip, UA, 30s timeout) +
  MbFetchUrl(spec, body, content_type) dispatching file/http. Deliver uses server
  Content-Type for the response mime (strips "; charset"); subresources over http work.
- mb_webview.cc LoadURL: http(s) top-level fetched via MbFetchUrl, committed with base=URL
  so relative subresources resolve through MbURLLoader.
- BUILD.gn: libs = [ "curl" ].
NOTE: FetchHttp is SYNCHRONOUS (blocks the task) — fine for headless/screenshot; async
(curl-multi on a thread) is a future refinement. Cross-origin subresources rely on default
CORS (stylesheets/images/scripts from other origins load fine).
PROVEN on a complex real site: `mb_shot https://news.ycombinator.com` fetched news.css +
hn.js + y18.svg + s.gif + triangle.svg (all logged) and rendered the full HN front page
correctly (docs/demos/hacker-news.png). Subresource HTTP loading works end-to-end.

### ✅ ROBUSTNESS SWEEP — 5/5 diverse real sites render (2026-06-23 14:38)
Swept example.org / danluu / gnu.org / lite.cnn.com / news.ycombinator — found + fixed 2
real crashes; now all render (gnu.org incl. a <video>: docs/demos/gnu-org.png).
FIX 1 (affects many sites that read navigator.onLine/connection):
  mb_runtime after blink::Initialize — WebNetworkStateNotifier::SetOnLine(true) +
  SetWebConnection(kWebConnectionTypeEthernet, 100.0). Else network_state_notifier DCHECK.
FIX 2 (<video>/<audio> pages): media-controls UA CSS (IDR_UASTYLE_MEDIA_CONTROLS_CSS) is in
  a SEPARATE pak (media_controls_resources_100_percent.pak), not blink_resources.pak.
  ResourceBundle::GetSharedInstance().AddDataPackFromPath(...media_controls...,
  ui::k100Percent). Vendored next to binary; build.sh copies it too.
METHOD: rendering diverse real sites + checking exit code is a cheap, high-value way to
find missing-platform-service crashes. More to find on heavier sites (e.g. fonts/@font-face,
forms, more navigator.* APIs) — repeat as needed.

### ✅ FETCH() + DATA-PIPE BODY DELIVERY (2026-06-23 14:34) — MDN etc. render; 9/10 sites
Big fix: switched MbURLLoader body delivery from DidReceiveDataForTesting to a real Mojo
DATA PIPE (the production path). Required for the Fetch API (FetchManager streams the body
through a BytesConsumer; the shortcut left place_holder_body_ set -> DCHECK on python/MDN).
THREE coordinated pieces:
1. mb_url_loader.cc: mojo::CreateDataPipe + DidReceiveResponse(consumer) + mojo::DataPipeProducer
   ::Write(StringDataSource over body_) async, then DidFinishLoading in the completion cb.
   (StringDataSource takes span<const char>; keep body_ alive in the loader; +deps
   //mojo/public/cpp/system.)
2. mb_runtime.cc: data pipes need mojo HANDLE WATCHING -> added an IO base::Thread +
   mojo::core::ScopedIPCSupport(io_thread->task_runner(), FAST). Without it
   SimpleWatcher/DataPipeBytesConsumer crash.
3. mb_url_loader: override GetTaskRunnerForBodyLoader() -> SingleThreadTaskRunner::
   GetCurrentDefault() (the default URLLoader ctor leaves it null -> SimpleWatcher null-runner DCHECK).
RESULT: MDN (docs/demos/mdn.png), HN, gnu, wikipedia, rust-lang, cnn, w3, danluu, example.org
all render (9/10). Suite still 8/8. Normal loads (CSS/img) unaffected — data pipe is universal.
### >>> NEXT: python.org crashes in Document::AddedEventListener (SIGSEGV via JS
   addEventListener). Narrow: some event type touches a null frame/page service. Backtrace
   in ~/Library/Logs/DiagnosticReports. Find which listener/null-ptr and guard/provide it.

### ✅ SCROLL-LISTENER CRASH FIXED → 10/10 sites render (2026-06-23 14:54)
python.org (and any page that does document.addEventListener('scroll',...)) crashed in
WebFrameWidgetImpl::SetHaveScrollEventHandlers — it derefs widget_base_->LayerTreeHost()
UNCONDITIONALLY, but our NON-COMPOSITING widget has no LayerTreeHost. (Found empirically:
tested each event type via mb_shot; only 'scroll' crashed. Path: EventHandlerRegistry
:236 -> ChromeClientImpl::SetHasScrollEventHandlers -> LocalRootFrameWidget()->...)
FIX = a 1-line blink null-guard (patches/0001-noncompositing-scroll-guard.patch), applied
to the donor tree by build.sh (idempotent git apply). python.org now renders
(docs/demos/python-org.png). FULL SWEEP NOW 10/10.
DESIGN NOTE: these "assumes a compositor" crashes are inherent to InitializeNonCompositing.
The blink-supported offscreen path is InitializeCompositing + a software LayerTreeFrameSink
(what SimTest does) — switching to it would avoid this class of patch but is bigger work.
For now: carry minimal guards as patches/ and keep the simpler non-compositing widget.

### ✅ react.dev FIXED — image.decode() re-entrancy (2026-06-23 15:18)
ROOT CAUSE (nailed by isolating: SVG/data decode OK, PNG/raster decode CRASH): for raster
images, ImageLoader::DispatchDecodeRequestsIfComplete's EraseIf lambda calls
frame->ChromeClient().RequestDecode -> WebFrameWidgetImpl::RequestDecode. With no compositor
(no LayerTreeHost) it ran the callback SYNCHRONOUSLY, which re-enters DecodeRequestFinished
and MUTATES decode_requests_ mid-EraseIf -> iterator invalidation -> SIGSEGV. (In a real
compositor QueueImageDecode is async, so no re-entrancy.)
FIX: patch WebFrameWidgetImpl::RequestDecode's no-LayerTreeHost branch to PostTask the
callback (async) instead of Run()-ing it inline. react.dev renders (docs/demos/react-dev.png).
patches/0001-offscreen-widget-compat.patch now = scroll-handler null-guard + decode-async
(both in web_frame_widget_impl.cc; reverted the chrome_client_impl guess).
NOTE: still consider InitializeCompositing long-term, but the per-crash guards are small,
well-understood, and the offscreen widget otherwise works across ~19/20 hard sites.

### (superseded) BROAD SWEEP ~18/20 + react.dev known crash + DESIGN DECISION POINT (2026-06-23 15:08)
Hard batch renders: github, stackoverflow, web.dev, vuejs, tailwind, apple,
developer.chrome, go.dev (+ the prior 10). cloudflare = slow (sync fetch, not a crash —
async curl-multi is the fix). KNOWN CRASH: **react.dev** — calls image.decode(); SIGSEGV
at null+0x8 in ImageLoader::DispatchDecodeRequestsIfComplete's EraseIf lambda (deeper than
the widget-null I guarded — likely a null Image*/detached frame in the decode path). Added
a defensive guard in ChromeClientImpl::RequestDecode (patch) but it's NOT the fix.
patches/ now = 0001-offscreen-widget-compat.patch (scroll-handler + request-decode guards).
>>> DESIGN DECISION: this is the 3rd "assumes a compositor" crash. The non-compositing
widget (InitializeNonCompositing) is the root fragility. The blink-SUPPORTED offscreen path
is InitializeCompositing + a software cc::LayerTreeFrameSink (what SimTest/frame_test_helpers
use). Switching to it would (a) give a real LayerTreeHost so scroll/decode/etc. "just work",
(b) eliminate the growing patches/ list, (c) match the path blink actually tests. COST:
implement the software LayerTreeFrameSink (mb_sw_frame_sink, scaffolded), InitializeCompositing
wiring + LayerTreeSettings, and keep the GetPaintRecord readback (still works post-composite).
RECOMMENDED as the next big robustness investment instead of more per-crash guards.

### ✅ CJK / i18n WORKS (2026-06-23 15:30) — no code change needed
ja.wikipedia.org renders full Japanese text correctly (docs/demos/japanese-wikipedia.png) —
Blink font fallback finds macOS system CJK fonts (PingFang/Hiragino) via CoreText
automatically; the Latin SetStandardFontFamily etc. don't block fallback. bbc.com OK too.
NOTE: baidu.com renders blank but that's NOT a bug — it serves a 227-byte redirect/JS stub
to our UA (no content to render). To render baidu-style sites would need cookie/redirect
handling + heavier JS; out of scope for a screenshot tool. No fix this tick (validation only).

### ✅ FORM CONTROLS + THEME (2026-06-23 15:38) — no code change needed
docs/demos/form-controls.png: text input, checkbox/radio (checked, blue), button (native
gradient), select + dropdown arrow, textarea, range slider, progress bar — all render with
correct native theming (LayoutTheme -> skia default theme). No crash, no fix. Interactive
form UIs work (display only; actual input event handling is P3/未).

### ✅ INPUT EVENTS — engine is now DRIVABLE (2026-06-23 15:48)
mbSendMouseClick(view,x,y) synthesizes WebMouseEvent down+up and dispatches via
static_cast<WebFrameWidgetImpl*>(widget_)->HandleInputEvent(WebCoalescedInputEvent(e,
ui::LatencyInfo())). Verified: suite case 9 clicks a button -> onclick fires -> DOM mutated
(confirmed via mbEvalJS). NOTE: paint once before clicking so layout is current for the
hit-test. Suite now 9/9. This crosses from "renders" to "renders + responds to input".
NEXT interactivity: scroll/wheel, mouse move/hover.
- ✅ KEYBOARD (2026-06-23 15:56): mbSendText(view, utf8) types ASCII via per-char
  kRawKeyDown+kChar(text[0])+kKeyUp through HandleInputEvent. Suite case 10: focus an
  <input>, type "hi there", verify .value via mbEvalJS. 10/10. (ASCII only; UTF-8 decode TODO.)
  => click + type = forms are fillable/submittable. Engine is interactively drivable.
- ✅ SCROLL (2026-06-23 16:?): mbSendScroll(view,x,y,dx,dy) — dy>0 scrolls down. Suite case
  11: tall page, scroll 400px, assert window.scrollY>0 (== 400). **Architectural finding:**
  a SYNTHETIC gesture/wheel scroll has NO valid path in a non-compositing widget — modern
  Blink routes scroll gestures through the compositor input pipeline and
  WebFrameWidgetImpl::HandleGestureEvent has CHECK(!event.IsScrollEvent()) (web_frame_widget_impl.cc:1205),
  so injecting kGestureScrollUpdate via HandleInputEvent crashes. Resolution: scroll the
  layout viewport PROGRAMMATICALLY on the main thread via LocalDOMWindow::scrollByForTesting
  (moves viewport, updates scrollY, fires 'scroll' event) — the headless-correct behavior.
  (x,y) accepted for API symmetry but unused; the document viewport is the target. This is
  another instance of the "non-compositing widget hits compositor-assumption paths" theme —
  but solved without a blink patch, by using the main-thread scroll API.

- ✅ MOUSE MOVE / HOVER (2026-06-23 16:?): mbSendMouseMove(view,x,y) dispatches a
  kMouseMove WebMouseEvent (kNoButton) via HandleInputEvent. Hit-test + :hover/:active
  recalc runs main-thread inside the event handler, so hover state updates and
  mouseover/mousemove fire without a compositor (same path as click). Suite case 12: hover
  a div -> onmouseover sets window.__h -> verified via mbEvalJS. 12/12. Mouse input now
  complete (click + move/hover). Matters for capturing real pages: hover menus, tooltips.
- ✅ UTF-8 TYPING (2026-06-23 16:?): mbSendText now decodes UTF-8 -> UTF-16 via
  base::UTF8ToUTF16 and types by code point (surrogate pairs travel together in one kChar
  text[]). Removed the ASCII-only limitation. Suite case 10b: type "café日本😀", assert
  value.length==8 and codePointAt(4)==26085 (日). 13/13. **Interactivity surface (click,
  move/hover, UTF-8 type, scroll) is now COMPLETE.** Next focus: shift to robustness sweep
  or the on-screen window / Windows port roadmap.

- ✅ LOAD-FAILURE VISIBILITY (2026-06-23 16:?): robustness sweep found that when a
  main-document fetch fails (DNS/TLS/HTTP error or network throttling — reproduced by
  blasting 8 domains back-to-back), the loader correctly calls DidFail and Blink commits a
  near-empty document, but mb_shot silently wrote a blank PNG and still printed OK / exited
  0. Fix (mb_shot.cc): after an http(s) load, query document.documentElement.outerHTML.length
  via mbEvalJS; a failed load yields ~39 bytes ("<html><head></head><body></body></html>")
  vs thousands for a real page. Below 512 → print a WARNING and exit non-zero. Verified:
  bad host -> FAILED rc=1; example.com -> OK rc=0. NOT a render regression — individual
  renders are fine (Wikipedia 275KB, HN 230KB PNGs); the blank batch was transient network.
  Possible follow-up: thread a real load-status through the C ABI (mbLoadURL is void), and/or
  curl retry/backoff, so transient failures recover instead of just being reported.

- ✅ FETCH RETRY/BACKOFF (2026-06-23 16:?): direct follow-up to the load-failure-visibility
  tick — now transient failures RECOVER, not just get reported. FetchHttp retries up to 3
  attempts with linear backoff (250ms, 500ms) but ONLY for transient causes: network-layer
  curl errors (resolve/connect/timeout/TLS/recv/send/got-nothing/partial) and server
  backpressure (429, 5xx). Deterministic answers (404/403/etc.) are NOT retried. Added
  CURLOPT_CONNECTTIMEOUT=15 to bound a dead host. Verified: bad host retries 2x then fails;
  example.com succeeds first try (no added latency on the happy path). The sleep is on the
  main thread, consistent with the existing synchronous-fetch render model (the fetch
  already blocks the main thread). 13/13 suite unaffected.

- ✅ BURST-BLANK ROOT-CAUSE + TWO FIXES (2026-06-23 16:?): re-ran the 8-domain burst; still
  intermittently blank. Instrumented the main-doc path ([mb_webview] main-doc ok=/bytes=
  under MB_VERBOSE). Findings: (a) individual renders are perfect (go.dev ok=1 64185 bytes,
  6/6); the blanks are TRANSIENT network throttling from hammering across ticks. (b) the
  failure shape is rc==CURLE_OK + http 200 but EMPTY body → FetchHttp returned false with NO
  retry (not in the transient set). FIX 1: treat empty-body-on-success as retryable (it's
  exactly a throttled/half-open connection). (c) LATENT BUG found while reading the path:
  LoadURL committed body.c_str() — truncating any document at its first NUL byte. FIX 2:
  funnel LoadHTML + file + http through CommitHtml(data,len,base) using the full byte length
  (base::span(data,len)); no more C-string truncation. Regression test = smoke case 13
  (file with embedded NUL, assert post-NUL element parses). rust-lang is a 301 (we follow),
  stackoverflow 403 anti-bot (genuine, not our bug). 14/14 suite.

- ✅ FULL-PAGE SCREENSHOT (2026-06-23 16:?): mb_shot now takes `--full` — after load it
  queries max(documentElement.scrollHeight, body.scrollHeight), mbResize(view, w, that)
  (capped 20000px so infinite-scroll can't OOM), then shoots at the full height. Verified:
  go.dev viewport 1200x900 vs --full 1200x3969 (whole page); deterministic file:// tall page
  800x600 vs 800x2000. Engine-level mechanism locked by smoke case 14: load blue(1000)+
  green(200) stack, mbResize to 1200 tall, paint, assert pixel at y=1100 is green (i.e.
  resize -> reflow -> paint captures below-the-fold). 15/15. This is the #1 product feature
  of a headless screenshot tool (cf. Puppeteer fullPage) and it builds on the scroll/viewport
  work. Flag parsing also generalized (flags filtered from positionals).

- ✅ HIDPI / DEVICE SCALE FACTOR (2026-06-23 16:?): mbSetDeviceScaleFactor(view, N) +
  mb_shot --scale N. The compositor path SetZoomFactorForDeviceScaleFactor DCHECKs
  does_composite_ (false for us) — blocked. Used the DevTools override route instead:
  Page::SetInspectorDeviceScaleFactorOverride(N) (DevicePixelRatio = override * zoom, so DPR
  reports N WITHOUT zooming layout), + Document::MediaQueryAffectingValueChanged(kOther) so
  resolution MQs / srcset re-evaluate, + canvas.scale(N,N) in PaintInto so skia re-rasters
  glyphs/vectors crisply into the (logical*N) bitmap. Verified: smoke 16 devicePixelRatio==2,
  smoke 17 @media(min-resolution:1.5dppx) matches; mb_shot --scale 2 of 400x300 -> 800x600;
  --full --scale 2 of go.dev -> 2400x7938 (flags compose). 17/17. Another "non-compositing
  hits compositor-assumption API" solved WITHOUT a blink patch, via the override route.

- ✅ USER-AGENT (2026-06-23 16:?): fixed a real gap — navigator.userAgent was EMPTY (base
  Platform::UserAgent() returns ""), so UA-sniffing sites could misbehave. Now MbFrameClient
  overrides UserAgentOverride() to return a realistic M150 desktop-Chrome default (shared
  with the libcurl fetch so DOM + network agree), and mbSetUserAgent(view, ua) overrides it
  for navigator.userAgent + main-doc fetch + subresource fetches (the UA is threaded into
  MbURLLoader at creation and into MbFetchUrl/FetchHttp). Default UA lives in one place
  (MbDefaultUserAgent(), base::NoDestructor to satisfy -Wexit-time-destructors). Set before
  navigating. Smoke 18: default contains "Mozilla"; smoke 19: override "MiniblinkBot/9.9
  (test)" reflected in navigator.userAgent. 19/19. go.dev still renders with the new default.

- ✅ CLIP / ELEMENT SCREENSHOT (2026-06-23 16:?): mbSavePngRect + mbPaintRectToBitmap, and
  mb_shot --clip x,y,w,h / --selector CSS. PaintInto gained an (origin_x,origin_y) shift:
  after the dsf scale, canvas.translate(-x,-y) so the logical (x,y) lands at the bitmap
  origin and a (w x h) bitmap holds just that region. --selector evals
  querySelector(...).getBoundingClientRect()+scroll to get the page-coords box; both first
  grow the view to full-page height so below-the-fold regions are laid out + painted.
  Verified: --selector '#card' -> 300x180 at (20,140); --clip 0,0,200,100 -> 200x100;
  --selector --scale 2 -> 600x360 (composes with HiDPI); no-match -> message + rc=1. Smoke
  20: green box at (50,60,100,40), clip exactly to it, assert all-green. 20/20.

- ✅ TRANSPARENT BACKGROUND / omitBackground (2026-06-23 16:?): mbSetTransparentBackground +
  mb_shot --transparent. The compositor API SetBaseBackgroundColorOverrideTransparent
  DCHECKs does_composite_ (blocked); used SetBaseBackgroundColorOverrideForInspector(
  SK_ColorTRANSPARENT) instead — no composite check, feeds the same BaseBackgroundColor()
  which returns transparent when the inspector override is set. PaintInto also clears to
  SK_ColorTRANSPARENT so unpainted areas keep alpha 0. Verified: opaque PNG hasAlpha=no,
  --transparent PNG hasAlpha=yes; smoke 21: opaque green box -> inside alpha 255 + green,
  empty area alpha 0. 21/21. THIRD compositor-assumption API routed via the inspector
  override (after DSF and... well, base color) — pattern: when does_composite_ blocks an
  override, the DevTools/inspector override twin usually doesn't.

- ✅ WAIT-FOR-CONTENT (2026-06-23 17:?): mbWait(ms) + mbWaitForSelector(css,timeout) +
  mb_shot --wait-ms / --wait-selector. Gap: PaintInto's RunUntilIdle drains READY tasks but
  does NOT advance to a not-yet-due delayed timer, so setTimeout/async-rendered content was
  missed. WaitMs/WaitForSelector interleave real PlatformThread::Sleep(10ms) + RunUntilIdle
  + lifecycle so timers fire as wall-clock passes; WaitForSelector polls
  document.querySelector(css) until match or timeout. Verified: smoke 22 — a 300ms
  setTimeout-injected #ready is caught (returns 1, textContent correct), #never times out
  (0); mb_shot --wait-selector '#late' then --selector '#late' captured the delayed element
  150x60. 23/23. NOTE dropped a flaky "absent immediately after load" assertion — the load's
  own 20x RunUntilIdle spans enough wall-clock that a short timer may already fire; not a
  guarantee worth asserting.

- ✅ DOM STORAGE (2026-06-23 17:?): probed and found localStorage threw TypeError (gap —
  SPAs need it). Two fixes: (1) settings->SetLocalStorageEnabled(true) — it's off by default,
  which made GetOrCreateLocalStorage return null; with it on, localStorage round-trips
  IN-MEMORY even though our broker drops the DomStorage mojo backend (CachedStorageArea
  serves reads from its local cache for the page lifetime). (2) sessionStorage also threw —
  it needs a StorageNamespace attached to the Page; call
  StorageNamespace::ProvideSessionStorageNamespaceTo(page, <36-char id>) in MbWebView::Create
  after DidAttachLocalMainFrame. Verified: smoke 23/24 localStorage 'v42' + sessionStorage
  's7' round-trip. 25/25. Storage is in-memory only (no disk persistence) — correct for a
  headless/automation host. Methodology: probe-an-assumed-feature, fix if the guard fails.

- ✅ requestAnimationFrame (2026-06-23 17:?): probed a batch of web APIs — all present
  (matchMedia, crypto.getRandomValues, URL, URLSearchParams, IntersectionObserver,
  ResizeObserver, structuredClone, TextEncoder) EXCEPT rAF callbacks never fired (compositor
  normally drives them via BeginMainFrame; we have none). Fix: ServiceAnimations() calls
  web_view_->GetPage()->Animator().ServiceScriptedAnimations(now), invoked each round of
  PaintInto and each WaitMs iteration so rAF fires and rAF-chains advance. Verified: smoke 25
  single rAF mutates DOM; smoke 26 a 2-frame rAF loop reaches __n==2. 27/27. go.dev still
  renders (no regression). (pushState SecurityError in the probe was just about:blank's opaque
  origin — fine on real pages, not a gap.) Another compositor-assumption gap closed by calling
  the page-level service directly, no patch.

- ✅ INTERSECTIONOBSERVER DELIVERY (2026-06-23 17:?): probed observer/event delivery —
  MutationObserver fires, load/DOMContentLoaded fire, pushState works on file://, animate/
  document.fonts present, but IntersectionObserver callbacks NEVER delivered (lazy-load,
  infinite scroll, viewability all depend on it). Cause: RunIntersectionObserverSteps gates
  on ShouldThrottleRendering() (local_frame_view.cc:1082) and our offscreen frame reads as
  throttled, so IO is skipped in the lifecycle. Fix: ServiceAnimations() now also calls
  frame->View()->ForceUpdateViewportIntersections() (DisallowThrottlingScope — blink's own
  hidden-frame path); the queued notifications deliver via the loop pump. Verified smoke 27/28:
  MutationObserver + IntersectionObserver(isIntersecting) both fire. 29/29, go.dev unaffected.
  (pushState earlier showed SecurityError only because about:blank is an opaque origin.)

- ✅ ANIMATION/NETWORK PROBE — all pass, hardened (2026-06-23 17:?): probed WAAPI finished
  promise, ResizeObserver delivery, dynamic Image().onload, sync XMLHttpRequest(data:), fetch
  — ALL WORK (no new gap). WAAPI advancing confirms the ServiceScriptedAnimations clock-drive
  from the rAF tick is doing its job. Converted the probe to 3 regression guards (smoke 29-31)
  so the animation-clock + observer + sync-XHR paths can't silently regress. 32/32. Web
  platform now broadly complete: storage, rAF, all observers, animations, XHR/fetch, events.
  Next probe categories that MIGHT find gaps: WebGL (no GPU — big lift, skip), service
  worker, IndexedDB, web fonts over network, CSS @container/@layer. Or pivot to networking
  (cookies/headers) / the wke C API / Cocoa window.

- ✅ CONSOLE CAPTURE + IndexedDB probe (2026-06-23 17:?): implemented console capture —
  MbFrameClient overrides DidAddMessageToConsole (still on WebLocalFrameClient) and buffers
  "level: text"; MbWebView::DrainConsole + mbDrainConsole(out,cap) C ABI + mb_shot --console.
  Verified smoke 30 (log/warn/error captured, buffer clears after drain) and mb_shot --console
  prints them. Genuinely useful for automation/debugging. PROBE4: IndexedDB open() stays
  'pending' forever (no backend responds) — a real gap, but fixing needs a full in-process
  IndexedDB mojo backend (large); documented as roadmap, not attempted. 34/34.
  [CORRECTION 2026-06-23: re-probed — open() does NOT hang. See the IndexedDB-graceful entry
  below; it fails fast via onerror. The 'pending forever' read here was wrong.]

- ✅ COOKIES (2026-06-23 17:?): added a process-wide in-memory cookie jar in the loader —
  CookieShare() lazily makes a CURLSH(CURL_LOCK_DATA_COOKIE); every FetchHttp handle sets
  CURLOPT_COOKIEFILE "" (enable engine) + CURLOPT_SHARE. So Set-Cookie is honored across a
  redirect chain (consent/login flows) AND across separate requests (main doc + subresources,
  successive navigations) — the host now behaves like one browsing session. Verified via
  httpbin /cookies/set (302->/cookies) then a separate /cookies request, both echo mbck=val99.
  Smoke 31 asserts this when httpbin is reachable, else SKIPs (network flakiness must not fail
  the suite) — confirmed both a SKIP run and a PASS run. Single-threaded, so no share lock
  callbacks. No on-disk persistence (session-scoped) — right for headless. 34/35 (cookie case
  conditional). Note: this is network-side cookies; document.cookie (JS) is separate and not
  yet wired to this jar.

- ✅ REQUEST HEADERS + Accept-Language (2026-06-23 17:?): mbSetExtraHeaders(view, "Name: Value\n
  ...") + mb_shot --header "N: V" (repeatable). Threaded like UA: frame client holds them,
  passes to MbURLLoader (subresources) and LoadURL passes to the main-doc MbFetchUrl. FetchHttp
  builds a curl_slist from the newline-separated lines and ALWAYS adds a default
  Accept-Language: en-US,en;q=0.9 unless the host supplied one (case-insensitive check) — so
  sites serve localized content (none was sent before). slist freed after each fetch. Verified
  smoke 32 (network, graceful SKIP): httpbin /headers echoes X-Mb-Test: probe-42 + Accept-Language
  — got a real PASS. 35/36 (two conditional network cases: cookies + headers). This subsumes
  many needs (auth tokens, content negotiation, custom API headers).

- ✅ mb_shot --text + document.cookie INVESTIGATION (2026-06-23 18:?): shipped --text (dump
  document.body.innerText to stdout — mb_shot doubles as a scraper; verified on a known page).
  Probed document.cookie (JS): write is a silent no-op, read returns '' (no throw). ROOT CAUSE
  mapped: CookieJar uses the FRAME's BrowserInterfaceBroker (document_->GetFrame()->
  GetBrowserInterfaceBroker(), cookie_jar.cc:305) to bind network::mojom::RestrictedCookieManager
  — NOT Platform's broker (where MbEmptyBroker lives). We pass NullRemote for the main-frame
  broker in CreateMainFrame, and frame_test_helpers (our model) does the same + never wires
  cookies; the broker is also reset on each navigation commit. To fix: implement in-process
  BrowserInterfaceBroker (1 method: GetInterface) routing RestrictedCookieManager (6 methods,
  back with an in-memory per-origin store) + wire a real PendingRemote into CreateMainFrame AND
  likely WebNavigationParams (broker-reset-on-commit). Deferred as a scoped roadmap gap (HTTP
  cookies already cover most sites). 35/36.

- ✅ document.cookie (JS) NOW WORKS (2026-06-23 18:?): implemented the deferred feature from
  last tick. New mb_frame_broker.{h,cc}: MbBrowserInterfaceBroker (GetInterface routes
  network::mojom::blink::RestrictedCookieManager, drops the rest) + MbCookieManager (the 6
  RCM methods, backed by a process-global per-origin in-memory store; SetCookieFromString
  parses name=value + max-age=0/expired deletion, GetCookiesString serializes "n=v; ..."). 
  Wired a real PendingRemote via MakeFrameInterfaceBroker() into CreateMainFrame (was
  NullRemote). The broker SURVIVES navigation commit — the feared broker-reset-on-commit did
  NOT nullify it (smoke 33 loads a real file:// doc then round-trips document.cookie='a=1'/'b=2'
  -> "a=1; b=2"). 36/36. Origin-scoped, session-only (no disk). NOTE: this in-process RCM is
  NOT bridged to the curl HTTP jar — JS cookies and network cookies are separate stores for now
  (acceptable; unifying them is a future nicety).

- ✅ SUITE SPEED FIX + advanced-API probe (2026-06-23 18:?): the network smoke cases
  (cookies, headers) each cost ~45s on an unreachable host (15s connect-timeout x 3 retries x
  multiple loads), making the default suite hang ~2min offline. Gated both behind
  MB_NET_TESTS=1 — default run is now fast + offline-deterministic (35 passed), network checks
  opt-in. Probed advanced APIs (wasm/SubtleCrypto/Blob/FileReader/performance) but the probe
  itself HUNG the binary >2min: RunJS uses an unbounded base::RunLoop().RunUntilIdle(), so a
  script that keeps the task queue non-idle (a tight setTimeout/async loop, or apparently one
  of these APIs) spins forever. Removed the probe. FOLLOW-UPS noted: (a) bound RunJS's
  RunUntilIdle with a deadline like WaitMs does (latent hang risk on timer-loop pages);
  (b) re-probe wasm/SubtleCrypto/Blob one-at-a-time to find which hangs. No engine change beyond
  the test gating this tick.

- ✅ RunJS bounded + Blob-hang ROOT-CAUSED (2026-06-23 18:?): RunJS now caps its post-script
  settle — base::RunLoop with QuitWhenIdleClosure() (fast common path) OR a 250ms QuitClosure
  (hard cap), instead of a bare unbounded RunUntilIdle. 35/35. Re-probed the advanced APIs
  one-at-a-time: WebAssembly = ok. **Blob/URL.createObjectURL/FileReader HANGS the main thread**
  — and bounded RunJS does NOT save it, proving the hang is a SYNCHRONOUS mojo block (the blob
  registry / BlobURLStore interface is dropped by our broker; a [Sync] call waits forever),
  not a RunUntilIdle spin. SubtleCrypto untested (probe hung before reaching it). HIGH-PRIORITY
  GAP: real pages use createObjectURL/FileReader (image previews, downloads, object URLs) and
  would hang the host — needs in-process Blob mojo services (BlobRegistry + BlobURLStore), like
  we did for MimeRegistry/RestrictedCookieManager. Probe removed (it hangs the suite). Next:
  implement the blob services, or at least make the broker reply-and-drop so sync blob calls
  fail fast instead of hanging.

- ⚠️ BLOB HANG — fix ATTEMPTED, reverted (2026-06-23 19:?): root cause confirmed —
  URL.createObjectURL → BlobURLStore.Register is [Sync] (blob_url_store.mojom:20); for a
  FRAME, BlobURLStore comes via frame->GetRemoteNavigationAssociatedInterfaces() (an
  ASSOCIATED interface from WebLocalFrameClient::GetRemoteNavigationAssociatedInterfaces),
  NOT the BrowserInterfaceBroker. Unserviced -> main-thread sync deadlock. ATTEMPT (reverted):
  mirror TestWebFrameClient — MbFrameClient holds a local blink::AssociatedInterfaceProvider
  (task-runner ctor) + OverrideBinderForTesting(BlobURLStore::Name_, &MbBindBlobURLStore),
  MbBlobURLStore answers Register() (acknowledge). It COMPILED but STILL HUNG (suite stalls at
  the Blob case, 2min). So servicing isn't happening during the sync wait — likely the
  self-owned associated receiver runs on a different sequence than the sync-wait pumps, or the
  override binder isn't invoked inline. Reverted to keep the suite green (35/35) + bounded RunJS
  intact. NEXT DEBUG: add MB_VERBOSE prints in MbBindBlobURLStore + MbBlobURLStore::Register to
  see if the binder/receiver are hit; check the receiver's task runner vs the file-reading
  runner PublicURLManager binds frame_url_store_ on; consider binding the receiver on the same
  task runner. Blob/createObjectURL/FileReader remain a known host-freeze gap until then.

- ✅ POST-FEATURE-RUN HEALTH CHECK (2026-06-23 19:?): after the long feature run (storage,
  rAF, observers, cookies, headers, document.cookie, bounded RunJS), verified no rendering
  regression. Offline rich page (grid+gradient+shadow+transform+CJK+canvas) -> 14.7KB PNG OK;
  example.com -> 16.5KB PNG; 4/4 repeated single fetches ok=1; raw curl 5/5 = 200. A burst
  sweep showed sporadic FAILED/3902-byte blanks — diagnosed as TRANSIENT network blips (raw
  curl works seconds later), NOT a regression, and mb_shot correctly reports them as FAILED
  (empty-doc detection) rather than silent success. Engine + fetch healthy. (Deferred another
  Blob debug cycle — understood LocalProvider::GetInterface routes the binder through an async
  provider pipe, so whether the [Sync] Register pump services it is the open question; left as
  documented gap to avoid burning ticks on 2-min-hang iterations.)

- ✅ INIT SCRIPTS / evaluateOnNewDocument (2026-06-23 19:?): mbSetInitScript(view, js) runs a
  host script on every new document BEFORE its own scripts. MbFrameClient overrides
  RunScriptsAtDocumentElementAvailable() (fires at <html> creation, allowed to run JS, before
  inline <script>s) -> owner_->RunDocumentStartScript() -> main_frame_->ExecuteScript(init).
  Verified smoke 34: init sets window.__early, the page's inline script reads it into
  __pageSaw == 'injected' (proving inject-before-page-scripts ordering). 36/36. Useful for
  automation: stub/override APIs, set globals, install a harness. (Moved on from the Blob
  associated-interface hang — still a documented gap.)

- 🔒 BLOB HANG — DEFINITIVE conclusion (2026-06-23 19:?): not a quick fix. createObjectURL
  does BlobURLStore.Register [Sync] on the main thread IMMEDIATELY after the async GetInterface
  that would bind the store. Mojo's sync-wait dispatches sync responses but QUEUES async
  messages (like the LocalProvider's GetAssociatedInterface that triggers our binder) until
  the sync call returns — so an in-process, SAME-THREAD servicer can never run while the main
  thread is blocked => deadlock. In production it works only because the servicer is a separate
  process/thread. PROPER FIX = run the frame's associated-interface servicing (BlobURLStore et
  al.) on a DEDICATED thread, like the browser process does — an architectural addition, not a
  shim. Until then, pages calling createObjectURL/FileReader during script will freeze the host.
  Marking settled; not to be re-attempted as a quick fix.

- ✅ COOKIE BRIDGE JS->HTTP (2026-06-23 19:?): document.cookie set in JS now also lands in the
  shared libcurl jar, so a JS-set (e.g. auth) cookie is sent on later fetches/subresources.
  MbAddCookieToJar(url, cookie) injects via a throwaway curl handle (CURLOPT_URL=origin,
  COOKIEFILE="" engine, SHARE=CookieShare(), COOKIELIST="Set-Cookie: <cookie>"); MbCookieManager
  ::SetCookieFromString calls it after storing in the JS map. No-op for non-http(s). Low-risk +
  additive (only adds to the jar; failure = JS cookie just not sent, no regression — default
  suite 36/36 confirms). Smoke 33 (network-gated) verifies end-to-end when httpbin is reachable;
  SKIPPED this run (httpbin down). Reverse direction (network Set-Cookie -> document.cookie read)
  is a noted follow-up.

- ✅ mb_shot --html (2026-06-23 19:?): dumps the rendered (post-JS) DOM via
  document.documentElement.outerHTML (2 MiB buffer) — scraping complement to --text for SPAs
  whose fetched source is near-empty. Verified: a JS-injected <p id=dyn>JS-ADDED</p> appears
  in the output (proves post-render serialization, not raw source). Also confirmed the network
  stack handles a real httpbin 503 correctly (curl=0 http=503 -> retried as transient -> on
  persistent 503 reported FAILED, not a silent blank); the cookie-bridge/jar/header network
  tests SKIP cleanly while httpbin is 503'ing (verify when it recovers). 36/36.

- ✅ NETWORK TESTS VERIFIED LOCALLY + cookie bridge CONFIRMED (2026-06-23 19:?): httpbin was
  503'ing, so made the 3 network smoke cases host-configurable via MB_NET_HOST (default
  httpbin.org) and added test/echo_server.py (a tiny httpbin-shaped local server:
  /cookies/set 302->/cookies, /cookies, /headers). Ran MB_NET_TESTS=1
  MB_NET_HOST=http://127.0.0.1:8899 -> ALL 3 PASS: cookie jar (Set-Cookie across redirect +
  shared across requests), request headers (custom + default Accept-Language), and the
  COOKIE BRIDGE (document.cookie -> HTTP jar -> sent on next request) — the previously
  unverified feature, now confirmed end-to-end. 39/39 with network on (36 default). Network
  tests no longer hostage to httpbin uptime.

- ⚠️ REVERSE COOKIE BRIDGE — attempted, HANGS, reverted (2026-06-23 19:?): tried to unify
  http cookies on the curl jar (GetCookiesString reads the jar via MbGetCookiesForUrl /
  CURLINFO_COOKIELIST; SetCookieFromString writes jar-only for http). It COMPILED and the
  default suite stayed 36/36, but the network suite HUNG at the reverse-bridge case (cut off
  at 60s and 100s watchdogs) — reading the curl jar from inside the [Sync] GetCookiesString on
  the main thread stalls (same family as the Blob same-thread sync issue). Reverted the 4
  code/test files to HEAD; the FORWARD bridge (JS document.cookie -> curl jar) stays and was
  RE-VERIFIED PASS this tick against the local echo server (also cookie jar + headers PASS).
  Reverse direction (network Set-Cookie -> document.cookie) left as a known gap. KEPT:
  echo_server.py upgraded to ThreadingHTTPServer + listen log (more robust local verification).
  PROCESS LESSON: build.sh runs mb_smoke at the end; when a run hangs (network/blob) the 2-min
  Bash timeout kills the foreground grep but leaves mb_smoke detached -> they piled up (user
  flagged 5+ stray shells). Cleaned all. Going forward: run mb_smoke directly with a bounded
  self-kill watchdog + explicit pkill, never via repeated timing-out build pipelines.

- ✅ JPEG OUTPUT (2026-06-23 20:?): SavePng/SavePngRect now pick the image codec by the output
  path's extension via a shared EncodeBitmapToPath helper — .jpg/.jpeg -> gfx::JPEGCodec::Encode
  (quality 90, //ui/gfx/codec already dep'd), else PNG. mbSavePng/mbSavePngRect + mb_shot infer
  format from the extension (no new flag). Verified: out.jpg = FFD8 magic, 3.8KB (vs PNG 14KB),
  `file` confirms valid JPEG 400x300; out.png unchanged. Clean low-risk tick; 36/36; no stray
  processes (ran the build directly, checked survivors). Used the disciplined process pattern
  after last tick's leak.

- ✅ ISOLATED-WORLD EVAL (2026-06-23 20:?): mbEvalJSIsolated(view, js, out, cap) runs script in
  a dedicated isolated world (id 1) via WebLocalFrame::ExecuteScriptInIsolatedWorldAndReturnValue
  + BackForwardCacheAware::kAllow; result stringified through the main-world context (fine for
  primitives). Content-script semantics: own JS globals, shared DOM. Smoke 35 (3 asserts):
  isolated eval can't see main-world window.__main (undefined), its window.__iso doesn't leak to
  main, and a DOM attr it sets IS visible in main. 39/39, no survivors. Useful for automation
  that must not collide with or be observed by page script. (Reverse cookie bridge + Blob remain
  the known gaps; both are same-thread [Sync] mojo issues.)

- ✅ DISABLE IMAGE LOADING (2026-06-23 20:?): mbSetLoadImages(view, 0) + mb_shot --no-images
  for faster text/HTML scraping. settings->SetLoadsImagesAutomatically(enabled). FINDING: this
  gates NETWORK image fetches only — inline data: images decode regardless (my first test used
  a data: GIF and showed on=1/off=1, exposing this). Re-tested correctly with a SERVED image:
  added /img to echo_server.py; network smoke case loads <img src=$host/img> on (naturalWidth>0)
  vs off (0) -> PASS (on=1 off=0). 43/43 with MB_NET_TESTS against the local server; 39/39
  default. No survivors (bounded self-killing run). Lesson reinforced: pick test image type to
  match what the setting actually gates.

- ✅ DARK MODE (2026-06-23 20:?): mbSetDarkMode(view, 1) + mb_shot --dark emulates
  prefers-color-scheme: dark via settings->SetPreferredColorScheme(mojom::PreferredColorScheme::
  kDark/kLight). Set before load. Smoke 36: matchMedia('(prefers-color-scheme:dark)').matches
  == true AND an @media(prefers-color-scheme:dark) CSS rule applies (color rgb(2,2,2)). 40/40,
  verified first try, no survivors. DevTools-style dark-theme capture.

- ✅ LOCALE / navigator.language (2026-06-23 20:?): mbSetLocale(view,"fr-FR,fr,en") + mb_shot
  --lang sets navigator.language/languages via Page->GetSettings().SetAcceptLanguages (core
  blink::Settings field acceptLanguages; Navigator::GetAcceptLanguages reads it). Set before
  load so the new document's navigator reads it fresh (Page::AcceptLanguagesChanged is private —
  not needed pre-load). Smoke 37: navigator.language=='fr-FR', navigator.languages=='fr-FR,fr,en'.
  41/41, no survivors. (DOM-side locale; the network Accept-Language header is separately the
  curl default / mbSetExtraHeaders.)

- ✅ TIMEZONE OVERRIDE (2026-06-23 20:?): mbSetTimezone(view,"America/New_York") + mb_shot --tz
  via icu::TimeZone::adoptDefault(createTimeZone(id)) + isolate->DateTimeConfigurationChange
  Notification(kSkip). KEY FIX: kSkip (use the ICU default we set), NOT kRedetect (which re-reads
  the host OS zone and clobbers it — first attempt showed Asia/Shanghai). Process-global.
  Smoke 38: Intl.DateTimeFormat().resolvedOptions().timeZone=='America/New_York' AND
  new Date(1609459200000).getHours()==19 (2021-01-01T00Z -> 2020-12-31 19:00 EST). 43/43, no
  survivors. DevTools-style emulation now: dark mode, locale, timezone, HiDPI, viewport, UA.

- ✅ CONSOLIDATION / FULL-SUITE VERIFY (2026-06-23 20:?): after a long feature run, confirmed
  no integration regression. Full suite with network on (local echo server, bounded run):
  47/47 (43 default + 4 network), no FAIL/SKIP, no stray processes. Verified all emulation +
  capture flags COMPOSE: `mb_shot --dark --scale 2 --lang ja-JP --tz Asia/Tokyo --full` -> valid
  800x400 PNG, and --text confirms the simultaneous runtime state "ja-JP | Asia/Tokyo | dpr=2".
  Engine + mb_shot healthy across the whole surface. (Known gaps unchanged: Blob sync-deadlock,
  reverse cookie bridge, IndexedDB, on-screen window — all documented above.)

- ✅ PDF EXPORT (2026-06-23 20:?): mbSavePdf(view,path) + mb_shot infers .pdf -> paginated PDF
  via Blink's print path. WebLocalFrame::PrintBegin/GetPageDescription/PrintPage/PrintEnd into
  an SkPDF doc (SkPDF::MakeDocument); each page beginPage(w*0.75,h*0.75 pts), canvas.scale(0.75)
  to map CSS px -> points, cc::SkiaPaintCanvas wraps the page canvas. US Letter (816x1056 css).
  TWO fixes during bring-up: (1) needed cc/paint/skia_paint_canvas.h + SkPDFDocument.h + gfx
  rect_f/size_f includes; (2) DCHECK at pagination_utils.cc:35 — print_scaling_option must be
  kSourceSize; fixed by using the WebPrintParams(gfx::SizeF) ctor (sets it). Verified: a
  60-paragraph doc -> "PDF document, version 1.4, 5 pages"; smoke 39 checks %PDF- header + size.
  44/44, no survivors. Flagship headless feature (Puppeteer page.pdf equivalent).

- ✅ COOKIE EXPORT (2026-06-23 21:?): mbGetCookies(view,url,out,cap) reads the shared curl jar
  for a host ("name=value; ...", non-HttpOnly) so a host can extract a session (e.g. after a
  login flow) for reuse. Re-added MbGetCookiesForUrl to the loader (CURLINFO_COOKIELIST parse;
  this was the helper from the reverted reverse-bridge — but here it's called DIRECTLY by the
  host, not from the [Sync] GetCookiesString, so no deadlock). Smoke 35 (network): set cookies
  via /cookies/set, mbGetCookies returns "mbck=val99; expk=expv". 49/49 with network (44
  default), no survivors. Rounds out the cookie story: set (headers/JS/network) + export.

- ✅ CUTTING-EDGE CSS verified (2026-06-23 21:?): probed the headline M150-vs-M47 CSS features —
  ALL work: :has() selector, native CSS nesting (& .inner), @container queries, color-mix()
  (-> color(srgb 0.5 0.5 0.5)), oklch() (preserved). Locked smoke 35: one document where each
  feature colors an element, asserts :has/nesting/container exact colors + color-mix contains
  0.5. 45/45, no survivors. Demonstrates the whole point of the upgrade (none of these existed
  in the frozen ~2015 M47 engine). On-theme verification + regression guard.

- ✅ WEB COMPONENTS verified (2026-06-23 21:?): Custom Elements v1 + Shadow DOM both work —
  customElements.define + connectedCallback upgrades a created element (textContent='upgraded'),
  attachShadow({open}) + shadowRoot.querySelector returns the shadow content, and light-DOM
  querySelector can't see it (encapsulated). Smoke 36 asserts all three. 46/46, no survivors.
  Another on-theme M150 marker (M47 had only the v0 prototype). Pure-DOM, offline-verifiable.

- ✅ WORKER CRASH HARDENED -> graceful degradation (2026-06-23): the `new Worker(...)` SIGSEGV
  is FIXED. Root cause: `DedicatedWorker`'s ctor calls
  `Platform::CreateDedicatedWorkerHostFactoryClient()`; base Platform returns nullptr, and
  `DedicatedWorker::Start()` then derefs it unconditionally (`factory_client_->CreateWorkerHost`,
  dedicated_worker.cc:312) — a hard null-deref the moment any page spawns a worker. Fix: override
  that Platform method in MbPlatform to return an inert stub (mb_platform.cc
  MbDedicatedWorkerHostFactoryClient): CreateWorkerHost is a no-op (the script-load callback
  never fires, so the worker never runs) and CloneWorkerFetchContext returns null. The main
  thread is NOT blocked — `new Worker` returns a valid object, the page keeps running, the
  worker is simply inert. So a worker-using site DEGRADES (worker doesn't execute) instead of
  crashing the host. Smoke 37 is the regression guard: spawn a Worker, pump, assert the host is
  still alive + scriptable. 47/47, no survivors.
  NOTE: this is hardening, NOT worker support. Workers still don't RUN — full support remains a
  HEAVY item (real WorkerThread/WorkerGlobalScope bring-up: own Platform/scheduler/loader/broker),
  same family as the other browser-process-service gaps (Blob sync-deadlock, IndexedDB). But a
  crash is no longer in that family: the process now survives worker spawns.

- ✅ WORKER FAMILY all crash-safe (2026-06-23, verified): with DedicatedWorker fixed, probed the
  rest. SharedWorker: `new SharedWorker(...)` constructs (typeof object); Connect() is a
  fire-and-forget mojo call our empty broker drops -> inert, NO crash. ServiceWorker:
  `navigator.serviceWorker.register()` rejects cleanly on an unsupported origin (file:// ->
  TypeError), and on a real origin RegisterServiceWorkerInternal has `if(!provider_) return;`
  (service_worker_container.cc) -> with no provider the promise stays pending, NO crash. So the
  ENTIRE worker family (dedicated/shared/service) now degrades gracefully instead of crashing.
  No code change needed for shared/service — already safe; smoke 38 guards the regression
  (spawn both inside try/catch on an opaque origin, assert the host stays scriptable). 48/48.

- ✅ INDEXEDDB fails gracefully via onerror — NOT a hang (2026-06-23, re-probed + corrected): an
  earlier note (PROBE4 above) claimed `indexedDB.open()` hangs 'pending forever'. Re-probed: it
  does NOT. open() returns a request, and `onerror` FIRES promptly. Mechanism: we bind no IDB
  backend, so the frame broker (MbBrowserInterfaceBroker) drops the IDBFactory interface ->
  IDBFactory pulls its connector from GetBrowserInterfaceBroker().GetInterface() (idb_factory.cc
  ~128), the unbound receiver pipe closes, the remote disconnects, and Blink delivers that to the
  request as a clean async error. So IndexedDB is in the SAME graceful-degradation bucket as the
  worker family: non-functional (no storage) but safe — no host hang, no crash, and a page that
  feature-uses IDB gets a normal onerror it can branch on. Smoke 39 guards it (open on a real
  http origin, assert window.__idb=='error' + host scriptable). 49/49.
  Net: of the three documented "browser-process-service" gaps, two (Workers, IndexedDB) are now
  confirmed graceful. Only Blob's [Sync] BlobURLStore.Register remains a true HANG (sync mojo on
  the main thread, no servicing thread) — the last hard hazard in this family.

- ✅ NETWORK/STORAGE APIs crash-safe; WebSocket degrades cleanly (2026-06-23): swept three more
  common backend-dependent APIs. (1) WebSocket: `new WebSocket(wss://...)` constructs, then with
  no network backend fires onerror + onclose — i.e. it reaches a terminal CLOSED state via the
  spec events rather than crashing or hanging, so a site's reconnect logic behaves normally.
  STRONG graceful degradation; smoke 40 guards it (construct on a real origin, assert the close/
  error event fired + host scriptable). (2) Cache API: `caches` exists (typeof object);
  caches.open() stays pending (no backend settles it) — an unsettled promise, NOT a host hang, no
  crash. (3) BroadcastChannel: constructs and postMessage() is a safe no-op (no peer). All three
  exit 0, no survivors. So beyond the worker family, the common network/storage surface is also
  crash/hang-safe; WebSocket in particular fails the "right" way (events, not silence). 50/50.

- ✅ FIX: host-driven canvas draws (mbRunJS) no longer crash + Canvas 2D round-trip verified
  (2026-06-23): probing canvas turned up a real bug. mbRunJS/RunDocumentStartScript executed JS
  via main_frame_->ExecuteScript() SYNCHRONOUSLY — outside any scheduler task (the RunLoop was
  created afterward). A canvas draw (CanvasRenderingContext::DidDraw) made outside a task scope is
  unbracketed by WillProcessTask/DidProcessTask, and the very next task trips a FATAL NOTREACHED
  in CanvasPerformanceMonitor (canvas_performance_monitor.cc:181). So any embedder doing the
  natural "mbRunJS('...fillRect...')" then screenshot pattern would crash (exit 134/SIGABRT).
  FIX (mb_webview.cc RunJS): post the script as a TASK and run it inside the nested RunLoop, so it
  is bracketed exactly like a page script; loop.Run() still blocks until it executed, so callers
  still see DOM effects synchronously. Page-script canvas draws were always fine (case 6) — only
  host-injected draws were affected. [CAVEAT below CLOSED 2026-06-23 — see next entry.] Also
  confirmed: Canvas 2D full round-trip
  works offline (getContext('2d') -> fillRect -> getImageData reads back 255,0,0,255 -> toDataURL
  emits data:image/png), so chart/image-processing libs function; WebGL getContext('webgl')
  returns null (no GPU) gracefully, not a crash. Smoke 41 guards all of it. 51/51, no survivors.

- ✅ FIX: ALL host-driven JS now task-bracketed (mbEvalJS/mbEvalJSIsolated too) (2026-06-23):
  closed the caveat from the previous entry. EvalToString and EvalIsolated still ran
  ExecuteScript(...AndReturnValue) SYNCHRONOUSLY, so a draw inside an eval expression
  (mbEvalJS("...fillRect...; getImageData...")) hit the same CanvasPerformanceMonitor NOTREACHED.
  Factored a shared MbWebView::RunInFrameTask(body, settle) helper: posts `body` as a scheduler
  task and runs a nested RunLoop so the script is bracketed by WillProcessTask/DidProcessTask
  exactly like a page script. settle=true keeps RunJS's drain-to-idle/250ms-cap behavior;
  settle=false runs just the one task (Eval's synchronous-read semantics, no extra async
  progress). Eval still returns synchronously — the v8 work writes the stringified result into a
  stack-local std::string, valid because loop.Run() blocks until the body has executed. RunJS,
  EvalToString, EvalIsolated now all funnel through it. Verified: drawing via mbEvalJS AND
  mbEvalJSIsolated no longer crashes (smoke 42/43 draw + read back the pixel in one eval expr),
  and ALL pre-existing Eval-driven cases still pass (no regression from task-wrapping reads).
  So host-driven canvas drawing is fully crash-safe through every JS entry point now. 53/53.

- ✅ FIX: JS dialogs (alert/confirm/prompt) no longer HANG the host (2026-06-23): probed a common
  hazard and found a hard hang. alert()/confirm()/prompt() route through
  ChromeClientImpl::OpenJavaScript{Alert,Confirm,Prompt}Delegate, which make [Sync] mojo calls on
  frame->GetLocalFrameHostRemote() (RunModalAlertDialog/RunModalConfirmDialog/RunModalPromptDialog,
  frame.mojom marks all three [Sync]). With no browser process to service LocalFrameHost, the sync
  call blocks the main thread FOREVER — and pages routinely call these during load, so any such
  page would hang the entire host (confirmed: mb_shot exit 137, watchdog-killed). This is the same
  [Sync]-mojo-to-browser class as the Blob hang. FIX (patches/0002-suppress-js-dialogs.patch, a
  Blink-compat patch in the established patches/ mechanism): the three delegates auto-dismiss —
  headless semantics — instead of making the sync call: alert returns, confirm/prompt return their
  "Cancel" defaults (false / null result). TruncateDialogMessage became unused -> marked
  [[maybe_unused]] (it has -Werror,-Wunused-function). Verified: mb_shot now exits 0 and the page
  runs to completion (alert->undefined, confirm->false, prompt->null, then continues); patch is
  reproducible (git reverse-check OK). Smoke 44 guards it by calling all three INLINE DURING LOAD
  (the realistic hang path) — a regression would hang the whole suite (caught by the watchdog).
  54/54, no survivors. NOTE: this knocks out a second member of the [Sync]-mojo hang family; only
  Blob's [Sync] BlobURLStore.Register remains (and unlike dialogs it needs the call's RESULT, so
  auto-dismiss won't work — it still wants a servicing thread).

- ✅ Completed LocalFrameHost [Sync] coverage + clipboard verified safe (2026-06-23): audited
  frame.mojom for [Sync] methods — exactly four: RunModal{Alert,Confirm,Prompt}Dialog (fixed last
  tick) and RunBeforeUnloadConfirm. Added beforeunload to patches/0002: its delegate
  (OpenBeforeUnloadConfirmPanelDelegate) made the same [Sync] call and would deadlock identically.
  HONEST NOTE: beforeunload is NOT currently reachable — CommitHtml calls
  WebLocalFrameImpl::CommitNavigation() directly, which skips the FrameLoader prompt-to-unload step
  that fires beforeunload — so this is a DEFENSIVE fix completing the [Sync] set, guarding a future
  real-navigation path (not a confirmed live hang like the dialogs were). Delegate now returns true
  (auto-proceed = unload allowed, the safe headless default). Also swept the clipboard, which has
  its own [Sync] reads (ClipboardHost ReadText/IsFormatAvailable/GetSequenceNumber/ReadAvailable
  Types) — the same deadlock class — and found it SAFE: Blink gates those reads behind permission/
  gesture, so page JS never reaches the sync call. execCommand('copy'/'paste') return false,
  navigator.clipboard read/write reject (NotAllowedError); nothing hangs (mb_shot exit 0). Smoke 45
  guards clipboard hang-safety (copy/paste/writeText run to completion, host stays scriptable).
  55/55, no survivors. So ALL four LocalFrameHost [Sync] calls are now handled and the clipboard
  [Sync] surface is confirmed gated; Blob's BlobURLStore.Register stays the one open [Sync] hang.

- ✅ FIX: URL.createObjectURL no longer hangs — the LAST [Sync]-mojo hang is closed (2026-06-23):
  precisely bisected the Blob gap with bounded probes — only createObjectURL hangs; Blob
  construction, .size/.type, .text(), .arrayBuffer() and FileReader.readAsText all run without
  hanging (the async reads just don't RESOLVE, since the blob data pipe is also unserviced —
  graceful, not a hang). Root cause: PublicURLManager::RegisterURL (public_url_manager.cc:161)
  makes the [Sync] BlobURLStore.Register call; blob_url_store.mojom marks Register as the ONLY
  [Sync] method (Revoke/ResolveAsURLLoaderFactory/ResolveAsBlobURLToken are async). The store
  remote is bound through the frame's navigation-associated channel
  (GetRemoteNavigationAssociatedInterfaces, line 77) — NOT our droppable broker — so its receiver
  is held pending-but-unserviced and the sync call blocks the main thread forever (confirmed
  mb_shot exit 137). FIX (patches/0003-skip-blob-url-register.patch): skip the Register call.
  createObjectURL still returns a blob: URL string (no hang); the URL won't resolve to data (the
  async ResolveAsURLLoaderFactory just fails to load — no block), and revokeObjectURL/Revoke are
  async so they're fine. Verified: mb_shot exit 0, createObjectURL returns "blob:...", suite 56/56,
  no survivors; patch reproducible. LIMITATION (honest): blob: URLs and blob DATA reads (text/
  arrayBuffer/FileReader) still don't COMPLETE — that needs an in-process blob registry/store on a
  service thread (heavy). But nothing in the Blob path HANGS anymore.
  >>> MILESTONE: with this, EVERY [Sync]-mojo-to-browser hang is closed — the three modal dialogs,
  beforeunload, clipboard (gated), and now Blob. No known JS API can hang or crash the host now;
  unbacked services degrade gracefully (inert / reject / pending-promise / event-based failure).

- ✅ FIX + capability: Web Crypto (crypto.subtle.*) now WORKS (2026-06-23): swept compute-heavy
  APIs; found crypto.subtle SIGSEGVs (the rest — WebAssembly compile/instantiate/call,
  structuredClone incl. Map, TextEncoder/TextDecoder UTF-8, crypto.getRandomValues — all already
  work). Root cause (same family as the worker crash): SubtleCrypto derefs
  Platform::Current()->Crypto() unconditionally (subtle_crypto.cc, every op), and base
  blink::Platform::Crypto() returns nullptr (platform.h:741) -> null deref on any crypto.subtle
  call. FIX: MbPlatform::Crypto() now returns a real BoringSSL-backed webcrypto::WebCryptoImpl
  (the same impl content's BlinkPlatformImpl uses); added //components/webcrypto to the GN deps,
  member is a std::unique_ptr<webcrypto::WebCryptoImpl> constructed in the ctor. Unlike the other
  fixes this is not just crash-safety — it ENABLES the feature: verified the async digest actually
  computes (SHA-256("abc") = ba7816bf...:32, the correct value), so the full WebCryptoImpl surface
  (encrypt/decrypt/sign/verify/generateKey/import/export/digest/HMAC/AES/RSA/EC) is now live in-
  process. Web Crypto requires a secure context, so the guard loads from an https origin. Smoke 47
  guards getRandomValues + subtle.digest. 57/57, no survivors. Confirmed-working compute set now:
  WASM, structuredClone, Text{Encoder,Decoder}, getRandomValues, and full SubtleCrypto.

- ✅ FIX: `new AudioContext()` no longer crashes (silent audio device) (2026-06-23): continued the
  API sweep. Confirmed working (no change needed): new Audio()/<video>, OffscreenCanvas + 2D,
  createImageBitmap, navigator.geolocation, navigator.gpu (WebGPU present), speechSynthesis,
  mediaDevices.getUserMedia, history.pushState/replaceState, performance + marks/measures, Intl
  (NumberFormat/DateTimeFormat), Notification, sendBeacon, requestIdleCallback, DOMParser/
  XMLSerializer, OfflineAudioContext. CRASH found: realtime `new AudioContext()` SIGSEGVs.
  Root cause (worker-pattern): base Platform::CreateAudioDevice returns nullptr, and
  AudioDestination's ctor derefs it UNGUARDED at audio_destination.cc:458
  (web_audio_device_->SampleRate()) — some sibling initializers are null-guarded, but not all.
  FIX: MbPlatform::CreateAudioDevice returns a silent stub WebAudioDevice (MbSilentAudioDevice):
  valid params (48k / 128-frame quantum / 2ch / OUTPUT_DEVICE_STATUS_OK), no-op Start/Stop/Pause/
  Resume so the render callback is never pulled (no sound, nothing to drive on a thread). Added
  //media to GN deps for media::AudioRendererSink / OutputDeviceStatus. Verified: `new
  AudioContext()` -> state 'running', sampleRate 48000, oscillator connects/starts; mb_shot exit 0
  (was 139). Graceful: AudioContext is fully constructible/wirable, just silent. OfflineAudioContext
  (which renders to a buffer) already worked and still does. Smoke 48 guards both. 58/58.

- ✅ Broad API sweep — NO crashes this round (2026-06-23): probed another ~16 common APIs; all
  safe, no fix needed. Streams (ReadableStream/TransformStream/CompressionStream/TextDecoderStream),
  MessageChannel/MessagePort, FontFace + document.fonts, CSS.registerProperty (Houdini custom
  props), CSS.paintWorklet.addModule (no crash; promise pending — worklets aren't wired), and
  native FORM CONTROLS painting (checkbox/radio/range/select/progress/meter/button) all work.
  Notably Platform::ThemeEngine() has a real default (not a null-returning inline like Crypto/
  CreateAudioDevice were), so the WebThemeEngine paint path is safe — form controls render to
  non-blank pixels. Locked in two meaningful guards (distinct subsystems, real data/pixels, not
  just "constructs"): smoke 49 = ReadableStream delivers an enqueued chunk through a reader (async
  stream plumbing end-to-end); smoke 50 = form controls paint non-blank via WebThemeEngine. 60/60.
  STATUS: the common web-platform API surface is now very broadly verified — across all sweeps the
  only crashes were the null-Platform-method class (Worker, SubtleCrypto, AudioContext, all fixed)
  and the [Sync]-mojo hangs (dialogs/Blob, all fixed). Nothing probed since hangs or crashes.

- ✅ RENDERING CORRECTNESS verified (2026-06-23): shifted from "doesn't crash / non-blank" to
  "produces the RIGHT pixels", the actual point of a modern engine. LAYOUT (exact, via
  getBoundingClientRect): flexbox justify-content:space-between -> children at x=0 and x=300 in a
  400px row; CSS grid repeat(3,1fr) in 300px -> 3rd cell x=200; transform:translateX(50px) ->
  bounding rect x=50 (transform reflected). PAINT (exact pixel sampling via mbPaintToBitmap, BGRA):
  solid #00ff00 -> pure green (0,255,0); rgba(0,0,255,0.5) over white -> correct alpha composite
  ~(128,128,255); two absolutely-positioned boxes -> later (blue) stacks over earlier (red) at the
  overlap; horizontal linear-gradient(#f00->#00f) -> red-ish at left edge, blue-ish at right. So
  the engine lays out modern CSS and rasterizes color/alpha/stacking/gradients CORRECTLY, not just
  without crashing. Smoke 51-52 lock in flexbox + solid + alpha + stacking + gradient. 65/65.

- ✅ ADVANCED paint correctness (filters/clip/shadow) (2026-06-23): verified the modern paint
  features that distinguish M150 from the old ~2015 engine, with exact pixel checks. CSS
  filter:grayscale(1) on a red box -> desaturated gray (R==G==B, ±4) — exercises the SkImageFilter
  pipeline; a !filter engine would leave it red. border-radius:50% -> a circle that CLIPS: center
  pixel is the box color, corner pixel is page background (rounded-corner rasterization, not just a
  layout attr). box-shadow:0 0 0 10px #000 (solid spread) -> a black ring OUTSIDE the border box
  (shadow paints beyond the box), with the box interior still white. All exact. Smoke 53-55 lock
  in grayscale-filter + border-radius-clip + box-shadow. So the engine renders filters, clipping
  and shadows correctly, not merely without crashing. 68/68, no survivors.

- ✅ TEXT rendering + font metrics verified (2026-06-23): directly checked the thing the "fonts"
  P1 gap worried about — that text actually rasterizes to glyphs (not tofu/blank). Rendered black
  30px monospace on white and scanned the text band: both dark pixels (glyph strokes) AND white
  pixels (inter/intra-glyph gaps) are present — proving real font data + shaping + rasterization
  (all-white would mean missing fonts; all-dark would mean a solid block, not text). Font METRICS
  scale too: canvas measureText('MMMM') at 40px is ~2x the 20px width (real shaping/advances, not a
  stub). So base text rendering on macOS system fonts WORKS; the remaining font work is narrower
  (web @font-face over network, full .pak coverage), not "text doesn't render". Smoke 56-57 lock
  in glyph rasterization + measureText scaling. 70/70, no survivors.

- ✅ REPRODUCIBLE BUILD verified via build.sh from a clean tree (2026-06-23): validated the
  "standalone project" claim end to end. Reverted the 3 patched donor files to pristine, then ran
  the documented `./build.sh <chromium-tree>`: it staged src/miniblink_host, applied ALL THREE
  patches fresh from clean (0001-offscreen-widget-compat, 0002-suppress-js-dialogs,
  0003-skip-blob-url-register — all "applied", no WARN), ran gn gen (the new //media and
  //components/webcrypto deps resolved), ninja-built libminiblink_host.dylib + mb_smoke + mb_shot,
  vendored the resource paks, and ran the smoke suite: 70 passed, 0 failed, no survivors. NOTABLE:
  build.sh runs mb_smoke UNBOUNDED at the end, and it now completes cleanly — a direct payoff of
  closing every [Sync]-mojo hang (dialogs/Blob) and null-Platform crash (Worker/Crypto/Audio):
  the documented build is no longer at risk of hanging. So the project builds reproducibly from a
  donor Chromium checkout via its own patches + GN seam, exactly as the standalone-project goal
  intends. (Only manual one-time step remains the gn_all deps += line in the root BUILD.gn, which
  build.sh detects and instructs.)

- ✅ SVG rendering + CSS transitions verified (2026-06-23): two more correctness checks. SVG (a
  distinct render path from CSS boxes, and ubiquitous for icons/charts/logos): inline <svg> with a
  green <rect> and red <circle> on white rasterizes correctly — sampling inside the rect gives
  green, inside the circle gives red, an empty corner is white. So SVG geometry + fill paint work.
  CSS transition: a div transitioning width 0->100px over 100ms reaches 100px after the clock is
  driven past the duration (the property interpolates over time via the animation clock, not a
  jump/no-op) — complements the existing rAF / Web Animations coverage with declarative CSS
  animation. Smoke 58-59 lock these in. 72/72, no survivors.

- 📌 SCOPING (corrects earlier note): Blob DATA resolution needs a SERVICE THREAD, not bounded.
  blob_registry.mojom marks `BlobRegistry.Register` as [Sync]. `new Blob()` constructs without
  hanging today ONLY because MbEmptyBroker DROPS the BlobRegistry receiver -> the pipe closes ->
  the [Sync] Register returns fast (failed) rather than registering, so reads (text/arrayBuffer/
  FileReader) stay pending. To actually serve blob bytes we must bind a real in-process
  BlobRegistry — but binding it on the MAIN THREAD would deadlock exactly like the JS dialogs did
  (a [Sync] call serviced by a same-thread receiver whose binder is delivered async). So Blob data
  (and by extension blob: URL resolution, FileReader results) requires a dedicated mojo SERVICE
  THREAD to host BlobRegistry/Blob receivers off the main thread. This is the same shared
  infrastructure real worker execution would want. Confirmed heavy; correctly deferred. (Earlier I
  speculated blob reads were "async so main-thread-serviceable" — true for the READS, but the
  Register gateway is [Sync], so the whole thing still needs the off-thread servicer.)

- ✅ Holistic integration test (2026-06-23): added a single realistic composed page exercising
  several subsystems at once — a flex header containing an inline SVG icon + bold text, over a CSS
  grid body — and asserted both exact geometry (grid columns at x=0 and x=150) AND paint (the SVG
  icon rasterizes greenish at its position; the header text produces dark glyph pixels) in one
  render. This catches cross-subsystem composition bugs that the isolated unit checks miss. Smoke
  60. 73/73, no survivors.

- ✅ CAPABILITY: mbClickSelector + mb_shot --click (2026-06-24): added the Puppeteer-style
  page.click(selector) automation primitive — the first new C-ABI method in a while, a real
  capability (not a test). MbWebView::ClickSelector(css) embeds the selector as a JS string
  literal (escaping \ and "), asks the page for the matched element's center via
  getBoundingClientRect, and synthesizes a click there via the existing SendMouseClick; returns
  false on no-match or zero-size box. Exposed as `int mbClickSelector(mbView*, const char*)` and
  wired into mb_shot as `--click CSS` (clicks before capture — expand a menu, dismiss a banner —
  then settles). Verified end to end: smoke 61 clicks a button by #id (handler runs) and confirms
  a non-matching selector returns 0; mb_shot --click toggled a page's DOM to 'CLICKED' before
  capture. README C-ABI list + mb_shot usage updated. 74/74, no survivors.

- ✅ CAPABILITY: mbFillSelector (Playwright-style fill) (2026-06-24): added form-fill by selector,
  the natural companion to mbClickSelector. MbWebView::FillSelector(css,text) focuses the matched
  <input>/<textarea>, sets .value through the prototype's NATIVE value setter (so frameworks that
  wrap it — React's value tracker — observe the change), then dispatches bubbling input+change
  events like real typing; returns false on no match. Factored a shared JsEscape() helper (escapes
  \ " \n \r) now used by both ClickSelector and FillSelector for safe selector/text embedding.
  Exposed as `int mbFillSelector(mbView*, const char* css, const char* text)`. Smoke 62 fills an
  input and asserts both .value updated AND an 'input' listener fired (frameworks depend on the
  event, not just the value), and that a non-match returns 0. README C-ABI list updated. 75/75,
  no survivors. The Puppeteer/Playwright-style automation surface now: waitForSelector + click +
  fill + type(SendText) + eval + screenshot/pdf.

- ✅ CAPABILITY: mbWaitForFunction (Puppeteer waitForFunction) (2026-06-24): general wait primitive
  that polls a JS expression (pumping the loop, same 10ms cadence as WaitForSelector) until it
  evaluates truthy or the timeout elapses; exceptions count as falsey. Generalizes
  mbWaitForSelector — wait on ANY condition (window.appReady, results.length>N, a spinner gone).
  MbWebView::WaitForFunction wraps the caller expr as (function(){try{return((EXPR)?1:0);}catch(e)
  {return 0;}})(). Exposed as `int mbWaitForFunction(mbView*, const char* js_expr, int timeout_ms)`.
  Smoke 63: a setTimeout flips window.__ready after 50ms -> waitForFunction returns 1; a never-true
  predicate times out -> 0 (and does not hang past the timeout). README updated. 76/76, no
  survivors.

- 📝 PLAN: off-main-thread mojo service host (docs/design-blob-service-host.md) (2026-06-24):
  scoped the one remaining heavy enabler into an executable, incremental plan rather than cramming
  it into a tick. Core insight (validated by the dialog/Blob analysis): every remaining functional
  gap is a [Sync] mojo call the main thread makes to a browser-process service; an in-process
  receiver on the SAME thread can't reply (deadlock), but mojo CAN service a sync call from a
  receiver on ANOTHER thread — so a dedicated service thread (base::Thread, IO pump) hosting
  BlobRegistry/Blob (and later worker host endpoints) is the fix. Key simplification found:
  DataElementBytes carries embedded_data inline for blobs <=256KB (the common `new Blob(['x'])`
  case), so the first working increment can store bytes directly and skip the BytesProvider
  callback. Plan has 6 increments, each independently buildable + mb_smoke-verified (no broken
  intermediate state): (1) service-thread scaffold, (2) VALIDATE cross-thread [Sync] before
  building anything on it, (3) Register inline bytes, (4) ReadAll via data pipe -> blob.text()
  resolves [the user-visible win], (5) blob: URL resolution, (6) BytesProvider for >256KB. Same
  foundation later hosts the worker host. Deferred deliberately: this is multi-tick design work,
  not a 5-minute cram; the plan de-risks it. (No code change this tick — the doc is the artifact.)

- 📝 PLAN REFINED with investigation findings (2026-06-24): de-risked the service-host plan with
  three concrete findings (folded into docs/design-blob-service-host.md): (1) the service thread
  ALREADY EXISTS — MbRuntime runs io_thread_ (mb-io, IO pump) with mojo::core::ScopedIPCSupport
  attached (mb_runtime.cc:146-151), so bind blob receivers there; no new thread (increment 1 done).
  (2) [Sync] mojo servicing already WORKS in-process — verified a file:// stylesheet loads + applies
  via the [Sync] MimeRegistry path (external CSS sets color:rgb(1,2,3), no hang); it doesn't
  deadlock because that caller is a loading sequence, not the blocked main thread. (3) For the real
  integration WE only provide the servicer — Blink makes the BlobRegistry.Register [Sync] call
  itself (as in production), so the friend-gated ScopedAllowSyncCall (unusable) is a non-issue and
  the standalone validation experiment can be DROPPED. Net: the 6-step plan collapses to "bind a
  real BlobRegistry/Blob on io_thread_ (inline embedded_data) -> ReadAll via data pipe -> blob.text()
  resolves", with the blob.text() smoke as the authoritative check of the one narrowed unknown
  (main-thread caller + io_thread servicer). Lower risk, fewer steps. Still multi-tick (real
  BlobRegistry/Blob + data-pipe code), so executed in a focused session, not crammed.

- 📝 PLAN now EXECUTION-READY: increment 3+4 fully specified (2026-06-24). Final code-inspection
  resolved every unknown (in docs/design-blob-service-host.md "Implementation notes"): BlobRegistry
  comes via the PLATFORM broker (blob_data.cc:93 -> Platform::GetBrowserInterfaceBroker), so route
  it in MbEmptyBroker; add MbRuntime::ServiceTaskRunner() (= io_thread_->task_runner()) and
  MakeSelfOwnedReceiver(MbBlobRegistry) there; blink variant (WTF types); Register appends each
  DataElementBytes.embedded_data (inline for <=256KB) and binds MbBlob(uuid,bytes); the read path
  is Blob.ReadAll(pipe, BlobReaderClient) (file_reader_loader.cc:102) -> OnCalculatedSize, write
  bytes (one WriteData for small blobs), OnComplete(0,size); other Blob methods stubbed; new
  blob/mb_blob_registry.{h,cc} into the GN sources; smoke = new Blob(['hello']).text()=='hello'.
  >>> NEXT TICK SHOULD EXECUTE increment 3+4 atomically (implement -> build -> verify blob.text(),
  or revert if it can't be made clean in-tick). The spec makes it mechanical. This concludes the
  de-risking phase (3 investigation ticks: plan -> io_thread/[Sync]-works findings -> full spec);
  the planning was warranted because it's the only remaining heavy item and cramming risks broken
  state, but it is now fully scoped and the next step is code.

- ✅✅ DONE: Blob data resolves — first heavy capability shipped; service-host validated (2026-06-24):
  executed increments 3+4. New blob/mb_blob_registry.{h,cc}: MbBlobRegistry + MbBlob (blink mojom
  variant) bound on the IO/service thread via MbRuntime::ServiceTaskRunner() (new accessor =
  io_thread_->task_runner()); MbEmptyBroker now routes blink::mojom::blink::BlobRegistry there
  instead of dropping it. Register concatenates each DataElementBytes.embedded_data (inline for
  <=256KB) and binds an MbBlob(uuid,bytes); the RegisterCallback Run() sends the () reply that
  unblocks Blink's [Sync] Register. Blob.ReadAll fires BlobReaderClient.OnCalculatedSize, writes
  the bytes (WriteAllData, one shot for small blobs), then OnComplete(0,size); GetInternalUUID/
  Clone/CaptureSnapshot real, the rest stubbed. RESULT: new Blob(['hello']).text()==='hello',
  arrayBuffer().byteLength===5, FileReader.readAsText delivers — confirmed by smoke 64 AND mb_shot
  (blob-text=hello, fr-result=hello, previously never resolved). 77/77, exit 0, NO SURVIVORS — the
  main thread's [Sync] Register is serviced by the io_thread receiver without deadlocking (the
  whole premise), and reads stream over a data pipe. This PROVES the cross-thread service-host
  architecture works in-process; the same foundation now unblocks blob: URL resolution (revisit
  patches/0003) and, later, real worker execution. Honest scope: small blobs only (inline bytes);
  >256KB needs the BytesProvider callback (increment 6); blob: URL load is increment 5.

- ✅ Consolidated the blob win + scoped the remainder (2026-06-24): measured the shipped path's
  real reach — blobs up to 200KB round-trip fine (inline embedded_data + one WriteAllData); the
  ONLY gap is >256KB, which resolves EMPTY (no embedded_data, BytesProvider ignored). Locked in
  smoke 65 (a 100KB blob text() round-trips to length 100000 — proves realistic sizes work, not
  just 'hello'). 78/78. Precisely scoped increment 6 in the design doc (BytesProvider lazy-fetch
  after the Register reply + read-gating; SimpleWatcher chunked pipe write for data > pipe
  capacity; verify with a 500KB blob), same pre-scope-then-execute approach that made increment 3
  go in first-try. Increment 5 (blob: URL resolution) needs the frame's navigation-ASSOCIATED
  interface channel (BlobURLStore is bound via GetRemoteNavigationAssociatedInterfaces, NOT the
  Platform broker we control for BlobRegistry) plus a blob: URLLoaderFactory — a separate heavier
  effort, not the broker-routing pattern used for blob data. Both remain focused follow-ups; the
  common blob-data case is shipped and guarded.

- ✅✅ DONE: large blobs (>256KB) — Blob DATA now complete for ALL sizes (2026-06-24): executed
  increment 6. Reworked MbBlob around ordered Parts (inline bytes OR a bound BytesProvider remote):
  Register replies immediately (unblocking the main thread), then Materialize() walks parts in
  order appending inline bytes synchronously and fetching provider parts via RequestAsReply (async,
  serviced by the now-unblocked main thread); reads arriving before materialization are queued and
  drained when ready. Capturing `this` in the reply callback is safe — the provider Remote is a
  member, so destroying MbBlob cancels the pending reply. Added BlobReadSession: a self-owned,
  CHUNKED data-pipe writer driven by a mojo::SimpleWatcher (WriteData loop; ArmOrNotify on
  SHOULD_WAIT) so blobs larger than the pipe buffer stream correctly; it also replaced the old
  WriteAllData path for small blobs (unified). RESULT: new Blob(['q'.repeat(500000)]).text().length
  === 500000 and arrayBuffer().byteLength === 500000 — confirmed by smoke 66 AND mb_shot
  (bigtext-len=500000, was 0). 79/79, exit 0, no survivors (no hang from the watcher/provider).
  Blob data is now correct for any size; remaining blob work is only increment 5 (blob: URL
  resolution via the navigation-associated channel + a blob: URLLoaderFactory).

- ✅ canvas.toBlob() works + idle tasks now run in the pump (2026-06-24): probing canvas->blob
  (now that blob data resolves) found toBlob's callback never fired promptly. Root cause: toBlob
  encodes on an IDLE task (CanvasAsyncBlobCreator -> ThreadScheduler::PostIdleTask), and idle tasks
  only run inside an idle period, which the compositor starts — we have none, so idle work only
  fired via each feature's ~1s fallback timeout. FIX: WaitMs now starts an idle period each pump
  iteration via ThreadScheduler::Current()->ToMainThreadScheduler()->StartIdlePeriodForTesting()
  then drains, so idle-scheduled work (canvas.toBlob, requestIdleCallback, lazy/GC idle tasks) runs
  promptly. General improvement, not just toBlob. Verified: canvas.toBlob('image/png') -> a
  readable PNG blob (89 50 4E 47 magic), completing within a 400ms wait instead of ~1s; the encode
  itself was always fine (toDataURL proved it), only the async scheduling stalled. So headless
  canvas image export (canvas -> toBlob -> bytes) now works end to end. Smoke 67 guards it; existing
  rAF/Web-Animations cases still pass (the idle-period change is safe). 80/80, no survivors.

- 🔎 INVESTIGATION: blob-adjacent gaps probed; three are deeper than one tick (2026-06-24). Probed
  capabilities the blob work might have unlocked; found three SEPARATE gaps, none a clean single-
  tick fix, so recorded precisely rather than shipping unverified code (reverted an attempt; tree
  stays 80/80):
  (1) Response.blob()/Request.blob() resolve EMPTY. It routes through
  BlobRegistry.RegisterFromStream (fetch_data_loader.cc:84) via the Platform-broker registry — i.e.
  our MbBlobRegistry. I implemented RegisterFromStream (DataPipeDrainer -> bytes -> MbBlob ->
  BlobDataHandle::Create, the blink type-mapped reply) and INSTRUMENTED it: the drain callback NEVER
  FIRED, so RegisterFromStream is not reached — the blocker is UPSTREAM body loading
  (BodyStreamBuffer::StartLoading / the body BytesConsumer not delivering into the stream), not the
  registry. Reverted the impl (correct but unverified/unreached). Needs a separate fetch-body
  investigation. (2) fetch('data:...') fails "Failed to fetch" — our libcurl loader (mb_url_loader)
  doesn't serve the data: scheme; data: URLs should be handled in-renderer / by the loader. Separate
  loader gap. (3) blob: URL resolution (increment 5) confirmed heavy: needs MbFrameClient to override
  GetRemoteNavigationAssociatedInterfaces returning an AssociatedInterfaceProvider with
  OverrideBinderForTesting("blink.mojom.BlobURLStore", ...) -> service-thread MbBlobURLStore, PLUS a
  blob: URLLoaderFactory+URLLoader (CreateLoaderAndStart -> OnReceiveResponse + a body pipe fed by
  the existing BlobReadSession-style chunked writer) -> OnComplete, PLUS reverting patches/0003 (the
  now-serviceable [Sync] Register). 3-4 components — a focused pass. VERIFIED WORKING this tick:
  new Blob([Uint8Array]).text() === 'hi' (typed-array element via the inline path). No code change
  committed; the value is the precise scoping of three independent follow-ups.

- ✅ FIX: blob-of-blob (is_blob elements) now read through — Response.blob()/Blob.slice() work
  (2026-06-24): pinned finding #1 from above. Instrumented Register: Response.blob() registers TWO
  blobs — an inner one (is_bytes, the body) and an OUTER wrapper (is_blob, a reference to the
  inner). My Register only handled is_bytes, so the wrapper had no bytes (blob.size was right, from
  Blink's record, but ReadAll returned empty). FIX: handle is_blob DataElements — DataElementBlob
  carries a PendingRemote<Blob> + offset/length; added a third MbBlob::Part variant (blob_ref +
  offset/length) and a self-owned BlobRefReader (ReadAll the referenced blob into a pipe, drain via
  DataPipeDrainer, slice [offset,offset+length)), materialized like the BytesProvider path. Added a
  WeakPtrFactory so the async read-through callback is safe if the MbBlob dies (BlobRefReader is
  self-owned and may outlive it). Verified: new Response('hello-resp').blob().text() ==='hello-resp'
  and new Blob(['0123456789']).slice(2,5).text() ==='234' — smoke 68 AND mb_shot (resp-blob was
  empty, now 'hello-resp'). 81/81, no survivors. So fetch(httpUrl).blob(), Response/Request.blob(),
  and Blob.slice() reads now resolve. (RegisterFromStream remains stubbed but is not on this path —
  the earlier instrumentation showed it isn't reached for these; the wrapper-blob route is. The
  fetch(data:) loader gap and blob: URL increment 5 remain separate follow-ups.)

- ✅ FIX: fetch('data:...') / data: subresources now load (2026-06-24): MbURLLoader handled only
  file: and http(s):, so data: URLs failed ("Failed to fetch") for fetch/XHR (note: data: IMAGES
  already worked via the image pipeline; only the loader path was missing). FIX: both MbFetchUrl
  (navigation) and MbURLLoader::Deliver (subresource/fetch) now decode data: in-process via
  net::DataURL::Parse(url, &mime, &charset, &bytes), feeding the parsed mime into the response
  Content-Type. //net was already linked (net_errors). Verified: fetch('data:text/plain,hello-data')
  .text() === 'hello-data' and fetch('data:application/octet-stream,abcd').arrayBuffer().byteLength
  === 4 (smoke 69; mb_shot confirmed ft=hello-data). 82/82, no survivors. KNOWN REMAINING GAP:
  fetch(dataUrl).blob() still rejects "Failed to fetch" — a fetch/blob-RESPONSE interaction (the
  Response.blob() of a *fetched* response), distinct from data: decoding and from the direct
  Response.blob() path that now works (smoke 68). Direct new Response(...).blob() and
  new Blob(...) reads work; only the fetched-response->blob combination remains. Separate follow-up
  alongside blob: URL (increment 5).

- ✅ FIX: fetch(url).blob() works — RegisterFromStream implemented + VERIFIED reached (2026-06-24):
  closed the gap noted just above. A *fetched* response's .blob() streams the body through
  BlobRegistry.RegisterFromStream (FetchDataLoader::CreateLoaderAsBlobHandle, fetch_data_loader.cc),
  which was stubbed to return nullptr -> "Failed to fetch". (Earlier I reverted a RegisterFromStream
  impl because new Response(string).blob() didn't reach it — that path uses the is_blob wrapper,
  smoke 68. A FETCHED response's body genuinely uses RegisterFromStream — the distinction the
  instrumentation revealed.) Implemented StreamRegistration (mojo::DataPipeDrainer reads the body
  pipe to EOF -> MbBlob(uuid,bytes) -> reply with BlobDataHandle::Create(uuid,type,size,remote), the
  blink type-mapped SerializedBlob reply). VERIFIED reached + correct: fetch('data:text/plain,
  blob-data').blob().text() === 'blob-data' (smoke 69 — would fail if RegisterFromStream returned
  null). 82/82, no survivors. Blob is now complete across ALL creation paths: inline, BytesProvider
  (>256KB), is_blob (Response.blob/slice), and RegisterFromStream (fetch/stream bodies). Only
  increment 5 (blob: URL resolution) remains.

- 📝 Increment 5 (blob: URL) EXECUTION-READY (2026-06-24): confirmed the routing and every
  signature, scoped it fully in docs/design-blob-service-host.md. Routing: a blob: subresource is
  resolved in loader_factory_for_frame.cc:199 -> PublicURLManager::Resolve ->
  BlobURLStore.ResolveAsURLLoaderFactory, so we MUST supply a URLLoaderFactory (blob: never reaches
  MbURLLoader — no shortcut). Components: (1) MbFrameClient::GetRemoteNavigationAssociatedInterfaces
  returns an AssociatedInterfaceProvider(task_runner) with OverrideBinderForTesting(
  "blink.mojom.BlobURLStore", ...) -> service thread; (2) MbBlobURLStore (url->Remote<Blob> map):
  Register(blob,url,cb) stores + cb.Run(), Revoke erases, ResolveAsURLLoaderFactory binds a
  factory; (3) MbBlobURLLoaderFactory (network::mojom::blink::URLLoaderFactory); (4)
  MbBlobURLLoader serving the blob via OnReceiveResponse(head, body pipe via the BlobReadSession
  chunked-writer) + OnComplete; (5) revert patches/0003. Verify: createObjectURL(blob) -> fetch(u)
  .text() and <img src=u> render. Genuinely heavy (3 new classes + associated override + the new
  mojo URLLoaderFactory surface) — a focused pass, not an end-of-tick cram; the spec makes it
  mechanical. This is the LAST blob item; everything else in the blob subsystem is shipped.

- 🔬 Increment 5 ATTEMPTED -> deadlock; reverted to 82/82 (2026-06-24): built the full blob: URL
  feature (MbBlobURLStore + URLLoaderFactory/Loader + MbFrameClient associated-provider override +
  reverted patch 0003) and it DEADLOCKED — createObjectURL hung on [Sync] BlobURLStore.Register.
  ROOT CAUSE (instrumented: GetRemoteNavAssoc fired but the OverrideBinderForTesting binder NEVER
  did): the testing AssociatedInterfaceProvider's LocalProvider invokes the override binder only
  when its receiver dispatches the queued GetAssociatedInterface message — on the provider's task
  runner (main thread). PublicURLManager binds frame_url_store_ then immediately makes the [Sync]
  Register on the main thread, blocking it before that dispatch runs -> binder never fires ->
  Register waits forever. The bind HANDSHAKE needs the main thread that the [Sync] call has
  blocked. Reverted cleanly (restored patch 0003, removed the new code, deleted mb_blob_url_store.*),
  rebuilt -> 82/82, no survivors. Finding recorded in docs/design-blob-service-host.md with three
  next-attempt options (real pre-bound associated remote on the service thread; eager bind at
  frame creation; directly-bound endpoint). blob: URL is harder than the signatures implied — the
  associated-binding handshake races the [Sync] call. All blob DATA paths remain shipped + working;
  this is only the blob:-URL-as-loadable-resource piece. Discipline: attempted, found the real
  blocker, reverted rather than ship a hanging build.

- 📌 Increment 5 fix CONFIRMED (option a), execution-ready (2026-06-24): investigated the deadlock
  fix and confirmed the mechanism + primitives (in docs/design-blob-service-host.md). Replace
  OverrideBinderForTesting (which loops the bind through the blocked main thread) with a REAL
  blink::mojom::blink::AssociatedInterfaceProvider proxy whose RECEIVER is bound on the SERVICE
  thread; then GetAssociatedInterface (bind) and the [Sync] Register both dispatch off the main
  thread -> no deadlock. Primitives verified present: PendingAssociatedRemote::
  InitWithNewEndpointAndPassReceiver() + EnableUnassociatedUsage() (dedicated standalone pair);
  proxy's GetAssociatedInterface(name,receiver) -> MakeSelfOwnedAssociatedReceiver(MbBlobURLStore,
  receiver.PassHandle()) when name==BlobURLStore::Name_. The store/factory/loader classes from the
  reverted attempt were correct (only the binding was wrong) — reconstruct + swap to the proxy
  binding + revert patch 0003 = one focused pass. One detail to confirm during execution: whether
  EnableUnassociatedUsage is needed on both the remote and the receiver. blob: URL is now a fully
  mechanical plan; deferred to a focused pass (not crammed end-of-tick). Tree stays 82/82.

- 🔬 Increment 5 attempt 2: DEADLOCK SOLVED, but a 2nd barrier found; reverted to 82/82 (2026-06-24):
  executed option (a) and it FIXED the deadlock — a real blink::mojom::AssociatedInterfaceProvider
  proxy (regular non-WTF variant, the ctor's type) via InitWithNewEndpointAndPassReceiver() +
  EnableUnassociatedUsage() (remote only), receiver bound on the service thread (MbAssocProvider),
  binding MbBlobURLStore on GetAssociatedInterface(BlobURLStore::Name_). Instrumented + confirmed:
  createObjectURL NO LONGER HANGS, Register fires, the url->blob map populates, and on fetch
  ResolveAsURLLoaderFactory runs (found=1) and binds the factory. SECOND BARRIER: Blink then calls
  Factory.Clone but NEVER CreateLoaderAndStart, and fetch(blobURL) rejects "Failed to fetch" even on
  a stable https origin (same-origin blob:https://...) — so it is NOT the factory and NOT a file://
  quirk; it's a fetch-level rejection of the blob: request AFTER the factory is obtained, before any
  load. Likely the minimal host's SecurityOrigin / blob-URL-origin bookkeeping (BlobURLNullOriginMap
  / SecurityOrigin::CanRequest for blob:). Reverted (no passing test for not-end-to-end code; patch
  0003 restored). NEXT: trace why the blob: ResourceRequest is aborted post-resolve (fetch/CORS/
  response path). The hard known blocker (the [Sync] associated-binding deadlock) is SOLVED + the
  mechanism captured; blob: URL now hinges on this separate, narrower fetch-security barrier.

- ✅ Advanced CSS paint verified: ::before generated content + clip-path polygon (2026-06-24):
  diversified off blob: URL (deadlock solved + documented; the fetch-security barrier is a dedicated
  session, not worth more circling). Two more modern-CSS paint-correctness guards: (1) CSS generated
  content — #g::before{content:'';...background:#00ff00} paints a green box (pixel check at (10,10))
  AND getComputedStyle(el,'::before').backgroundColor reads rgb(0,255,0); ubiquitous (icons/badges/
  numbering). (2) clip-path:polygon(0 0,100% 0,0 100%) clips a blue box to a top-left triangle —
  inside (8,8) is blue, the clipped corner (70,70) is page background; a distinct clip mechanism from
  border-radius (smoke 54). Smoke 70-71. 84/84, no survivors. The rendering-correctness suite now
  spans layout, basic + advanced paint (filters/clip/shadow/gradient/blend-adjacent), text/glyphs,
  SVG, generated content, and clip-path — all asserting CORRECT output.

- ✅ CSS var()/calc()/clamp() verified (2026-06-24): the building blocks of modern stylesheets and
  design systems, resolved via computed style. Custom property color var(--accent) -> rgb(10,20,30);
  calc(100px+50px) -> 150px; clamp(10px,40px,100px) -> 40px; calc(var(--gap)*2) -> 16px (custom
  property cascading from :root + math composition). Smoke 72. 85/85, no survivors. Common framework/
  design-system CSS confirmed working.

- ✅ MULTIPLE concurrent views work (2026-06-24): a real embedder (tabs/windows) creates several
  mbViews over the one shared MbRuntime; verified that works rather than assuming a single view.
  Created a 2nd view alongside the 1st, loaded different documents + set different JS globals in
  each: each keeps its own DOM (#x -> 'view1' vs 'view2') and its own JS world (window.__who ->
  'one' vs 'two'), no cross-contamination; destroying the 2nd leaves the 1st fully usable
  (1+1===2 after). So the host supports concurrent views (no single-view/global-state assumption) —
  important for tab-like or parallel-render/scrape embedders. Smoke 73 (3 asserts). 88/88, no
  survivors.

- ✅ Stability across many sequential loads (2026-06-24): a long-running scraper/automation does
  thousands of loads on one view, so verify repeated loads don't leak state or degrade. Loaded 25
  varied documents in a row (different widths/colors/text + a per-load script) on one view,
  evaluating DOM + window.__k each time and painting every 8th; all 25 rendered AND scripted
  correctly, the final load is exactly right, exit 0, no survivors. Confirms clean per-load lifecycle
  (no accumulated breakage / state leak across navigations). Smoke 74. 89/89.

- ✅ FIX: file:// URLs now percent-decode (spaces) + web fonts (@font-face) work (2026-06-24):
  probing @font-face over file:// (a documented "fonts" gap) surfaced a real loader bug. MbURLLoader
  fed url.path() — still percent-encoded — to ReadFileToString, so any file:// path with a space
  (e.g. "Andale%20Mono.ttf", or macOS "Application Support") failed (0 bytes, NetworkError). FIX:
  both file:// sites (MbFetchUrl + Deliver) now use net::FileURLToFilePath(url,&fp), which decodes
  and converts properly. RESULT: @font-face over file:// now LOADS end to end — verified via mb_shot
  (FontFace.load() status='loaded'; the custom monospace font measures differently from serif:
  wTF=192 vs wSerif=195), so web fonts via the FontFace API + Skia/FreeType parse + metrics work
  (the "web fonts" gap was really this decode bug, not a missing pipeline). Portable regression guard
  (smoke 75): write a stylesheet with a space in its name, link it via file:///...%20..., assert the
  style applies (rgb(7,8,9)); uses a file:// base so it's same-origin (opaque about:blank -> file://
  is separately policy-blocked — noted). 90/90, no survivors. Real bug fix, found by probing.

- ✅ CSS background-image (data: SVG) paints (2026-06-24): a distinct path from <img> — the CSS
  background paint pipeline + a data: URL image + SVG-as-image. A 30x30 div with a green-SVG
  background-image (data:image/svg+xml,...) paints green at its center (pixel check). Works; common
  pattern (inline SVG backgrounds, icons). Smoke 76. 91/91, no survivors.

- 🔎 IFRAMES degrade gracefully (content doesn't load); CreateChildFrame is a heavy gap (2026-06-24):
  probed <iframe srcdoc/src>. No crash (exit 0) and the <iframe> element exists (contentWindow is an
  object, parent stays fully scriptable + renders), BUT the child frame is NOT created — window.
  frames.length===0 and f.contentDocument===null, so iframe content does NOT load. Cause: the host
  doesn't implement WebLocalFrameClient::CreateChildFrame (the documented TODO), so Blink never
  instantiates the child WebLocalFrame/document/widget/loader. This is a HEAVY gap (a whole child-
  frame lifecycle, like real workers / blob: URL) — not a single-tick fix; full support needs
  CreateChildFrame to build a child frame + frame client + commit its srcdoc/src via a child loader,
  and a widget to paint it. But it's robustness-safe: iframe-using pages don't crash or hang, they
  just show an empty subframe. Smoke 77 guards the no-crash/parent-scriptable invariant (like the
  worker/dialog guards). 92/92, no survivors. Added to the heavy-roadmap items.

- ✅ element.scrollIntoView() works (2026-06-24): a common automation primitive (scroll a target
  into view before click/capture). Our non-compositing widget handles scroll specially, so verified
  programmatic scroll-into-view actually moves the viewport: a target at 1500px down -> scrollY
  jumps from 0, and the target's getBoundingClientRect().top lands within [0, innerHeight). Smoke 78
  (paint first to force layout, then assert scrolled + on-screen). 93/93, no survivors.

- 📝 IFRAME support (CreateChildFrame) — execution-ready blueprint (2026-06-24): fully scoped the
  heavy iframe gap from frame_test_helpers (a working reference — this is a SOLVED pattern, unlike
  blob: URL). Pieces: (1) MbFrameClient gains `blink::WebLocalFrame* web_frame_` + a Bind(frame,
  unique_ptr<MbFrameClient> self) (stores web_frame_ + self_owned_) and SetFrame(frame) for the
  main frame; MbWebView calls frame_client_->SetFrame(main_frame_) after CreateMainFrame. (2)
  CreateChildFrame(scope,name,...,policy_container_bind_params,...,finish_creation): bind
  policy_container_bind_params.receiver to a tiny self-owned MbPolicyContainerHost (mojom::blink::
  PolicyContainerHost — 2 no-op methods: SetReferrerPolicy, AddContentSecurityPolicies); make a
  child MbFrameClient; `web_frame_->CreateLocalChild(scope, child_client, nullptr, LocalFrameToken())`;
  child_client->Bind(child, std::move(child_client_uptr)) (self-owned); finish_creation(child,
  DocumentToken(), mojo::NullRemote(), make_unique<UnguessableToken>(UnguessableToken::Create()));
  return child. (3) FrameDetached(DetachReason r): for a CHILD (self_owned_ set) do
  web_frame_->Close(r) then self_owned_.reset() LAST (self-destruct); for the MAIN frame (self_owned_
  null) do nothing — MbWebView owns its teardown. RISKS to verify at execution: child srcdoc/src
  content actually loads (frames.length==1, contentDocument!=null); child needs no separate
  WebFrameWidget (local children composite into the parent — confirm no widget DCHECK); the
  NullRemote browser-interface-broker for the child is tolerated. ~100 lines across mb_frame_client.
  {h,cc} + one MbWebView line. Heavy but mechanical with this blueprint; a focused pass, not an
  end-of-tick cram. iframes currently degrade gracefully (smoke 77), so this is additive.

- ✅ PARTIAL: iframe CreateChildFrame implemented — child frames are created (2026-06-24): executed
  the blueprint. MbFrameClient now implements CreateChildFrame (creates a local child via
  web_frame_->CreateLocalChild with its own self-owned child MbFrameClient, calls finish_creation),
  FrameDetached (self-destructs children; leaves the MbWebView-owned main frame alone), and SetFrame/
  Bind for frame association; MbWebView sets the main frame's web_frame_. The PolicyContainerHost
  receiver is left unbound (advisory; content still loads). RESULT: a page with an <iframe> now
  builds a real child frame — window.frames.length===1 (was 0) and contentDocument is accessible
  (was null), no crash, no regression (93/93). DetachReason is blink::DetachReason (namespace, not
  class-nested); MakeSelfOwnedAssociatedReceiver didn't fit the policy-container receiver so it's
  dropped. REMAINING (next increment): the child's srcdoc/src CONTENT does not commit (body empty) —
  needs child-navigation handling (override BeginNavigation, or commit the srcdoc into the child like
  MbWebView::CommitHtml does for the main frame). So iframes went from "no subframe" to "real but
  empty subframe"; content-commit is the follow-up. Smoke 77 updated to assert frame creation.

- 🔬 iframe content-commit attempted -> body_loader CHECK; reverted to 93/93 (2026-06-24): tried
  the next increment — override BeginNavigation (gated to child frames via self_owned_) to commit
  the child's navigation: WebNavigationParams::CreateFromInfo(*info), set fallback_base_url for
  about:srcdoc, then To<WebLocalFrameImpl>(child)->CommitNavigation(params). It CRASHED:
  FATAL frame_loader.cc:1012 "Check failed: params->body_loader" — CommitNavigation REQUIRES a body
  loader even for srcdoc (my assumption that DocumentLoader fills srcdoc was wrong at this layer).
  Also WebNavigationInfo carries NO srcdoc content (only url_request + requestor_base_url), so the
  srcdoc body must be read from the iframe owner element. NEXT (precise): in BeginNavigation, fill
  the body before committing — for about:srcdoc, get the srcdoc string from the child's owner
  (To<WebLocalFrameImpl>(child)->GetFrame()->Owner() -> the HTMLIFrameElement's srcdoc attr; this is
  what TestWebFrameHelper::FillStaticResponseForSrcdocNavigation does) and
  WebNavigationParams::FillStaticResponse(params, "text/html", "UTF-8", span(srcdoc)); for http/
  file/data src=, fetch via MbFetchUrl + FillStaticResponse (mime from the response). Reverted the
  crashing BeginNavigation; CreateChildFrame (frame creation) stays shipped at 93/93. Content-commit
  is a clean next increment now that the body_loader requirement + srcdoc source are known.

- 🔬 iframe content-commit attempt 2: 2 CHECKs deep, reverted to 93/93 (2026-06-24): implemented
  BeginNavigation with body-filling. Got PAST the body_loader CHECK: for about:srcdoc, read the
  srcdoc from the owner via To<HTMLFrameOwnerElement>(To<WebLocalFrameImpl>(child)->GetFrame()
  ->Owner())->FastGetAttribute(html_names::kSrcdocAttr).Utf8(), then WebNavigationParams::
  FillStaticResponse(params, "text/html", "UTF-8", span(body)). Hit the NEXT CHECK:
  document_loader.cc:2839 "did_have_policy_container || WillLoadUrlAsEmpty(Url())" — srcdoc is NOT
  load-as-empty, so params->policy_container MUST be set (frame_test_helpers sets it via
  WebPolicyContainer(WebPolicyContainerPolicies(), MockPolicyContainerHost::
  BindNewEndpointAndPassDedicatedRemote())). So content-commit needs the FULL frame_test_helpers
  CommitNavigation port, in order: (1) FillStaticResponse with the srcdoc body [DONE/known],
  (2) params->policy_container = WebPolicyContainer(policies, a PolicyContainerHost dedicated
  associated remote — a small MbPolicyContainerHost with a member AssociatedReceiver, kept alive),
  (3) likely merge sandbox flags into policy_container->policies.sandbox_flags and possibly
  origin_to_commit for sandboxed-origin. Each missing piece is a hard CHECK, so it must be ported
  in one go (not incrementally) — a focused pass. Reverted the crashing BeginNavigation; CreateChild
  Frame stays at 93/93. This is now FULLY mapped (exact CHECK sequence), just not single-tick-safe.

- ✅✅ DONE: iframe srcdoc content loads — full content-commit shipped (2026-06-24): executed the
  mapped sequence atomically and it CONVERGED (no CHECK #3). MbFrameClient::BeginNavigation (child
  frames only, gated on self_owned_): CreateFromInfo(*info); for about:srcdoc set fallback_base_url
  and read the srcdoc text from the owner (To<HTMLFrameOwnerElement>(child->GetFrame()->Owner())
  ->FastGetAttribute(html_names::kSrcdocAttr)); WebNavigationParams::FillStaticResponse(params,
  text/html, UTF-8, body) [satisfies the body_loader CHECK]; params->policy_container =
  WebPolicyContainer(WebPolicyContainerPolicies(), transient MbPolicyContainerHost::BindRemote())
  [satisfies the policy_container CHECK — the host is a local, like frame_test_helpers' mock]; then
  To<WebLocalFrameImpl>(child)->CommitNavigation(params, nullptr). RESULT: <iframe srcdoc> now
  renders — contentDocument.body.textContent is the srcdoc DOM ('child-body'), frames.length==1, no
  crash, no regression. Smoke 77 upgraded to assert content. 93/93, no survivors. So iframes work
  end to end for srcdoc (the common test/embed case). REMAINING: src=http/file/data children still
  commit EMPTY (BeginNavigation fills an empty body for non-srcdoc) — the follow-up is to fetch the
  src body via MbFetchUrl + FillStaticResponse; and sandboxed iframes don't enforce sandbox flags
  (CreateChildFrame ignores frame_policy). Both are clean, bounded follow-ups on this working base.

- ✅✅ DONE: iframe src= content loads (file/http/data children) — shipped (2026-06-24): the
  follow-up to srcdoc. Extended MbFrameClient::BeginNavigation: for a child whose URL is neither
  about:srcdoc nor about:* and non-empty, fetch the body via MbFetchUrl(url, &body, &content_type,
  user_agent_, extra_headers_) [the SAME loader subresources use — file://, http(s)://, data:], use
  the response content-type (stripped of ;params) as the commit mime, then the same
  FillStaticResponse + transient-MbPolicyContainerHost + To<WebLocalFrameImpl>->CommitNavigation
  path. RESULT: <iframe src=...> now fetches and commits its own document. Verified two ways:
  (1) mb_shot probe — file:// parent + data: child read back 'data-child'; (2) smoke case 78 — a
  data: src child under a file:// parent reads contentDocument.body.textContent == 'src-child'.
  KEY FINDING (cost a debug cycle): the child of an about:blank (opaque-origin) parent gets a FRESH
  opaque origin, so the parent's contentDocument read is cross-origin BLOCKED — correct browser
  behavior, NOT a commit failure (frames.length==1, navigation committed; the parent just can't peek
  in). The child of a file:// parent inherits file:// → same-origin → readable. So the test asserts
  content under a file:// base. 94/94, no survivors. REMAINING from the iframe arc: sandboxed
  iframes don't enforce sandbox flags (CreateChildFrame ignores frame_policy) — a clean follow-up.

- ✅✅ DONE: <iframe sandbox> flags enforced — shipped (2026-06-24): the last queued item of the
  iframe arc. CreateChildFrame now captures the owner's frame_policy.sandbox_flags onto the child
  MbFrameClient (SetSandboxFlags); BeginNavigation folds them into the committed policy container
  (params->policy_container->policies.sandbox_flags |= sandbox_flags_) and, if the kOrigin bit is
  set (sandbox w/o allow-same-origin), forces a fresh opaque origin
  (params->origin_to_commit = SecurityOrigin::Create(url)->DeriveNewOpaqueOrigin()) — exactly
  frame_test_helpers' BeginNavigation tail (lines 951-957). Header: new member
  network::mojom::WebSandboxFlags sandbox_flags_ + SetSandboxFlags; .cc includes
  services/network/public/cpp/web_sandbox_flags.h (bitwise operators) + .../mojom-shared.h +
  platform/weborigin/security_origin.h. VERIFIED: the document_loader.cc CalculateOrigin kOrigin
  block applies our origin (postcommit debug showed the sandboxed srcdoc child origin=[null]
  opaque=1, while a non-sandboxed sibling = file://). KEY FINDING: a cross-origin parent READ can't
  observe this here — the file:// parent has universal access (web-security-off / file-access
  settings), so it can read even an opaque child; origin enforcement still happens, just isn't
  observable that way. So smoke case 79 asserts the origin-INDEPENDENT signal: a sandboxed child's
  inline <script> does NOT run (kScripts), while a non-sandboxed sibling's does (data-ran probe).
  96/96, no survivors. The iframe arc (create -> srcdoc -> src= -> sandbox) is now complete.

- ✅✅ DONE: document.cookie read-first no longer hangs — fixed (2026-06-24): probing common web
  APIs found document.cookie READ hung (watchdog SIGKILL) when it was the FIRST cookie op from a
  page's inline script (the universal "read existing cookies on load" pattern). Root cause: the
  RestrictedCookieManager (MbCookieManager in mb_frame_broker.cc) was reached via a
  BrowserInterfaceBroker bound on the MAIN thread. GetCookiesString is a [Sync] mojo method; on a
  read-first, the main thread blocked in the sync wait BEFORE the async broker.GetInterface that
  binds the manager had been pumped, so the manager never bound → self-deadlock. (Case 33 passed
  only because it WRITES first, which pumps GetInterface and binds the manager on the main thread,
  after which a same-thread [Sync] read is serviceable.) FIX: bind the broker on the runtime
  service thread, mirroring the blob subsystem exactly — PostTask to MbRuntime::ServiceTaskRunner()
  and call MakeSelfOwnedReceiver INSIDE the posted task (NOT by passing a task_runner arg to
  MakeSelfOwnedReceiver from the main thread — that created the router on the main sequence and
  tripped interface_endpoint_client.cc:741 sequence_checker on the service thread). Now GetInterface
  is processed off-thread and GetCookiesString is serviced off-thread; read-first works. Knock-on:
  MbCookieManager (and its CookieStore map) now run on the service thread; the curl cookie share
  (mb_url_loader.cc CookieShare) gained CURLSHOPT_LOCK/UNLOCKFUNC callbacks (one base::Lock,
  NO_THREAD_SAFETY_ANALYSIS) so the document.cookie->curl-jar bridge (service thread) is safe vs
  network reads (calling thread). Smoke case 33b added (inline read-first asserts no hang + reads
  back the a=1/b=2 from case 33, same file:// origin). 97/97, no survivors.

- ✅✅ DONE: page-initiated main-frame navigation — shipped (2026-06-24): broad API probing (after
  the cookie fix) showed nearly everything works (localStorage, async fetch/XHR data:, observers,
  ES module graphs over the loader, dynamic import, FileReader, DOMParser, crypto.subtle, etc.) —
  but a page navigating ITSELF (link click / location.href= / form submit) did NOTHING. Cause:
  MbFrameClient::BeginNavigation early-returned for the main frame ("driven by MbWebView"); only the
  INITIAL document is driven by MbWebView (CommitHtml), page-initiated navs were dropped. FIX:
  refactored BeginNavigation to dispatch — child frames still commit synchronously (fires during
  parent parse; proven), the main frame POSTS the commit to web_frame_->GetTaskRunner(kInternalLoading)
  guarded by a WeakPtr (re-entrancy: main-frame navs fire from inside JS/event handling, so a
  synchronous re-commit is unsafe — frame_test_helpers also posts it). Extracted the shared commit
  body into DoCommit(info). Main frame only handles real navs (about:*/empty left alone). VERIFIED:
  mb_shot --click on an <a href> AND a location.href= onclick both land on pageB; smoke case 80
  (location.href= → new document commits, navB-here). 98/98, no survivors. LIMITATION: GET only
  (MbFetchUrl is GET) — POST form submit would fetch as GET; documented follow-up.

- ✅✅ DONE: POST form submission — shipped (2026-06-24): the documented follow-up to main-frame
  navigation. A method=post form was fetched as GET (body lost). FIX: MbFetchUrl/FetchHttp gained
  optional post_body + post_content_type params (curl CURLOPT_COPYPOSTFIELDS + Content-Type header;
  defaults to application/x-www-form-urlencoded); DoCommit detects HttpMethod()=="POST", extracts
  the body from info->url_request.HttpBody() (concatenating kTypeData elements via Element.data.Copy())
  and the content type, and passes them through. VERIFIED end-to-end: mb_shot --click submit on a
  form posting to https://postman-echo.com/post echoed {"form":{"user":"alice","x":"42"}} with the
  right content-type; smoke case 36 (gated MB_NET_TESTS) posts to <host>/post and confirms the field
  comes back — PASS against postman-echo. Default suite still 98/98 (POST path is isolated; no
  regression). LIMITATION: only kTypeData body elements (urlencoded/text forms) — multipart file
  uploads (kTypeFile/kTypeBlob) aren't assembled; documented follow-up.
- ⚠️ ENV FINDING (2026-06-24): mb's libcurl reaches REAL public hosts fine (example.com,
  postman-echo.com, httpbin.org) but TIMES OUT (curl=28) connecting to 127.0.0.1/localhost in this
  environment — a loopback-connect quirk of this sandbox/build, NOT a code bug. So network features
  must be verified against a public host (mb_shot or MB_NET_TESTS with the default
  MB_NET_HOST=https://httpbin.org), never a local echo_server. (The 127.0.0.1 echo_server path in
  the docs won't connect here.) Worth remembering before chasing "http hangs" again.

- ✅✅ DONE: fetch()/XHR request bodies (POST/PUT/etc.) — shipped (2026-06-24): MbURLLoader::Deliver
  always fetched http(s) as a bodyless GET — it ignored request->method and request->request_body,
  so every fetch()/XHR POST/PUT (the dominant SPA/API pattern) silently sent nothing. FIX: Deliver
  now extracts the body from request->request_body (concatenating kBytes DataElements via
  As<network::DataElementBytes>().AsStringView()) and the Content-Type from request->headers, and
  passes them plus request->method to MbFetchUrl. MbFetchUrl/FetchHttp gained an http_method param:
  a non-GET verb sets CURLOPT_CUSTOMREQUEST (so POST/PUT/PATCH/DELETE are correct, not all-POST),
  and a body sets CURLOPT_COPYPOSTFIELDS. VERIFIED end-to-end vs postman-echo (mb_shot): fetch POST
  JSON body echoed json.hello; XHR POST urlencoded echoed form.k; fetch PUT echoed data=putbody.
  Gated smoke case 37 (fetch POST) PASSES. Default suite 98/98 (loader GET/file/data path
  unchanged; method/body only engage for non-GET http). LIMITATION: only kBytes body elements
  (string/urlencoded/JSON) — multipart file uploads (kFile/kDataPipe) and per-request custom headers
  beyond Content-Type aren't forwarded yet; documented follow-ups.

- ✅✅ DONE: fetch()/XHR per-request headers forwarded — shipped (2026-06-24): MbURLLoader::Deliver
  forwarded only the host-level extra_headers_ (+ Content-Type), dropping a request's own headers,
  so fetch headers:{} / XHR setRequestHeader (Authorization, X-*, Accept, …) never reached the
  server — breaking authenticated API calls. FIX: Deliver now iterates request->headers
  .GetHeaderVector() and appends each "key: value" to the per-request header set passed to
  FetchHttp (Content-Type still carried via post_ct to avoid a duplicate header). VERIFIED vs
  postman-echo (mb_shot): a fetch with Authorization + X-Mb-Custom echoed back both (was MISSING
  before). Gated smoke case 38 (fetch custom header -> mbtok7) PASSES. Default suite 98/98.
  LIMITATION: still no multipart file upload (kFile/kDataPipe body elements). The fetch/XHR network
  surface (method + body + headers) is now complete for the common JSON/urlencoded API case.

- ✅✅ DONE: HTTP response status + headers exposed to JS — shipped (2026-06-24): MbURLLoader
  hardcoded SetHttpStatusCode(200) and only set Content-Type, and FetchHttp turned any non-2xx/3xx
  into a failure. So fetch saw a 404/500 as a rejected TypeError (not a Response with .status/.ok),
  and Response.headers.get()/XHR getResponseHeader returned nothing. FIX: (1) FetchHttp captures the
  final response's header block (CURLOPT_HEADERFUNCTION + a callback that resets on each HTTP/ status
  line, so redirects/retries leave only the last response) and the status code, via new out params;
  it now returns true for ANY complete HTTP response (incl. 4xx/5xx) — only a transport error (no
  response) is a failure. (2) MbURLLoader::Deliver sets response.SetHttpStatusCode(real) and parses
  the header block to SetHttpHeaderField each (skipping content-length/transfer-encoding which Blink
  derives). VERIFIED vs httpbin (mb_shot + smoke case 39): fetch /status/404 -> 404/ok=false (was
  TypeError); /response-headers?X-Smk=sv1 -> readable. Default suite 98/98. Knock-on: a 404
  navigation now commits the error page instead of blank — correct. Also made smoke case 38 match
  the header VALUE not the key (httpbin Title-Cases, postman-echo lowercases).
- ✅✅ DONE: document.cookie -> HTTP-jar bridge fixed (2026-06-24): gated net case 33 "cookie bridge"
  now PASSES (mbGetCookies shows mbjs=fromjs). ROOT CAUSE (not the service-thread move I'd guessed —
  that was a red herring): MbAddCookieToJar injected the cookie via CURLOPT_COOKIELIST as a
  "Set-Cookie: name=value" line with NO domain. curl can only infer the host for such a line from an
  ACTIVE transfer; MbAddCookieToJar does no transfer (just sets the option on a throwaway handle), so
  the host-less cookie was silently dropped. Diagnosed via an MB_VERBOSE read-back: right after
  injecting mbjs the handle listed only mbck (set earlier by a real Set-Cookie *response*), confirming
  the COOKIELIST line never stored. FIX: build a Netscape TSV line with an EXPLICIT domain
  (domain \t tailmatch \t path \t secure \t expiry \t name \t value) from the URL host + parsed
  cookie attrs (domain/path/secure, max-age=0/1970 -> past expiry to delete). Netscape format needs
  no transfer. Full gated net suite now 106/106 (also clears the earlier "request headers"/"custom
  headers" host-shape flakes). Default suite 98/98.

- ✅✅ DONE: character-encoding / non-UTF-8 pages — shipped (2026-06-24): a page in any non-UTF-8
  charset (latin-1, windows-1252, Shift_JIS, …) rendered as mojibake — <meta charset> was ignored
  and every byte force-decoded as UTF-8. CAUSE: MbWebView::CommitHtml used
  WebNavigationParams::CreateWithHTMLStringForTesting, which hardcodes FillStaticResponse(...,"UTF-8",
  ...) — an AUTHORITATIVE encoding, so the HTML parser's <meta>/BOM detection can't override. FIX:
  CommitHtml now builds params itself and calls FillStaticResponse with a `charset` arg that is empty
  by default (TENTATIVE — parser honors <meta charset>, BOM, and UTF-8 auto-detection); LoadURL
  parses the charset out of the HTTP Content-Type and passes it (authoritative when the server
  declares one). VERIFIED (mb_shot): latin1 'café crème', win1252 'price €100 "quoted"', Shift_JIS
  '日本' all decode; UTF-8 WITH and WITHOUT <meta> still correct (café / café 日本). Smoke cases 81
  (ISO-8859-1 via meta) + 82 (UTF-8 no-meta auto-detect) added — 100/100, no survivors.

- ✅ DONE: navigation redirects commit the final URL — shipped (2026-06-24): navigating to a URL
  that 3xx-redirects (http->https, www, shorteners, login flows) committed the document with the
  ORIGINAL URL as its base, so location.href was wrong and relative subresources resolved against
  the pre-redirect URL. curl follows the redirect internally; now FetchHttp exposes the effective
  URL (CURLINFO_EFFECTIVE_URL) via a new out_final_url param, MbFetchUrl forwards it, and
  MbWebView::LoadURL commits with that final URL as the document base. VERIFIED: mb_shot nav to
  httpbin /redirect-to?url=example.com renders Example Domain; gated smoke case 40 asserts
  location.href == <host>/get after a 302 (was the /redirect-to URL). Net suite 109/109, default
  100/100. SCOPED OUT (the harder sibling): fetch()/XHR response.url + response.redirected after a
  redirect still report the request URL — fixing those needs the loader to follow redirects MANUALLY
  and report each hop via WillFollowRedirect (Blink DCHECKs response.CurrentRequestUrl()==url_list_
  .back(), so you can't just rewrite the response URL — confirmed: it aborts at fetch_manager.cc:714).
  That's a sizable loader rework; deferred.
- ⚠️ KNOWN GAPS (probed 2026-06-24, all the documented heavy items; everything else works): fetch(blob:)
  fails (TypeError) — blob: URL resolution is the reverted BlobURLStore work; Web Workers are inert
  (new Worker never fires onmessage/onerror) — "real worker execution"; fetch redirect response.url/
  redirected (above). NOT gaps (verified working this tick): CSS calc/vw/@supports, TextDecoder,
  AbortController fetch-abort, pseudo-elements, tables, media queries, checkbox-click+change,
  animationend, custom elements/shadow DOM, designMode.

### REMAINING ROADMAP
- P0-history: page-driven history.back()/forward() does nothing — History::back() ->
  LocalFrameClientImpl::NavigateBackForward -> LocalFrameHost.GoToEntryAtOffset (mojo to the absent
  browser), and it's gated on WebViewImpl::HistoryBackListCount() (0 here). Needs an in-renderer
  session-history controller + intercepting the browser-owned LocalFrameHost — sizable, deferred.
- P1-polish: fonts/text (GetDataResource -> .pak + macOS system fonts).
- P2: wire the wke/mb C API surface onto this host; drive from port/mac/minibrowser_main.mm
  (Cocoa window) -> real sites in a window. JS is free (V8 isolate already up).
- P2-net: real navigation via a libcurl network::mojom::URLLoaderFactory (mb_url_loader) for
  mbLoadURL (currently a stub).
- P3: input/events. P4: GPU/onscreen compositing. P5: Windows host + API parity.

### (done) load + paint — see milestone above
mb_webview.cc currently creates view+frame but skips widget/attach. Add, in MbWebView::Create
after CreateMainFrame:
  1. Create the frame widget + InitializeCompositing, then web_view_->DidAttachLocalMainFrame();
     Resize(width,height). (Model: frame_test_helpers.cc CreateFrameWidgetAndInitializeCompositing
     :689 + InitializeCompositing :697. The widget needs the WidgetHost/FrameWidget mojo
     channels — use the no-op/null associated channels like TestWebFrameWidget; this is the
     fiddly part. See frame_test_helpers.h TestWebFrameWidgetHost / BindWidgetChannels.)
  2. mb_webview.cc LoadHTML: commit an in-memory doc. Easiest path:
     main_frame_->CommitNavigation with WebNavigationParams::CreateWithHTMLBufferForTesting /
     frame_test_helpers::LoadHTMLString(frame, html, base_url) pattern. Then PumpMessages.
  3. mb_widget.cc BeginFrameAndReadback: CompositeForTest + GetPaintRecord().Playback->SkBitmap.
  4. mb_smoke: LoadHTML("<h1 style='color:red'>hi</h1>"), paint, write PNG. ← P1 DONE.
ALSO add web_view_->Close() in ~MbWebView for clean view teardown (currently commented).

## (resolved) BRING-UP v2: real scheduler (FIXED), then cppgc/V8 heap mismatch — see milestone above
- REWROTE mb_runtime bring-up to the FULL path (mb_runtime.cc current):
  1 AtExit, 2 CommandLine, 3 ICU, 4 FeatureList, 5 mojo::core::Init, 6 ThreadPool,
  7 LoadV8Snapshot(kWithAdditionalContext), 8 blink::Platform::InitializeBlink(nullopt),
  9 WebThreadScheduler::CreateMainThreadScheduler(base::MessagePump::Create(UI)),
  10 blink::Initialize(platform,&binder_map, scheduler.get()), 11 CreateMainThreadIsolate.
  (Dropped SingleThreadTaskExecutor — the WebThreadScheduler owns the message pump.)
- The scheduler DCHECK is GONE — all 11 steps run, mbCreateView reaches isolate creation.
- NEW crash at step 11 (CreateMainThreadIsolate -> V8Initializer::InitializeMainThread):
  **FATAL thread_state.cc:143 (ThreadState::AttachToIsolate)
  CHECK_EQ(cpp_heap_, isolate->GetCppHeap()) — two DIFFERENT cppgc heaps.**
- What I know:
  * ThreadState::AttachMainThread (in InitializeBlink) does `new ThreadState(gin::V8Platform::Get(), marker)`
    which CREATES cpp_heap_ A.
  * V8Initializer::InitializeMainThread() (v8_initializer.cc:1045) creates the isolate via
    V8PerIsolateData::Initialize(..., ThreadState::Current()->ReleaseCppHeap()) — SHOULD reuse A.
  * blink::Initialize internally calls V8Initializer::InitializeIsolateHolder ->
    gin::IsolateHolder::Initialize (v8_initializer.cc:1021) — the gin/V8 platform+cppgc setup.
  * Yet the isolate ends up with a DIFFERENT heap B. content uses the SAME order
    (InitializeBlink before InitializeIsolateHolder) and works — so plain ordering isn't it.
- >>> NEXT TICK — empirical fixes to try (in order):
  (a) Get a real backtrace + nearby state: `sudo lldb` is now available (user enabled
      passwordless sudo) — `sudo DYLD_LIBRARY_PATH=$PWD lldb -b -o run -o bt -o quit ./mb_smoke`
      (or read the newest ~/Library/Logs/DiagnosticReports/mb_smoke-*.ips). Confirm WHICH
      call makes heap B (is V8PerIsolateData::Initialize ignoring ReleaseCppHeap? is there a
      2nd ThreadState?).
  (b) Try calling gin::IsolateHolder::Initialize / the gin V8 platform init BEFORE
      Platform::InitializeBlink so the cppgc heap is built on the initialized platform.
  (c) Check V8PerIsolateData::Initialize + ReleaseCppHeap: print cpp_heap_ ptr at each step
      via temporary fprintf in a LOCAL copy (can't edit blink, but can add a wrapper that logs
      ThreadState::Current()->... ). OR diff against exactly what content_main_runner +
      render_thread_impl do between InitializeBlink and CreateMainThreadIsolate (any V8 init
      step I omitted — e.g. gin::V8Initializer::Initialize(ScriptMode,...) at gin v8_initializer.h:32).
  (d) Consider: maybe must call gin::V8Initializer::Initialize(kNonStrictMode, ...) explicitly.

## HANDSHAKE: compiles+links; runtime crash in scheduler (2026-06-23 11:49)
- mb_webview.cc real WebView::Create + WebLocalFrame::CreateMainFrame COMPILE + LINK vs
  modern Blink (all 16 args, agent-group-scheduler, tokens, sandbox flags correct).
  - Needed include: "third_party/blink/public/mojom/page/prerender_page_param.mojom.h"
    (web_view.h only pulls the -forward.h → PrerenderParamPtr incomplete without it).
  - agent_group_scheduler_ member added to mb_webview.h.
- Runtime: mbCreateView reaches WebViewImpl ctor, then **FATAL
  web_private_ptr.h:253 DCHECK !IsNull() in WebAgentGroupScheduler::DefaultTaskRunner()**.
  My agent group scheduler wraps a NULL internal ptr.
- HYPOTHESIS: the simple `blink::CreateMainThreadAndInitialize` path yields a degenerate
  main-thread scheduler; `ThreadScheduler::Current()->ToMainThreadScheduler()
  ->CreateAgentGroupScheduler()` returns empty. SimTest works because
  ScopedUnittestsEnvironmentSetup builds a REAL MainThreadSchedulerImpl (manual init +
  Platform::CreateMainThreadForTesting), not the simple path.
- >>> NEXT TICK: fix the scheduler. Options to investigate:
  (a) Switch to full `blink::Initialize(platform, &binder_map, main_thread_scheduler)`
      with a real `blink::scheduler::WebThreadScheduler` (create via
      blink::scheduler::WebThreadScheduler::CreateMainThreadScheduler() — find exact API).
      Then keep the SingleThreadTaskExecutor as the underlying message pump.
  (b) OR check whether mbInitialize must create the main-thread scheduler BEFORE
      CreateMainThreadAndInitialize and pass it in.
  Reference: how content/renderer/render_thread_impl or
  third_party/blink/renderer/controller blink test main builds the main thread scheduler.
  Verify fix: mb_smoke mbCreateView returns non-null, no DCHECK.

### NEXT: the render path (P1 finish)
1. mb_webview.cc: uncomment + fix the real WebView::Create (frame_test_helpers.cc:778) +
   WebLocalFrame::CreateMainFrame (:489). Needs a WebAgentGroupScheduler — get it from the
   main thread scheduler (blink::scheduler::WebThreadScheduler::CreateForTesting? or
   Thread::MainThread()->Scheduler()->CreateAgentGroupScheduler()). Pin via compiler.
2. mb_widget.cc: create the frame widget + InitializeCompositing; AllocateNewLayerTreeFrameSink.
3. mb_webview LoadHTML: commit in-memory doc (WebNavigationParams::CreateWithHTMLString-ish);
   PumpMessages until DidStopLoading; PaintToBitmap via GetPaintRecord().Playback -> SkBitmap.
4. Extend mb_smoke: create view, LoadHTML("<h1 style=color:red>hi</h1>"), paint, write PNG.
   ← P1 DONE = first modern-Blink-rendered pixels.
Use the SAME compile-fix loop (cp canonical→donor, ninja mb_smoke, run, read .ips on crash).

## RUNTIME BRING-UP IN PROGRESS (2026-06-23 11:37)
- mb_smoke exe (test/mb_smoke.cc) links + RUNS vs the dylib. Bring-up reaches step 8.
- Steps 1-7 + 6b(ThreadPool) WORK: AtExit, CommandLine::Init(0,nullptr), InitializeICU
  (icudtl.dat present in out/Release), FeatureList, mojo::core::Init,
  SingleThreadTaskExecutor(UI), ThreadPoolInstance::CreateAndStartWithDefaultParams,
  new MbPlatform. All good.
- **CRASH at step 8 = blink::CreateMainThreadAndInitialize.** Backtrace (from
  ~/Library/Logs/DiagnosticReports/mb_smoke-*.ips — lldb is perm-blocked, use the .ips):
    TimeZoneController::Init() -> Platform::GetBrowserInterfaceBroker()->GetInterface()
    -> base::Lock::Acquire on a NULL broker -> EXC_BAD_ACCESS @0x20.
- **ROOT CAUSE: MbPlatform::GetBrowserInterfaceBroker() returns nullptr (my stub).**
  Blink core init dereferences it. Must return a real EMPTY broker.
- >>> NEXT TICK FIX: implement `MbBrowserInterfaceBroker : public
    blink::ThreadSafeBrowserInterfaceBrokerProxy` with a no-op GetInterface (drop the
    receiver) + whatever other pure-virtuals it has; return it from
    GetBrowserInterfaceBroker(). Reference impl: `TestingBrowserInterfaceBroker` in
    vendor/reference/testing_platform_support.cc. Read its class for the exact vtable.
    Then re-run mb_smoke; expect to advance past step 8 (maybe hit the NEXT missing piece,
    e.g. V8 snapshot for step 9 CreateMainThreadIsolate — snapshot_blob.bin in out/Release).
- Keep the step-marker fprintfs + ThreadPool init; they're load-bearing/useful.

### NEXT PHASE: flesh bodies to actually render (P1 real work)
Order (each its own compile-fix mini-loop; keep the dylib green after each):
1. mb_runtime.cc — real bring-up: AtExit, CommandLine, ICU (vendor icudtl.dat OR
   base::i18n::InitializeICU from the donor's data), discardable alloc, FeatureList,
   mojo::core::Init, SingleThreadTaskExecutor, CreateMainThreadAndInitialize(platform,
   &empty_binder_map), CreateMainThreadIsolate. Write a tiny main() test that calls
   mbInitialize() and asserts it returns 1 (link a small exe vs the dylib).
2. mb_platform.cc — fill GetDataResource (UA stylesheet etc.) — likely needs the donor's
   resource .pak (content_resources / blink_resources). Find & load it.
3. mb_webview.cc — the real WebView::Create + CreateMainFrame handshake (uncomment, fix
   exact args vs frame_test_helpers.cc:778/489). + agent_group_scheduler from runtime.
4. mb_widget.cc — widget create + InitializeCompositing + BeginFrameAndReadback via
   GetPaintRecord().Playback into SkBitmap.
5. mb_webview.cc LoadHTML — commit an in-memory doc; pump; PaintToBitmap → PNG. ← P1 DONE.
6. THEN mb_url_loader.cc libcurl factory for LoadURL.

### compile/run helper
  cd chromium-150.0.7871.24 && PATH=buildtools/mac:$PATH ninja -C out/Release miniblink_host
  (rebuilds in seconds now — only our 9 files). Errors → /tmp/mbhost_build.log.
SYNC: edit canonical (miniblink-modern/src/miniblink_host/*), cp changed file to
  third_party/blink/renderer/miniblink_host/* before ninja. (Donor BUILD.gn has an extra
  include_dirs=[//third_party/blink/renderer] shim — keep that donor-only.)

## COMPILE-FIX LOOP IS LIVE (since 2026-06-23 11:27)
- Build boqyrpe51 KILLED (was not critical path).
- **Donor build-sandbox location: `chromium-150.../third_party/blink/renderer/miniblink_host`**
  (MUST be under blink/* — renderer-internal targets + `//third_party/blink/renderer:config`
  have `visibility = [//third_party/blink/*]`. Top-level //miniblink_host was REJECTED.)
- BUILD.gn shim: `include_dirs = [ "//third_party/blink/renderer" ]` so `#include
  "miniblink_host/..."` still resolves from the new location.
- Wired into root BUILD.gn gn_all: `deps += [ "//third_party/blink/renderer/miniblink_host" ]`.
- **gn gen SUCCEEDS** (30981 targets) — BUILD.gn valid, all dep labels + visibility OK. ✅
- SYNC RULE: canonical = miniblink-modern/src/miniblink_host. Donor sandbox = the blink/renderer
  copy. Edit either, but COPY donor fixes BACK to canonical (and re-cp canonical→donor before gen).
- First compile RUNNING: bg **b35gfgpdi**, log /tmp/mbhost_build.log (+ tasks/b35gfgpdi.output).
  May build blink deps first (blink_unittests build was interrupted mid-way).

### compile-fix loop steps (repeat each tick)
1. Check b35gfgpdi (or rerun `ninja -C out/Release miniblink_host`).
2. Read FIRST error in /tmp/mbhost_build.log. Fix in the donor sandbox file (include path,
   base class, signature) using the real blink header. Copy fix back to canonical.
3. Re-run ninja. Repeat. Goal: COMPILES + LINKS against blink (stub bodies OK) = seam proven.
4. Then flesh bodies (handshake, paint readback, libcurl factory) for the render.

## (superseded) earlier pivot note kept for reference:
The build (boqyrpe51) is NOT critical path (it's just the render-proof smoke; blink
component libs //miniblink_host needs are already built). To get the compiler pinning
signatures, do this next tick:
  1. Kill build: TaskStop boqyrpe51 (or pkill -f 'ninja -C out/Release'). It's been ~40min,
     slow, and holds the build dir lock we need.
  2. Stage host into donor tree so GN can reach blink:
       cp -R /Users/yangxin/dennis/chrome/miniblink-modern/src/miniblink_host \
             /Users/yangxin/dennis/chrome/chromium-150.0.7871.24/miniblink_host
     (canonical home stays the standalone project; donor copy is the build sandbox.
      After each compile-fix round, copy fixes BACK to the standalone project.)
  3. Make GN see it: add to root BUILD.gn group("gn_all") deps OR just gen+build directly:
       cd chromium-150.0.7871.24
       PATH=buildtools/mac:$PATH gn gen out/Release
       ninja -C out/Release miniblink_host  2>&1 | tee /tmp/mbhost_build.log
  4. The FIRST errors = the real TODO list. Fix include paths + base classes + signatures
     against the real blink headers, iterate. This replaces guessing with compiler truth.
  5. Milestone: //miniblink_host COMPILES + LINKS against blink (even with stub bodies) =
     proves the seam. THEN flesh bodies for the render.
- KEY: blink::Platform override set is SMALL (~6, per TestingPlatformSupport). Init seam =
  CreateMainThreadAndInitialize(Platform*, BinderMap*) + CreateMainThreadIsolate;
  init-order template = ScopedUnittestsEnvironmentSetup.
- ⚠️ EXPECTED diagnostics: mb_platform.h "blink header not found" — just missing include
  paths; NOT a bug. Resolves when //miniblink_host GN target is wired. Don't "fix" it.

## NEXT ACTION (what the next iteration should do)
1. Check bg build `boqyrpe51` (signal = TOTAL objs since 10:40, not last-N-min).
   - If SUCCEEDED → `out/Release/blink_unittests --gtest_filter=*Sim*` smoke (P0.2). THEN
     it is safe to `gn gen` → wire `//miniblink_host` GN target in the donor tree (add
     BUILD.gn from phase-1-spec; depend on //third_party/blink/renderer/controller etc.)
     and start compiling mb_platform.cc/mb_runtime.cc (write the .cc stubs).
   - If still running → DON'T touch donor GN graph. Continue writing host .cc/.h scaffolds
     + the BUILD.gn file content (as a draft) under src/miniblink_host/; flesh out
     mb_webview.h, mb_frame_client.h, mb_widget.h, loader/mb_url_loader.h headers.
   - If FAILED → triage, fix args.gn/deps, restart.
2. Keep this file current. One fact per change.

## Log
- 2026-06-23 10:40 — P0 started. Build env already configured (46k objs precompiled).
  Kicked off blink_unittests build (boqyrpe51). Scaffolded project + this tracker.
- 2026-06-23 10:42 — User: use modern Chromium folder structure. Re-rooted project to
  `src/` mirroring chromium layout (copied code keeps original paths; host at
  `src/miniblink_host/`). Build boqyrpe51 still ramping.
- 2026-06-23 10:47 — Loop tick. Build healthy but slow (no-PCH). **Mapped the full
  minimal render surface** → docs/interface-surface.md. Major de-risk: no browser process
  / no Mojo hosts needed for static render (all null/no-op). Confirms Path A feasible.
  Next: Phase 1 host spec + faster render proof.
- 2026-06-23 10:52 — Loop tick. Diagnosed build = healthy (796 objs done, no swap, heavy
  core/ TUs in flight) — false-alarm corrected. Wrote **docs/phase-1-spec.md** (host
  layout + C ABI + standalone copy/vendor strategy). Next: scaffold //miniblink_host
  while build finishes; smoke-test SimTest when binary lands.
- 2026-06-23 10:57 — Loop tick. Build still running (829 objs). Copied blueprint files to
  vendor/reference/. Confirmed Platform override set is small. Wrote header scaffolds:
  mb_capi.h (full C ABI), mb_platform.h, mb_runtime.h. Didn't touch donor GN graph (build
  in flight). Next: more host headers / .cc, or wire GN once build done + SimTest smoke.
- 2026-06-23 11:02 — Loop tick. Build 884 objs (still going; STOPPED gating on render proof
  — SimTest is CI-proven). Wrote mb_frame_client.h, mb_url_loader.h. **NETWORKING
  CORRECTION** (recorded in interface-surface.md): production loads use
  network::mojom::URLLoaderFactory via LocalFrameClientImpl::GetURLLoaderFactory(), NOT a
  plain URLLoader hook (that's test-only, CreateURLLoaderForTesting). P1 net = minimal
  libcurl-backed mojom::URLLoaderFactory. class URLLoader =
  renderer/platform/loader/fetch/url_loader/url_loader.h (non-public; host reaches into
  renderer/). Next: mb_widget.h + mb_webview.h headers, draft BUILD.gn.
- 2026-06-23 11:07 — Loop tick. Build 909 objs. Extracted widget/compositor surface from
  TestWebFrameWidget + SimCompositor. Wrote widget/mb_widget.h, view/mb_webview.h, and a
  DRAFT BUILD.gn (component target, deps on blink public+renderer internals+net mojom).
  Next: mb_view_client.h + mb_sw_frame_sink.h headers; then start .cc files.
- 2026-06-23 11:12 — Loop tick. Build 1175 objs (accelerating). Wrote final headers
  mb_view_client.h + mb_sw_frame_sink.h. **DEEPEST RISK (compositor/pixels) RESOLVED**:
  readback via GetPaintRecord().Playback into SkBitmap, no viz renderer (recorded in
  interface-surface.md + mb_widget.h). All structural headers complete. Next: start .cc
  files (begin mb_capi.cc + mb_runtime.cc + mb_platform.cc).
- 2026-06-23 11:17 — Loop tick. Build 1214 objs. Read ScopedUnittestsEnvironmentSetup ctor
  → captured real init sequence. Wrote capi/mb_capi.cc (complete C ABI dispatcher) +
  runtime/mb_runtime.cc (scaffold). Bring-up spine started. (All "file not found"
  diagnostics are the known unset-include-root noise — not bugs.) Next: more .cc
  (mb_platform.cc, mb_webview.cc); wire GN when build idle.
- 2026-06-23 11:22 — Loop tick. Build 1230 objs (crawling). Wrote mb_platform.cc +
  mb_webview.cc (substantive scaffolds) + 5 stub .cc. **FULL SOURCE SET COMPLETE (19
  files).** Decided build is NOT critical path. PIVOT planned for next tick: kill build,
  stage host into donor tree, gn gen, compile → let compiler drive signature-pinning.
- 2026-06-23 11:37 — Loop tick. Fleshed mb_runtime bring-up (compiled first try). Built+ran
  mb_smoke. Bring-up reaches step 8 then SIGSEGV. Got backtrace from the .ips crash report
  (lldb perm-blocked). ROOT CAUSE: MbPlatform::GetBrowserInterfaceBroker()==nullptr,
  deref'd by TimeZoneController::Init during CoreInitializer. Next: implement empty broker.
- 2026-06-23 15:56 — Loop tick. **🎉 KEYBOARD INPUT** — mbSendText types into focused
  element (kRawKeyDown+kChar+kKeyUp). Suite case 10: type into <input>, verify .value. 10/10.
  Click + type => forms fillable. Committed.
- 2026-06-23 15:48 — Loop tick. **🎉 INPUT EVENTS** — mbSendMouseClick via
  WebFrameWidgetImpl::HandleInputEvent. Suite case 9: click button -> onclick -> DOM change
  verified. Engine is now drivable (renders + responds). Committed.
- 2026-06-23 15:38 — Loop tick. Validated form controls + theming (input/checkbox/radio/
  button/select/textarea/range/progress all render native-styled). No code change.
  docs/demos/form-controls.png. Committed doc/demo updates.
- 2026-06-23 15:30 — Loop tick. Validated CJK/i18n: ja.wikipedia renders Japanese via
  CoreText font fallback (no code change). baidu = 227-byte stub (not a bug). docs/demos/
  japanese-wikipedia.png. + committed initial repo (64f098b) last tick.
- 2026-06-23 15:18 — Loop tick. **🎉 react.dev FIXED** — image.decode() on raster images
  re-entered DecodeRequestFinished synchronously mid-EraseIf (no-compositor sync callback).
  Patched WebFrameWidgetImpl::RequestDecode to PostTask the callback async. ~19/20 hard sites.
- 2026-06-23 15:08 — Loop tick. Broad sweep ~18/20 (github/stackoverflow/apple/vuejs/
  tailwind/web.dev/go.dev/developer.chrome all render). react.dev crashes in image.decode()
  microtask (deeper null, recorded). 3rd compositor-assumption crash → DESIGN DECISION
  logged: switch to InitializeCompositing + software frame sink (vs per-crash guards).
- 2026-06-23 14:54 — Loop tick. **🎉 10/10 SITES — scroll-listener crash fixed.** python.org
  (+any scroll listener) crashed: SetHaveScrollEventHandlers derefs null LayerTreeHost on the
  non-compositing widget. 1-line blink null-guard (patches/0001-...patch, applied by build.sh).
  docs/demos/python-org.png. Sweep now 10/10.
- 2026-06-23 14:46 — Loop tick. **🎉 FETCH() WORKS — MDN renders; 9/10 sites.** Switched body
  delivery to a real Mojo data pipe (+ ScopedIPCSupport IO thread + body task runner). Fixes
  fetch()-using sites. docs/demos/mdn.png. python.org still crashes (addEventListener) — next.
- 2026-06-23 14:38 — Loop tick. **Robustness sweep: 5/5 real sites render.** Fixed 2 crashes
  found on real sites: network-state init (navigator.onLine) + media-controls pak (<video>).
  gnu.org (with video) now renders → docs/demos/gnu-org.png.
- 2026-06-23 14:30 — Loop tick. **🎉🎉🎉 HACKER NEWS renders** — proved subresource HTTP
  loading end-to-end: fetched news.css/hn.js/y18.svg/s.gif/triangle.svg over HTTPS, full
  front page rendered correctly (docs/demos/hacker-news.png). Real complex website works.
- 2026-06-23 14:24 — Loop tick. **🎉🎉 LIVE WEBSITES RENDER over HTTP(S).** Unblocked via
  SYSTEM libcurl (macOS SDK; libs=["curl"]). FetchHttp + MbFetchUrl; LoadURL fetches http
  top-level; loader fetches http subresources. mb_shot https://example.com → real render
  (docs/demos/live-website.png). The biggest roadmap item is DONE.
- 2026-06-23 14:16 — Loop tick. Rewrote mb_smoke as a proper **8-case assertion test suite**
  (getComputedStyle/getImageData/eval/pixel checks + PASS/FAIL + exit code). All 8 PASS:
  HTML/DOM, JS, CSS computed, UA sheet, mbRunJS+mbEvalJS, canvas getImageData, external
  <link> CSS, paint-to-bitmap. A real regression test for the standalone project.
- 2026-06-23 14:10 — Loop tick. Added **mbEvalJS** (host <- page: eval JS, read string
  result back via ExecuteScriptAndReturnValue + v8 HandleScope/Context + Utf8Value).
  Verified: returned "mbRunJS drove the page | 42" (DOM state + JS compute). Host<->page
  scripting bridge now bidirectional.
- 2026-06-23 14:04 — Loop tick. Added **mbRunJS** (host-driven scripting) to the C ABI —
  WebLocalFrame::ExecuteScript(WebScriptSource). Verified: host-injected JS changed bg+text
  post-load ("mbRunJS drove the page" on green). Real embedding capability (cf. wkeRunJS).
- 2026-06-23 13:58 — Loop tick. Polish: gated all debug logging ([mb_runtime] step spam +
  [mb_url_loader]) behind the MB_VERBOSE env var. mb_shot is now silent by default,
  debuggable via MB_VERBOSE=1. Production-clean.
- 2026-06-23 13:52 — Loop tick. **🎉 mb_shot CLI** — headless HTML→PNG renderer (the
  deliverable tool). Added mbSavePng (gfx::PNGCodec), refactored PaintInto, executable target.
  Rendered /tmp/demo.html → polished PNG (docs/demos/mb_shot.png). Engine is now a usable product.
- 2026-06-23 13:40 — Loop tick. **🎉 CANVAS 2D works** — getContext('2d') + fillRect/arc/
  gradient/fillText render via skia. Saved docs/demos/canvas-2d.png; updated README matrix.
- 2026-06-23 13:32 — Loop tick. **🎉 EXTERNAL <link> CSS APPLIES** — root cause was the
  browser-side MimeRegistry (file: stylesheets need ext->text/css). Implemented in-process
  MbMimeRegistry + bound it in the broker. Subresource loading now COMPLETE (img + css).
- 2026-06-23 13:18 — Loop tick. **🎉 SUBRESOURCE LOADING WORKS** — external SVG <img>
  loads via MbURLLoader + renders (verified). Fixes: SetLoadsImagesAutomatically(true),
  DidReceiveDataForTesting body delivery, interleaved lifecycle+pump rounds, Content-Type
  header. Remaining: external <link> CSS fetched but not applied (activation-specific) —
  plan recorded. Images/fonts/JS/file-load all work.
- 2026-06-23 13:05 — Loop tick. Implemented MbURLLoader (file-backed blink::URLLoader),
  wired via MbFrameClient::CreateURLLoaderForTesting. **Confirmed invoked by blink's
  production loader path + reads files** (`[mb_url_loader] ...style.css -> OK (207 bytes)`).
  External CSS not yet applied (body-application detail) — diagnostic plan recorded above.
- 2026-06-23 12:50 — Loop tick. **Packaged the STANDALONE PROJECT**: README.md (+demos),
  build.sh (one-command build, verified end-to-end), docs/demos/ screenshots. Directly
  addresses the "standalone project" ask. Next: HTTP loader / window.
- 2026-06-23 12:43 — Loop tick. **🎉 FILE LOADING + IMAGE/SVG DECODE.** mbLoadURL(file://)
  reads+commits a disk file; inline SVG data-URI <img> rendered (concentric circles) in a
  flex row. New subsystems validated: file nav + image decode + SVG. Next: HTTP loader / window.
- 2026-06-23 12:37 — Loop tick. **🎉 MODERN CSS RENDERS** (grid/flexbox/gradient/radius/
  shadow/transform) — the upgrade payoff, screenshotted. Fix: trivial inline
  DiscardableMemoryAllocator (gradient needs it; testonly dep blocked, so hand-rolled).
- 2026-06-23 12:31 — Loop tick. **🎉 JAVASCRIPT WORKS.** Enabled SetJavaScriptEnabled(true);
  inline <script> mutated bg->blue + text->"JS WORKS", re-rendered. /tmp/mb_out.png confirms.
  Full V8->DOM->style->layout->paint loop proven. Next: P2 — wke C API + Cocoa window.
- 2026-06-23 12:27 — Loop tick. **🎉 TEXT RENDERS: "hello modern blink" green-on-red bold.**
  Wired ui::ResourceBundle(blink_resources.pak) + GetDataResource forwarding (UA stylesheet)
  and default font families/sizes on WebView settings (Times/Helvetica). /tmp/mb_out.png
  confirms styled text. P1 fully polished. Next: P2 — wke C API + Cocoa host window.
- 2026-06-23 12:20 — Loop tick. **🎉 P1 COMPLETE: modern Blink renders HTML to a bitmap.**
  Implemented LoadHTML (CommitNavigation of an in-memory doc) + PaintToBitmap
  (UpdateAllLifecyclePhases + GetPaintRecord().Playback -> SkBitmap). mb_smoke RENDER OK,
  /tmp/mb_out.png = solid red (inline-CSS bg) confirmed. Fixes: WebURL via KURL (INSIDE_BLINK),
  WebURL header in public/platform. KNOWN GAP: text/fonts (GetDataResource empty). Next:
  fonts, then wke C API + macOS host (P2).
- 2026-06-23 12:11 — Loop tick. Added the frame widget via the NON-COMPOSITING path
  (InitializeFrameWidget no-op channels + InitializeNonCompositing + Resize +
  DidAttachLocalMainFrame). Fixed compositing_enabled=false + DefaultLocale="en-US"
  (layout/font crash). **mbCreateView clean RC=0; widget attached, layout runs.** Next:
  LoadHTML + GetPaintRecord readback → first PNG.
- 2026-06-23 12:04 — Loop tick. NAILED the cppgc heap mismatch: blink::Initialize already
  creates the isolate; removed the duplicate CreateMainThreadIsolate; +scheduler Shutdown on
  teardown. **mbCreateView SUCCEEDS — WebView + main LocalFrame created, clean exit.** Next:
  widget + LoadHTML + paint readback → first pixels.
- 2026-06-23 11:54 — Loop tick. Rewrote bring-up to the full path (real WebThreadScheduler
  + blink::Initialize + Platform::InitializeBlink). **Scheduler DCHECK FIXED** — all 11
  steps run, mbCreateView reaches isolate creation. New crash: cppgc/V8 heap mismatch at
  thread_state.cc:143 (CreateMainThreadIsolate). Deep V8 integration issue; diagnosis +
  empirical fix plan recorded above. Real progress: view-creation scheduler blocker gone.
- 2026-06-23 11:49 — Loop tick. Wrote the real WebView::Create + CreateMainFrame handshake
  in mb_webview.cc → COMPILES + LINKS (fixed PrerenderParam incomplete-type via mojom.h
  include). Runtime: mbCreateView reaches WebViewImpl ctor then DCHECK !IsNull() in
  WebAgentGroupScheduler::DefaultTaskRunner — null agent scheduler from the simple init
  path. Next: real main-thread scheduler (full blink::Initialize). Diagnosis recorded above.
- 2026-06-23 11:42 — Loop tick. Implemented MbEmptyBroker (no-op GetInterfaceImpl) → past
  the null-deref. Next crash: V8 context-snapshot DCHECK. Added
  configs+=//v8:external_startup_data + deps+=//gin + LoadV8Snapshot(kWithAdditionalContext).
  **🎉 mbInitialize() == 1 — modern Blink + V8 isolate up in-process.** Runtime bring-up
  COMPLETE. Next: the WebView::Create handshake → first render.
- 2026-06-23 11:32 — Loop tick. EXECUTED PIVOT: killed blink_unittests build; staged host
  under //third_party/blink/renderer/miniblink_host (visibility!); include_dirs shim;
  wired into gn_all; **gn gen SUCCEEDS**; first compile → 7/9 objs built immediately, then
  fixed 3 trivial -Werror nits → **🎉 libminiblink_host.dylib BUILDS + LINKS vs modern
  Blink.** Architecture + seam PROVEN. Next: flesh mb_runtime bring-up (+ tiny exe test),
  then handshake + paint readback toward first render.
