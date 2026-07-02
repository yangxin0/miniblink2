# miniblink2

A **standalone, single-process embedder of modern Blink** (Chromium M150 / V8 15) — a
hand-written tiny "content layer" that boots the real Blink engine in-process and renders
HTML/CSS/JavaScript through a small C ABI. **No CEF**, no separate browser process, no
cross-process Mojo IPC.

It is the spiritual successor to [miniblink49](https://github.com/weolar/miniblink49)
(whose Blink froze at ~M47/2015), rebuilt against the M150 engine. The old miniblink
embedding model — call straight into `WebViewImpl` — no longer exists in modern Blink
(everything routes through Mojo + `//content`). This project provides the *minimum* host
that satisfies modern Blink so it runs without the full browser.

**The deliverable is a self-contained SDK**: one `libminiblink2.dylib` (or one complete
`libminiblink2.a`) + C headers + the engine's runtime data. An app links one library and
gets the whole modern web platform — GPU-accelerated WebGL on Metal included
(`map.baidu.com` and MapLibre GL render their full vector maps).

> New here? `include/miniblink2/wke.h` (classic miniblink API) and
> `include/miniblink2/mb_capi.h` (the native ABI) are the two public headers.
> Quick start below; `PROGRESS.md` has the development journal.

---

## Quick start

Prerequisites (see [Requirements](#requirements)): a configured Chromium M150 checkout and
depot_tools as **siblings** of this repo, plus full Xcode (for the Metal shader compiler).

```sh
# 1. Build the self-contained SDK -> dist/release/
scripts/build-lib.sh --both --release --size-optimized

# 2. Build the sample browsers against it
scripts/build-samples.sh --both

# 3. Run (from the dist dir — runtime data is loaded from beside the binary)
cd dist/release
./minibrowser_dyn    https://map.baidu.com     # links the .dylib (89 KB app)
./minibrowser_static https://map.baidu.com     # fully self-contained (no dylib dep)
```

A Cocoa window opens and renders the page — WebGL vector maps run on the **Metal GPU**.

## The SDK (`dist/<release|debug>/`)

| File | What it is |
|---|---|
| `libminiblink2.dylib` | the whole engine as ONE shared library (exports `wke*` + `mb*`) |
| `libminiblink2.a` | the same engine as ONE complete static archive |
| `include/miniblink2/{wke.h, mb_capi.h}` | the two public C headers |
| `blink_resources.pak`, `media_controls_…pak` | Blink resources (UA stylesheet, …) |
| `icudtl.dat`, `v8_context_snapshot.bin` | ICU data + the V8 context snapshot |
| `libEGL.dylib`, `libGLESv2.dylib` | **ANGLE** — the GL/WebGL driver (→ Metal) |
| `libvk_swiftshader.dylib`, `vk_swiftshader_icd.json` | software-GL fallback (optional) |
| `minibrowser_dyn`, `minibrowser_static`, `mb_shot` | sample apps (see below) |

Deployment rules:

- The runtime data files load **from the executable's directory** — ship them next to
  your binary.
- The ANGLE dylibs are **`dlopen()`ed at runtime** (Chromium's standard GL loading), so
  they never appear in `otool -L` — but WebGL is unavailable without them. The
  SwiftShader pair is only needed if you run with `--use-angle=swiftshader`
  (headless CI / GPU-less VMs); desktop-only distributions can drop those ~20 MB.
- The `--size-optimized` `.a` contains **ThinLTO bitcode**: link it with an LTO-capable
  toolchain (`scripts/build-samples.sh` shows the exact invocation — Chromium's
  `clang++` + `lld`).

### `scripts/build-lib.sh` — building the SDK

```sh
scripts/build-lib.sh [--shared|--static|--both] [--release|--debug]
                     [--size-optimized] [--webgpu] [--video] [--ml] [--wasm]
                     [--av1-encode] [--tracing] [--swiftshader] [--icu-full]
                     [--chromium DIR] [--depot DIR] [--no-stage] [--print-only]
```

| Flag | Effect | Size impact (release dylib, stripped) |
|---|---|---|
| *(default dev build)* | fast `-O2`, DCHECKs on, no LTO | 183 MB |
| `--size-optimized` | **ship build**: ThinLTO + `-Oz` + ICF + DCHECKs off | **88 MB** (`.a`: 1.9 GB ThinLTO bitcode) |
| `--webgpu` | include WebGPU (Dawn); off = Dawn completely absent from the binary | +~9 MB |
| `--video` | include `<video>` decode (H.264/ffmpeg-video). Off = **audio still plays** (miniblink49 parity) | +~8 MB |
| `--wasm` | include WebAssembly (V8 wasm engine). Off = `window.WebAssembly` absent (miniblink49 parity) | +~4.5 MB |
| `--ml` | include WebNN on-device ML (TFLite/LiteRT/XNNPACK) + the TFLite language-detection model + WebRTC's neural echo estimator (patches 0019/0021) | +~4 MB |
| `--av1-encode` | include AV1 *encoding* (libaom: WebCodecs/MediaRecorder/WebRTC send; decode always in) | +~1 MB |
| `--tracing` | include OPTIONAL_TRACE_EVENT instrumentation (perf-investigation builds) | +~0.5 MB |
| `--swiftshader` | ship SwiftShader software Vulkan in `dist/` (headless/CI/no-GPU `--use-angle=swiftshader`) | +20 MB (dist) |
| `--icu-full` | ship untrimmed `icudtl.dat` (all ~90 locales); default trims to root+en+zh (`MB_ICU_KEEP=en,zh,ja` to customize) | +4.1 MB (dist) |

Feature flags are include-only and **default off** — the default SDK is the trimmed,
miniblink49-like profile (audio on; video/webgpu/ml/wasm off). For comparison: miniblink49
ships 78 MB *unstripped* (~53 MB stripped); this is the full 2026 engine at ~1.7× its size.
Per-component size attribution for further pruning: `nm -n <out>/libminiblink2.dylib |
scripts/sizemap.py` (see `BACKLOG.md` §E for the measured, deliberately-not-cut leftovers).

The first build of a mode is a full engine compile (slow); re-runs are incremental
(staging uses rsync, builds are lock-serialized, flag flips rebuild only what changed).

### `scripts/build-samples.sh` — sample apps against the SDK

```sh
scripts/build-samples.sh [--dyn|--static|--both] [--release|--debug]
```

- `minibrowser_dyn` — a Cocoa mini-browser (toolbar + address bar) linking the `.dylib`.
  The app itself is **89 KB**.
- `minibrowser_static` — the same browser statically linked: a **110 MB single binary**
  with zero engine dylib dependencies.

## GPU architecture (macOS)

```
2D  (Skia Ganesh)  ─┐
3D  (WebGL 1+2)    ─┼──► GL ES ──► ANGLE ──► Metal (GPU)
                    │                └─ or SwiftShader (CPU) via --use-angle=swiftshader
WebGPU (--webgpu)  ────► Dawn ──► Metal
```

- WebGL runs on the **real GPU** through ANGLE's Metal backend
  (`ANGLE (Apple, ANGLE Metal Renderer: Apple M4 Pro …)`) — verified end-to-end with
  Baidu Maps and MapLibre GL rendering full vector maps (tiles parsed in `blob:` Web
  Workers, drawn via WebGL, composited into the page).
- Without `--webgpu`, **Dawn is completely absent** — no code, no symbol references, no
  runtime probing (`patches/0017`); `navigator.gpu.requestAdapter()` resolves null.
- Canvases paint **inline** (no display compositor): the drawing buffer is snapshotted
  into the software page paint (`patches/0008` + `0018`), which is what makes WebGL
  content appear in `wkePaint`/`mbPaintToBitmap`/`mb_shot` screenshots.

## What works (verified by a 400+ case automated battery)

| Subsystem | Status |
|---|---|
| Engine boot in-process: V8 isolate + Oilpan/cppgc + main-thread scheduler | ✅ |
| HTML parsing, UA stylesheet, CSS cascade; fonts + text via CoreText | ✅ |
| **Modern CSS**: Grid, Flexbox+gap, gradients, transforms, `:has()`, nesting, `@container`, `color-mix()`, `oklch()` | ✅ |
| **Web Components**: Custom Elements v1 + Shadow DOM | ✅ |
| JavaScript (V8) + DOM mutation → style recalc → relayout → repaint | ✅ |
| `<canvas>` 2D; image decode + SVG (data: URIs and external files) | ✅ |
| **HTTP/HTTPS** via vendored libcurl (WebSocket-enabled); subresource loading | ✅ |
| Paint readback to BGRA bitmap + PNG/JPEG; **PDF export** (paginated) | ✅ |
| Input events (click/move/type/scroll/drag, sub-frame routing), HiDPI, custom UA | ✅ |
| DOM storage, cookies (jar + `document.cookie`), History/Navigation API | ✅ |
| `requestAnimationFrame`; Mutation/Intersection/Resize observers; Web/CSS animations | ✅ |
| `structuredClone`; Web Crypto; console capture; init scripts; isolated-world eval | ✅ |
| **WebGL 1 + 2 on the Metal GPU** (ANGLE, WebGL-compat contexts, multi-context) | ✅ |
| **WebGL map engines**: `map.baidu.com`, MapLibre GL — full vector maps render | ✅ |
| **WebGPU** (`--webgpu` builds): real adapter/device via in-process Dawn→Metal | ✅ |
| **IndexedDB** — full in-process backend (stores, indexes, transactions, Blob values) | ✅ |
| **Web Workers** — dedicated + shared, own isolate/scheduler; **`blob:`/`data:` worker scripts**; worker `fetch`/XHR/`importScripts`/wasm/OffscreenCanvas/transferables | ✅ |
| Service/Shared Workers + BroadcastChannel; Notifications | ✅ |
| **OPFS** + Cache Storage + Cookie Store API (persist to disk) | ✅ |
| WebSocket; EventSource/SSE; streaming `fetch` | ✅ |
| **Media**: `<audio>` playback always; `<video>` decode with `--video`; WebCodecs; MSE | ✅ |
| **WebAssembly** (always on — required by full Blink) | ✅ |
| Network interception (block/mock/rewrite + request hook) | ✅ |
| Editor commands + clipboard; file upload; find-in-page; AX-tree export | ✅ |
| Screenshots: PNG/JPEG, transparent, element/region clip, full-page, zoom, mobile emulation | ✅ |
| Threaded/GPU display compositor; child-frame independent history; full WebRTC | ⏳ roadmap |

## Tool: `mb_shot` (headless HTML → PNG/JPEG/PDF)

A standalone headless renderer/scraper CLI over the same engine:

```sh
mb_shot \
  # request config
  [--user-agent UA] [--header "N: V"] [--proxy URL] [--insecure] [--no-follow] \
  [--block SUBSTR] [--mock URL FILE] [--rewrite FROM TO] \
  [--load-cookies FILE] [--save-cookies FILE] [--post BODY] \
  [--no-images] [--dark] [--lang L,L2] [--tz Area/City] \
  # interact
  [--fill CSS TEXT] [--click CSS] [--drag FROM TO] [--dispatch CSS EVT] [--press KEY] \
  # synchronize
  [--wait-selector CSS] [--wait-visible CSS] [--wait-hidden CSS] [--wait-eval JS] [--wait-idle] [--wait-ms N] \
  # prepare the view
  [--css STYLES] [--auto-scroll] [--scroll-to Y] [--scroll-to-selector CSS] \
  # extract (to stdout)
  [--title] [--url] [--cookies URL] [--local-storage KEY] [--session-storage KEY] \
  [--text] [--html] [--html-for CSS] [--eval JS] [--eval-json JS] [--frame N] \
  [--value CSS] [--checked CSS] [--count CSS] [--visible CSS] [--rect CSS] \
  [--style CSS PROP] [--text-all CSS] [--attr CSS NAME] [--attr-all CSS NAME] \
  [--requests] [--console] [--headers] \
  # capture
  [--full] [--scale N] [--mobile] [--clip x,y,w,h | --selector CSS] [--transparent] \
  [--pdf-size letter|a4|legal|a3|tabloid|WxH] [--landscape] [--pdf-scale N] [--pdf-margin PT] \
  # assert (scripting)
  [--require CSS] \
  <input.html | file://URL | http(s)://URL> <out.(png|jpg|pdf)> [width height]
```

Highlights (each flag documented in `src/miniblink_host/tools/mb_shot.cc`):

- **Capture**: `--full` (whole document, Puppeteer `fullPage`), `--scale N` (retina @N×),
  `--clip`/`--selector` (region/element shots), `--transparent`, `--mobile` (390×844,
  dpr 3, iPhone UA in one flag). Output format by extension: `.png`, `.jpg`, or `.pdf`
  (paginated via Blink print; `--pdf-size/--landscape/--pdf-scale/--pdf-margin`).
- **Synchronize**: `--wait-selector/-visible/-hidden` (appear / shown / gone),
  `--wait-eval JS` (arbitrary condition), `--wait-idle` (network-idle), `--wait-ms`.
- **Interact**: `--fill`, `--click`, `--drag FROM TO`, `--dispatch CSS EVT`,
  `--press Enter|Tab|Escape|…` (trusted key with default action).
- **Extract**: page (`--title/--url/--text/--html`), element (`--html-for/--value/
  --checked/--count/--visible/--rect/--style/--attr`), lists (`--text-all/--attr-all`
  → JSON arrays), storage/cookies, `--eval` / `--eval-json` (structured scraping,
  pipe into `jq`), `--frame N` (evaluate inside an iframe, even cross-origin),
  `--requests`/`--console`/`--headers` diagnostics.
- **Scripting**: `--require CSS` asserts the page reached the expected state — exit `3`
  if not (exit codes: `0` ok, `1` load/capture failure, `2` usage, `3` require unmet).

Rendered by `mb_shot` from an HTML file (gradient, CSS grid, translucent cards, a
rotated card, JS-injected text — all modern Blink, headless, no CEF):

![mb_shot](docs/demos/mb_shot.png)

**Live websites over HTTPS** — `mb_shot https://news.ycombinator.com out.png`:

![hacker news](docs/demos/hacker-news.png)

More demos — modern CSS, JS mutating the DOM, `file://` + SVG, `<canvas>` 2D:

![modern css](docs/demos/modern-css.png)
![javascript](docs/demos/javascript.png)
![file and image](docs/demos/file-and-image.png)
![canvas 2d](docs/demos/canvas-2d.png)

## Architecture

```
┌─ wke compatibility layer (src/wke) ─────────────────────┐
│  the classic miniblink `wke` C API on modern Blink      │
└──────────── wraps the mb_capi ABI ▼ ────────────────────┘
┌─ miniblink_host (GN target, src/miniblink_host) ────────┐
│  capi/      extern "C" ABI (the seam)                   │
│  runtime/   engine bring-up (V8 snapshot, ThreadPool,   │
│             ResourceBundle, scheduler, blink::Initialize)│
│  platform/  blink::Platform, in-process GPU thread,     │
│             WebGL/WebGPU context providers, compositor  │
│  frame/     LocalFrameHost + per-frame mojo services    │
│             (storage, IndexedDB, OPFS, notifications…)  │
│  loader/    URLLoader over vendored libcurl (+ ws/wss)  │
│  worker/    dedicated/shared worker hosts + script fetch│
│  media/     WebMediaPlayerImpl glue (audio out, MSE)    │
│  view/ widget/  WebView + non-compositing frame widget  │
├─────────────────────────────────────────────────────────┤
│  modern Blink + substrate (base, mojo, cc, skia, v8,    │
│  ANGLE, ffmpeg, BoringSSL, ICU…) — built as-is by GN    │
└─────────────────────────────────────────────────────────┘
```

The **C ABI** dissolves the GN↔consumer build mismatch: GN builds everything that
touches Blink/base/mojo C++ types; an app links only against the pure-C headers.

**Donor patches** (`patches/`, 18): small, documented Chromium patches the build
applies automatically — offscreen-widget compat, in-process GPU de-testonly, the
non-composited canvas paint path (`0008`/`0018`), gating Dawn out of the mac GPU path
when WebGPU is off (`0017`), and similar. Each patch header explains the exact reason.

## Public C ABI (`include/miniblink2/mb_capi.h`)

108 functions; the header has the full, commented signatures. The canonical flow:

```c
mbInitialize();
mbView* v = mbCreateView(1200, 800);
mbLoadURL(v, "https://example.com");
mbWaitForFunction(v, "document.readyState==='complete'", 5000);
char buf[256]; mbEvalJS(v, "document.title", buf, sizeof buf);
mbSavePng(v, "shot.png", 1200, 800);
mbDestroyView(v);
mbShutdown();
```

A complete runnable example (fill → read value → dispatch event → wait for network
idle → scrape → element screenshot) is `src/miniblink_host/tools/mb_demo.cc`.

Grouped overview (see the header for exact signatures):

- **Lifecycle / pump:** `mbInitialize` `mbShutdown` `mbCreateView` `mbDestroyView`
  `mbResize` `mbPumpMessages` `mbWait` `mbWaitForSelector` `mbWaitForFunction`
  `mbWaitForVisibleSelector` `mbWaitForSelectorHidden` `mbWaitForNetworkIdle`
- **Load / navigation:** `mbLoadHTML` `mbLoadURL` `mbPostURL` `mbReload`
  `mbGoBack`/`mbGoForward`/`mbCanGoBack`/`mbCanGoForward` `mbGetURL` `mbGetTitle`
  `mbGetHttpStatus` `mbGetResponseHeaders`
- **Scripting:** `mbRunJS` `mbSetInitScript` `mbInsertCSS` `mbEvalJS` `mbEvalJSEx`
  `mbEvalJSIsolated` `mbDrainConsole` `mbJsBindFunction`
- **Scraping:** `mbGetText` `mbGetHTML` `mbGetTextForSelector`
  `mbGetAllTextForSelector` `mbGetAllValueForSelector` `mbGetHtmlForSelector`
  `mbSetHtmlForSelector` `mbGetAttribute` `mbGetAllAttributeForSelector`
  `mbSetAttribute` `mbGetValueForSelector` `mbGetCheckedForSelector`
  `mbIsVisibleForSelector` `mbGetComputedStyle` `mbCountSelector`
  `mbGetElementRect` `mbGetContentSize` `mbGetViewSize`
- **Input:** `mbSendMouseClick`/`Down`/`Up`/`Move` `mbSendTouchTap`/`Swipe`
  `mbSendText` `mbSendKey` `mbSendScroll` `mbScrollTo` `mbScrollToBottom`; by
  selector: `mbClickSelector` `mbDoubleClickSelector` `mbRightClickSelector`
  `mbHoverSelector` `mbFocusSelector` `mbBlurSelector` `mbFillSelector`
  `mbSelectOption` `mbDispatchEvent` `mbDragSelector` `mbScrollIntoView`
- **Capture / output:** `mbPaintToBitmap` `mbPaintRectToBitmap` `mbSavePng`
  `mbSavePngRect` `mbSaveElementPng` `mbSavePdf` `mbEncodePng`
- **Cookies / session:** `mbGetCookies` `mbGetCookie` `mbGetAllCookies` `mbSetCookie`
  `mbClearCookies` `mbSaveCookies`/`mbLoadCookies`
  `mbGet`/`mbSetLocalStorage` `mbGet`/`mbSetSessionStorage` `mbClearStorage`
- **Network config:** `mbSetProxy` `mbSetIgnoreCertErrors` `mbSetFollowRedirects`
  `mbSetExtraHeaders` `mbSetUserAgent` `mbGetUserAgent` `mbSetLoadImages`
  `mbGetRequestLog`/`mbClearRequestLog` `mbBlockUrl`/`mbClearUrlBlocks`
- **Page config:** `mbSetDeviceScaleFactor` `mbSetTransparentBackground`
  `mbSetDarkMode` `mbSetLocale` `mbSetTimezone` `mbSetFocus`

## wke compatibility layer (`include/miniblink2/wke.h`)

A drop-in subset of classic miniblink's `wke` C API implemented on top of `mb_capi`,
so an existing `wke` app runs on modern Blink with the original signatures (`utf8`,
`wkeWebView`, `jsValue`, …). Verified by `wke_smoke` (96 default cases). Functions
marked *(ext)* are port extensions beyond the classic surface.

- **Lifecycle / load:** `wkeInitialize`/`wkeFinalize`, `wkeCreateWebView`/
  `wkeDestroyWebView`, `wkeLoadURL`/`wkeLoadHTML`/`wkeLoadHtmlWithBaseUrl`,
  `wkePostURL`, `wkeReload`, the loading-state pollers.
- **Geometry / rendering:** `wkeResize`, width/height/content-size getters,
  `wkeSetTransparent`, `wkeSetZoomFactor`, `wkeSetEditable`, `wkeSetDarkMode` *(ext)*,
  `wkeSetDeviceScaleFactor` *(ext)*, `wkeScrollTo`/`wkeScrollToBottom` *(ext)*,
  `wkeSetFocus`/`wkeKillFocus`.
- **Capture:** `wkePaint` (caller BGRA buffer) + *(ext)* `wkePaintRect`,
  `wkeSavePng`/`wkeSavePngRect`/`wkeSaveElementPng`, `wkeSavePdf`, `wkeEncodePng`.
- **Input:** `wkeFireMouseEvent`, `wkeFireMouseWheelEvent`,
  `wkeFireKeyDown/Up/PressEvent`.
- **Scripting (full string-backed `jsValue` model):** `wkeRunJS` + `wkeGlobalExec`;
  the `jsTypeOf`/`jsIs*`/`jsTo*`/`js*` constructor-accessor set; `jsGet`/`jsSet`/
  `jsCall`/`jsCallGlobal`; `wkeSetInitScript`, `wkeInsertCSS` *(ext)*,
  `wkeRunJsInIsolatedWorld` *(ext)*, `wkeOnJsBridge` *(ext)*,
  `wkeJsBindFunction` (native C functions callable from JS, typed args + return).
- **DOM automation** *(ext, Puppeteer-style)*: the full query/act/wait selector set
  (`wkeCountSelector`, `wkeGetTextForSelector`, `wkeClickSelector`, `wkeFillSelector`,
  `wkeWaitForSelector`, `wkeWaitForNetworkIdle`, …).
- **Networking:** cookies + jar persistence, `wkeSetProxy`, and *(ext)*
  `wkeSetExtraHeaders`, locale/timezone, redirect/cert toggles, HTTP status +
  response headers, request log, URL blocking.
- **Callbacks:** `wkeOnLoadingFinish`, `wkeOnTitleChanged`, `wkeOnConsole`,
  `wkeOnDocumentReady` (+ `wkeString` helpers).

```c
#include "wke.h"

wkeInitialize();
wkeWebView wv = wkeCreateWebView();
wkeResize(wv, 1200, 800);
wkeLoadURL(wv, "https://example.com");        // synchronous in this build
if (wkeIsLoadingSucceeded(wv)) {
    printf("title: %s\n", wkeGetTitle(wv));
    jsValue n = wkeRunJS(wv, "document.querySelectorAll('a').length");
    printf("links: %d\n", jsToInt(wkeGlobalExec(wv), n));
    int w = wkeGetWidth(wv), h = wkeGetHeight(wv);
    void* bits = malloc((size_t)w * h * 4);   // BGRA
    wkePaint(wv, bits, w * 4);
    free(bits);
}
wkeDestroyWebView(wv);
wkeFinalize();
```

Loading is synchronous here, so a `wke` app can poll `wkeIsLoadingCompleted` instead of
waiting on a message loop. A complete automation example is `src/wke/wke_demo.cc`.

## Requirements

- **macOS arm64** (Apple Silicon).
- A **Chromium M150 checkout** (`chromium-150.0.7871.24`) and **depot_tools**, as
  siblings of this repo (override with `CHROMIUM=`/`DEPOT=` env or
  `--chromium`/`--depot`):

  ```
  <parent>/
  ├── miniblink2/        (this repo)
  ├── chromium-150.0.7871.24/  (donor source tree)
  └── depot_tools/
  ```
- **Full Xcode** (not just CommandLineTools) with the license accepted and the Metal
  toolchain installed — ANGLE's Metal backend compiles `.metal` shaders at build time:

  ```sh
  sudo xcode-select -s /Applications/Xcode.app
  sudo xcodebuild -license accept
  xcodebuild -downloadComponent MetalToolchain   # newer macOS: separate download
  ```
- The vendored WebSocket-enabled libcurl builds via `scripts/build-curl-macos.sh`
  (pinned to `MACOSX_DEPLOYMENT_TARGET=12.0` to match the engine link).

## Development workflow

`build.sh /path/to/chromium` is the inner-loop build: it stages `src/` into the donor
tree, applies `patches/`, runs GN + ninja against the **component** `out/Release`, and
executes the full test battery:

- `mb_smoke` — 179-check capability + regression suite over the C ABI.
- `wke_smoke` — 96 checks over the wke layer.
- `mb_shot_smoke.sh` — 62 offline CLI cases asserting `mb_shot`'s exact stdout,
  capture geometry, and an end-to-end fill→click→wait→eval scrape.
- `MB_NET_TESTS=1` adds the live-network cases (POST/cookies/proxy/redirects/certs).

`scripts/build-lib.sh` (the SDK build) uses a separate non-component out dir
(`out/mono-release`), so the dev and ship builds don't interfere.

## Credits

Classic engine, the `wke`/`mb` API design, and years of groundwork by **weolar** —
<https://github.com/weolar/miniblink49> · <http://miniblink.net>. This project is an
independent re-implementation of that embedding model on modern Blink.
