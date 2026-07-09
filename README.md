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
`libminiblink2.a`) + two C headers + the engine's runtime data. An app links one library
and gets the whole modern web platform — GPU-accelerated WebGL on Metal included
(`map.baidu.com` and MapLibre GL render their full vector maps).

**Runs on macOS arm64 and Windows x64.** The same sources, patches, samples, and test
battery build and pass on both: on Windows the SDK is one `miniblink2.dll` + import lib
(`scripts/build-lib.ps1`, same flags as the mac script), with WebGL on
SwiftShader-ANGLE by default (`--use-angle=d3d11` for hardware) and WebGPU on
Vulkan/SwiftShader. See `BUILD.md` § "Windows (x64)" for the one-time bootstrap.

> New here? The public API is two headers: `include/miniblink2/webview.h` (the
> embedder core — lifecycle, loads, paint, input, callbacks) and
> `include/miniblink2/automation.h` (the automation kit — waits, selectors,
> screenshots, PDF; these calls pump). Quick start below; `samples/` is the
> guided tour.

---

## Quick start

Prerequisites (see [Requirements](#requirements)): a configured Chromium M150 checkout and
depot_tools as **siblings** of this repo, plus full Xcode (for the Metal shader compiler).

```sh
# 1. Build the self-contained SDK -> dist/release/
scripts/build-lib.sh --release --ship

# 2. Build the samples against it
scripts/build-samples.sh --both

# 3. Run (from the dist dir — runtime data is loaded from beside the binary)
cd dist/release
./minibrowser_dyn    https://map.baidu.com     # tabbed browser linking the .dylib
./minibrowser_static https://map.baidu.com     # fully self-contained (no dylib dep)
```

A window opens and renders the page — WebGL vector maps run on the **Metal GPU**.

The API in one screen (pure C, no Blink types):

```c
#include "miniblink2/automation.h"   // includes webview.h, the embedder core

mbInitialize();
mbView* v = mbCreateView(1200, 800);
mbLoadURL(v, "https://example.com");          // pumps until the `load` event
char title[512], links[64];
mbGetTitle(v, title, sizeof title);
mbEvalJS(v, "document.querySelectorAll('a').length", links, sizeof links);
printf("%s — %s links\n", title, links);
mbSavePng(v, "shot.png", 1200, 800);
mbDestroyView(v);
mbShutdown();
```

A complete automation example (fill → read value → dispatch event → wait for network
idle → scrape → element screenshot) is `src/miniblink_host/tools/mb_demo.cc`; the
interactive-host pattern is below.

## The SDK (`dist/<release|debug>/`)

| File | What it is |
|---|---|
| `libminiblink2.dylib` | the whole engine as ONE shared library (exports the `mb*` C API) |
| `libminiblink2.a` | the same engine as ONE complete static archive |
| `include/miniblink2/webview.h` | the embedder core header |
| `include/miniblink2/automation.h` | the automation kit header (includes webview.h) |
| `blink_resources.pak`, `media_controls_…pak` | Blink resources (UA stylesheet, …) |
| `icudtl.dat`, `v8_context_snapshot.bin` | ICU data + the V8 context snapshot |
| `libEGL.dylib`, `libGLESv2.dylib` | **ANGLE** — the GL/WebGL driver (→ Metal) |
| `libvk_swiftshader.dylib`, `vk_swiftshader_icd.json` | software-GL fallback (optional) |
| `minibrowser_dyn/_static`, `sample*`, `mb_shot` | the sample set + tools (see below) |

Deployment rules:

- The runtime data files load **from the executable's directory** — ship them next to
  your binary.
- The ANGLE dylibs are **`dlopen()`ed at runtime** (Chromium's standard GL loading), so
  they never appear in `otool -L` — but WebGL is unavailable without them. The
  SwiftShader pair is only needed if you run with `--use-angle=swiftshader`
  (headless CI / GPU-less VMs); desktop-only distributions can drop those ~20 MB.
- The `.a` is **native machine code** (`--ship` deliberately skips ThinLTO for the
  archive — bitcode would bloat it ~8x and force lld on consumers): it links with any
  toolchain (`scripts/build-samples.sh` shows the framework list a consumer supplies).

### `scripts/build-lib.sh` — building the SDK

```sh
scripts/build-lib.sh [--release|--debug] [--ship]
                     [--webgpu] [--video] [--ml] [--wasm] [--webrtc] [--av1-encode]
                     [--turbofan] [--maglev]
                     [--tracing] [--swiftshader] [--icu-full]
                     [--chromium DIR] [--depot DIR] [--no-stage] [--print-only]
```

On **Windows**, `scripts\build-lib.ps1` takes the *same flags* and produces
`dist\release\miniblink2.dll` + `miniblink2.dll.lib` + the runtime data
(SwiftShader GL DLLs and `libcurl.dll` included). The dev-release profile skips
the merged static lib on Windows — its ~4 GB of DCHECK/debug objects exceed the
COFF archive format — use `--ship` for `miniblink2_static.lib`.

Profiles (each in its own out dir, so switching stays incremental):

| Profile | Artifacts | Config |
|---|---|---|
| `--debug` | dylib 1.3 GB | no optimization, full symbols, DCHECKs — for lldb |
| `--release` | dylib 183 MB + dev `.a` | `-O2`, **DCHECK assertions on** (Chromium's developer default: near-ship speed, internal bugs abort loudly) |
| `--release --ship` | **dylib 88 MB + `.a` 757 MB** | the publishable SDK: `-Oz`, DCHECKs compiled out, stripped. The dylib uses **ThinLTO** (whole-program opt at our link); the `.a` is **native code** on purpose — an archive never passes through our linker, so bitcode would only bloat it (1.9 GB) and force lld on consumers |

Feature flags (apply to any profile):

| Flag | Effect | Size impact (ship dylib) |
|---|---|---|
| `--webgpu` | include WebGPU (Dawn); off = Dawn completely absent from the binary | +~9 MB |
| `--video` | include `<video>` decode (H.264/ffmpeg-video). Off = **audio still plays** (miniblink49 parity) | +~8 MB |
| `--wasm` | include WebAssembly (V8 wasm engine). Off = `window.WebAssembly` absent (miniblink49 parity) | +~4.5 MB |
| `--ml` | include WebNN on-device ML (TFLite/LiteRT/XNNPACK) + the TFLite language-detection model + WebRTC's neural echo estimator (patches 0019/0021) | +~4 MB |
| `--webrtc` | include the WebRTC stack: `RTCPeerConnection` + SDP/signaling, libwebrtc core, Blink RTC bindings, p2p platform layer (patch 0030). Off keeps getUserMedia/mediaDevices surfaces (headless: no devices) but `window.RTCPeerConnection` is absent | +~3.3 MB |
| `--turbofan` | include V8's TurboFan top-tier optimizing JIT. Default is **V8 lite mode** (Ignition interpreter + Sparkplug baseline JIT only, low-memory heap) — fine for automation/scraping; sustained-hot JS runs ~5-20x slower without it | +~11 MB |
| `--maglev` | include V8's Maglev mid-tier JIT as well (implies `--turbofan`); the full 4-tier pipeline, i.e. stock Chromium JS performance | +~5 MB |
| `--av1-encode` | include AV1 *encoding* (libaom: WebCodecs/MediaRecorder/WebRTC send; decode always in) | +~1 MB |
| `--tracing` | include OPTIONAL_TRACE_EVENT instrumentation (perf-investigation builds) | +~0.5 MB |
| `--swiftshader` | ship SwiftShader software Vulkan in `dist/` (headless/CI/no-GPU `--use-angle=swiftshader`) | +20 MB (dist) |
| `--icu-full` | ship untrimmed `icudtl.dat` (all ~90 locales); default trims to root+en+zh (`MB_ICU_KEEP=en,zh,ja` to customize) | +4.1 MB (dist) |

Feature flags are include-only and **default off** — the default SDK is the trimmed,
miniblink49-like profile (audio on; video/webgpu/ml/wasm off). For comparison: miniblink49
ships a 53 MB stripped dylib / 134 MB `.a`; this is the full 2026 engine at ~1.7× / ~5.6×
those sizes (the archive ratio is worse because an archive can't be dead-stripped — it
must carry every object a consumer might pull).
Per-component size attribution for further pruning: `nm -n <out>/libminiblink2.dylib |
scripts/sizemap.py` (see `BACKLOG.md` §E for the measured, deliberately-not-cut leftovers).

The first build of a mode is a full engine compile (slow); re-runs are incremental
(staging uses rsync, builds are lock-serialized, flag flips rebuild only what changed).

## Two kinds of hosts

The API serves two host shapes, and the headers say which calls belong to which
(webview.h opens with a per-host-type wiring matrix):

**Automation / capture hosts** (scrapers, screenshotters, testing) call straight
through: loads are synchronous, the `mbWaitFor*` calls pump to a condition, and the
one-shot captures settle the page before painting. `mb_shot` and `mb_demo` are this
shape.

**Interactive hosts** (an embedded webview in a real app) own a frame tick and never
let the engine pump their run loop. The canonical loop, from the webview.h preamble:

```c
mbInitialize();
mbSetMaxUpdateTime(0.008);                // 8 ms engine budget per tick
mbView* v = mbCreateView(w, h);
mbLoadURL(v, "https://example.org");

// every vsync tick:
mbUpdateAt(frame_time);                   // advance the world (never nests)
if (mbViewIsDirty(v))                     // damage-gated: skip clean frames
    if (mbRepaintToBitmap(v, buf, pw, ph, pitch))
        blit(buf);                        // premultiplied BGRA, sRGB
```

Everything an interactive embedder needs is push-based: load lifecycle
(`mbOnBeginLoading`/`mbOnDOMContentLoaded`/`mbOnLoadFinish`/`mbOnFailLoadingEx` with
machine-checkable error domain+code), tab metadata (`mbOnUrlChanged`/`mbOnTitleChanged`/
`mbOnFaviconChanged`), pointer UI (`mbOnCursorChanged`/`mbOnTooltipChanged`), history
state (`mbOnHistoryChanged`), `window.open` as a real adopted child view
(`mbOnCreateChildView` — live opener/`postMessage`), `<select>` popups surfaced to the
host, JS dialogs, an OS-clipboard bridge, and `mbDefer`/`mbInEngineCall` for re-entrancy
discipline. Creation-time choices collect in an `mbViewConfig` builder
(`mbCreateViewWithConfig`), so nothing depends on call-before-load ordering.

**Sessions** isolate browsing profiles: `mbCreateSession(name, persist_path)` gives
each profile its own cookies, DOM storage, IndexedDB, OPFS, and cache partitions —
in-memory by default, disk-backed when a path is given, with `mbSessionFlush` as the
durability barrier.

**DevTools**: every view exposes a Chrome-DevTools-Protocol session
(`mbDevToolsAttach`/`Send`/`Detach`). Bridge it to a WebSocket + `/json` endpoint —
`samples/sample8_minibrowser/cdp_bridge.cc` is a drop-in — and **ordinary Chrome
attaches as the frontend** via `chrome://inspect` (Elements/Console/Sources, verified
against real Chrome). `mbOnDevToolsPaused` tells the host to stop its frame tick at a
breakpoint instead of treating it as a hang.

## Samples (`samples/`) — macOS + Windows

A numbered set covering the API end to end. The sample code is OS-independent; the
platform scaffold lives in `samples/compat/` (Cocoa + Win32 backends). See
`samples/README.md` for the full table and per-platform build lines.

| # | Sample | Shows |
|---|---|---|
| 1 | render-to-png | headless URL → PNG in five calls |
| 2/3 | basic / resizable app | windowed hosting, live relayout, HiDPI |
| 4 | javascript | `mbJsBindFunction` both ways, `mbOnWindowObjectReady` |
| 5 | file loading | `file://` + a memory-served "virtual file" (`mbMockResponse`) |
| 6 | intro to the C API | plain C99, headless paint + `mbEvalJS` read-back |
| 8 | **minibrowser** | a tabbed browser: HTML chrome bound to native code, error pages, downloads, `window.open` tabs, CDP DevTools endpoint |
| 9 | multi window | two windows/views, `mbDefer`, history-replace loads |

`scripts/build-samples.sh` builds them against the SDK on macOS
(`minibrowser_static` variant included); `samples\build.ps1` is the Windows peer.
Every windowed sample honors `MB_SAMPLE_AUTOEXIT_MS` for scripted smoke runs.

### `scripts/package.sh` — zip the SDK for distribution

```sh
scripts/package.sh [--release|--debug] [--dynamic|--static|--both] [--out ZIP]
```

The miniblink49 `tools/package-macos.sh` equivalent: stages `dist/<mode>/` into
`miniblink2-macos-arm64-<mode>[-kind].zip` (47 MB for release dynamic) with
`lib/` + `include/` + `resources/` + a generated README containing the exact
link lines. The staged dylibs are made **portable**: the vendored-curl reference
is rewritten from the build machine's absolute path to `@loader_path`, install
ids become `@rpath`, and everything is ad-hoc re-signed. Verified by compiling
and booting the sample browser from an unpacked zip in a clean directory.

## GPU architecture

```
                     macOS                          Windows
2D  (Skia Ganesh)  ─┐                             ─┐
3D  (WebGL 1+2)    ─┼─► GL ES ─► ANGLE ─► Metal    ┼─► GL ES ─► ANGLE ─► SwiftShader (default)
                    │   └ or SwiftShader (CPU)     │   └ or D3D11 via --use-angle=d3d11
WebGPU (--webgpu)  ───► Dawn ─► Metal             ───► Dawn ─► Vulkan/SwiftShader
```

- WebGL runs on the **real GPU** through ANGLE's Metal backend
  (`ANGLE (Apple, ANGLE Metal Renderer: Apple M4 Pro …)`) — verified end-to-end with
  Baidu Maps and MapLibre GL rendering full vector maps (tiles parsed in `blob:` Web
  Workers, drawn via WebGL, composited into the page).
- Without `--webgpu`, **Dawn is completely absent** — no code, no symbol references, no
  runtime probing (`patches/0017`); `navigator.gpu.requestAdapter()` resolves null.
- Canvases paint **inline** (no display compositor): the drawing buffer is snapshotted
  into the software page paint (`patches/0008` + `0018`), which is what makes WebGL
  content appear in `mbPaintToBitmap`/`mb_shot` screenshots.

## What works (verified by a 460+ case automated battery, on macOS **and** Windows)

| Subsystem | Status |
|---|---|
| Engine boot in-process: V8 isolate + Oilpan/cppgc + main-thread scheduler | ✅ |
| HTML parsing, UA stylesheet, CSS cascade; fonts + text via CoreText/DirectWrite | ✅ |
| **Modern CSS**: Grid, Flexbox+gap, gradients, transforms, `:has()`, nesting, `@container`, `color-mix()`, `oklch()` | ✅ |
| **Web Components**: Custom Elements v1 + Shadow DOM | ✅ |
| JavaScript (V8) + DOM mutation → style recalc → relayout → repaint | ✅ |
| `<canvas>` 2D; image decode + SVG (data: URIs and external files) | ✅ |
| **HTTP/HTTPS** via vendored libcurl (WebSocket-enabled); subresource loading | ✅ |
| Paint readback to BGRA bitmap + PNG/JPEG; **PDF export** (paginated) | ✅ |
| Input events (mouse/typed keys/IME/touch/wheel, sub-frame routing), HiDPI, custom UA | ✅ |
| DOM storage, cookies (jar + `document.cookie`), History/Navigation API | ✅ |
| **Sessions**: per-profile cookies/storage/IndexedDB/OPFS, memory or disk-backed | ✅ |
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
| **WebAssembly** (`--wasm` builds) | ✅ |
| **WebRTC** (`--webrtc` builds): `RTCPeerConnection` + SDP/signaling | ✅ |
| Network interception (block/mock/rewrite; request + response hooks) | ✅ |
| **`window.open` child views** — adopted popups with live opener/`postMessage` | ✅ |
| **DevTools**: per-view CDP sessions; real Chrome as the frontend | ✅ |
| Editor commands + clipboard (incl. OS-clipboard bridge); file upload; find-in-page; AX-tree export | ✅ |
| Screenshots: PNG/JPEG, transparent, element/region clip, full-page, zoom, mobile emulation | ✅ |
| Threaded/GPU display compositor; dirty-rect damage; child-frame independent history | ⏳ roadmap |

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
┌─ miniblink2 public API (src/miniblink2) ────────────────┐
│  webview.h + automation.h — the extern "C" mb* API      │
└────────────── pure C, no Blink types ▼ ─────────────────┘
┌─ miniblink_host (GN target, src/miniblink_host) ────────┐
│  runtime/   engine bring-up (V8 snapshot, ThreadPool,   │
│             ResourceBundle, scheduler, blink::Initialize)│
│  platform/  blink::Platform, in-process GPU thread,     │
│             WebGL/WebGPU context providers, compositor  │
│  frame/     LocalFrameHost + per-frame mojo services    │
│             (storage, IndexedDB, OPFS, notifications…)  │
│  session/   browsing profiles (per-profile partitions)  │
│  loader/    URLLoader over vendored libcurl (+ ws/wss)  │
│  worker/    dedicated/shared worker hosts + script fetch│
│  media/     WebMediaPlayerImpl glue (audio out, MSE)    │
│  devtools/  per-view CDP sessions (in-process mojo)     │
│  view/ widget/  WebView + non-compositing frame widget  │
├─────────────────────────────────────────────────────────┤
│  modern Blink + substrate (base, mojo, cc, skia, v8,    │
│  ANGLE, ffmpeg, BoringSSL, ICU…) — built as-is by GN    │
└─────────────────────────────────────────────────────────┘
```

The **C ABI** dissolves the GN↔consumer build mismatch: GN builds everything that
touches Blink/base/mojo C++ types; an app links only against the pure-C headers.

**Donor patches** (`patches/`, 40): small, documented Chromium patches the build
applies automatically — offscreen-widget compat, in-process GPU de-testonly, the
non-composited canvas paint path (`0008`/`0018`), gating Dawn out of the GPU path
when WebGPU is off (`0017` mac, `0035`/`0036`/`0040` Windows), host `<select>`
popups on every platform (`0033`), the host font-fallback hook (`0029`), and
similar. Each patch header explains the exact reason.

## Public C ABI (`include/miniblink2/`)

**264 functions** across the two headers, every one with a commented contract; the
header preambles state the global threading, coordinate/pixel, and string
conventions once. Grouped overview:

- **Lifecycle / tick:** `mbInitialize` `mbShutdown` `mbCreateView(WithConfig)`
  `mbViewConfig*` builder `mbDestroyView` `mbResize` `mbUpdate`/`mbUpdateAt`
  `mbSetMaxUpdateTime` `mbDefer` `mbInEngineCall` `mbPumpMessages`
  `mbPurgeMemory` `mbVersion`/`mbApiVersion`/`mbChromiumVersion`
- **Sessions:** `mbCreateSession` `mbDestroySession` `mbDefaultSession`
  `mbCreateViewInSession` `mbViewGetSession` `mbSessionFlush`
  `mbSessionClearStorage` + introspection getters
- **Load / navigation:** `mbLoadHTML(Ex)` `mbLoadURL` `mbPostURL(Data)` `mbReload`
  `mbStopLoading` `mbGoBack`/`mbGoForward`/`mbGoToOffset` `mbGetURL` `mbGetTitle`
  `mbGetHttpStatus` `mbGetResponseHeaders` `mbGetLastError`
- **Push callbacks:** load lifecycle (`mbOnBeginLoading` `mbOnDOMContentLoaded`
  `mbOnWindowObjectReady` `mbOnLoadFinish` `mbOnFailLoading(Ex)`), tab metadata
  (`mbOnUrlChanged` `mbOnTitleChanged` `mbOnFaviconChanged`), pointer UI
  (`mbOnCursorChanged` `mbOnTooltipChanged`), `mbOnHistoryChanged`
  `mbOnNavigation` `mbOnNewWindow` `mbOnCreateChildView` `mbOnRequestClose`
  `mbOnSelectPopup` `mbOnDownload` `mbOnConsoleMessage(Ex)` `mbOnLogMessage`
- **Scripting:** `mbRunJS` `mbSetInitScript` `mbInsertCSS` `mbSetUserStylesheet`
  `mbEvalJS(Ex/Catch/Isolated/InFrame)` `mbJsBindFunction` `mbSetJsDialogCallback`
- **Input:** typed events (`mbSendMouseEvent` `mbSendWheelEvent` `mbSendKeyEvent`)
  + shorthands (`mbSendMouseClick(Ex)`/`Down`/`Up`/`Move` `mbSendTouchTap`/`Swipe`
  `mbSendText` `mbSendKey(Ex/Up)` `mbSendIme` `mbSendWheel` `mbSendScroll`)
- **Interception:** `mbMockResponse` `mbOnRequestMock` `mbRewriteUrl` `mbBlockUrl`
  `mbBlockResourceType` `mbSetRequestHeader` `mbSetRequestHook` + `mbRequest*`
  handle `mbSetResponseCallback` + `mbResponse*` handle
- **Paint:** `mbRepaintToBitmap` `mbViewIsDirty`/`mbViewSetDirty` (interactive);
  `mbPaintToBitmap` `mbPaintRectToBitmap` `mbSavePng(Rect)` `mbSaveElementPng`
  `mbSavePdf(Ex)` `mbEncodePng` (one-shot, settling)
- **DevTools:** `mbDevToolsAttach`/`Send`/`Detach` `mbOnDevToolsPaused`
- **Automation kit** (automation.h): `mbWaitFor*`, selector actions
  (`mbClickSelector` `mbFillSelector` `mbDragSelector` …), scraping getters
  (`mbGetText/HTML/Attribute/ComputedStyle/…`), storage/cookie save-load,
  emulation (`mbEmulateDevice/Media` `mbSetTimezone` `mbSetGeolocation`
  `mbSetOnline` `mbSetVisibility`), find-in-page, AX-tree export
- **Host services:** `mbSetClipboardHandler` (OS-clipboard bridge)
  `mbSetFontFamilies` `mbSetFontFallbackCallback` `mbAddFontData`
  `mbSetProxy` `mbSetIgnoreCertErrors` `mbSetFollowRedirects` `mbSetExtraHeaders`
  `mbSetUserAgent` `mbSetLocale` `mbSetDarkMode` `mbSetDeviceScaleFactor`
  `mbSetZoomFactor` `mbSetTransparentBackground` `mbSetEnableJavascript`
  `mbSetLoadImages` `mbExecuteEditCommand(Value)`

## Requirements

- **macOS arm64** (Apple Silicon) **or Windows x64** (Windows 10/11 — VS 2022
  Build Tools + ATL, Windows 11 SDK with Debugging Tools, Git for Windows, CMake,
  ninja, Python 3.12; the full one-time bootstrap incl. toolchain pins and the
  Schannel libcurl is `BUILD.md` § "Windows (x64)").
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
executes the full test battery (466 checks, green on macOS and Windows):

- `mb_smoke` — 213-check capability + regression suite over the C ABI.
- `mb_smoke_platform` (46) + `mb_smoke_render` (141) — platform services and
  rendering/worker/storage regression suites.
- `mb_shot_smoke.sh` — 66 offline CLI cases asserting `mb_shot`'s exact stdout,
  capture geometry, and an end-to-end fill→click→wait→eval scrape.
- `MB_NET_TESTS=1` adds the live-network cases (POST/cookies/proxy/redirects/certs).

`scripts/build-lib.sh` (the SDK build) uses a separate non-component out dir
(`out/mono-release`), so the dev and ship builds don't interfere.
`IMPROVEMENT.md` is the API design log — every embedder-facing surface traces to a
real integration incident recorded there.

## Credits

Classic engine, the original `wke` API design (which shaped the `mb` API), and years of groundwork by **weolar** —
<https://github.com/weolar/miniblink49> · <http://miniblink.net>. This project is an
independent re-implementation of that embedding model on modern Blink.
