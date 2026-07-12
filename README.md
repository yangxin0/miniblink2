# miniblink2

A standalone, single-process embedder of modern Blink (Chromium M150 / V8 15).
miniblink2 boots the real Blink engine inside your process behind a small C API —
no CEF, no separate browser process, no cross-process IPC.

It is the successor to [miniblink49](https://github.com/weolar/miniblink49), whose
Blink froze at ~M47 (2015). The direct embedding model it used no longer exists in
modern Blink, so this project implements the minimal content layer that the modern
engine requires to run without a full browser.

Supported platforms: **macOS arm64** and **Windows x64** — the same sources,
patches, samples, and test battery on both.

## Highlights

- **Self-contained SDK** — one shared library (`libminiblink2.dylib` /
  `miniblink2.dll`) or one complete static archive, two C headers, and the
  engine's runtime data. An app links one library and gets the web platform.
- **Modern web platform** — current CSS (Grid, `:has()`, container queries,
  `oklch()`), Web Components, workers (dedicated / shared / service), IndexedDB,
  OPFS, WebSocket/SSE, streaming fetch, media, and GPU-accelerated WebGL 1+2
  (ANGLE → Metal on macOS, D3D11/SwiftShader on Windows). WebGPU, WebAssembly,
  `<video>` decode, and WebRTC are optional build flags.
- **Two host models** — synchronous calls for automation and capture, or a
  non-blocking frame-tick API for embedding a live webview in an application.
- **Async navigation** — `mbNavigate` returns immediately with a navigation id;
  `mbOnNavigationEvent` delivers an id-correlated lifecycle
  (started / committed / terminal with outcome); `mbCancelNavigation` cancels by id.
- **Browsing sessions** — isolated profiles for cookies, DOM storage, IndexedDB,
  and OPFS; in-memory or disk-backed; bound immutably at view creation.
- **Network interception** — block, mock, and rewrite requests with hooks on both
  request and response; transparent rewrites keep public URLs across redirects.
- **DevTools** — every view exposes a Chrome DevTools Protocol session, and
  `mbDevToolsStartServer` hosts the endpoint so ordinary Chrome attaches directly.
- **Verified** — a 570+ case automated test battery runs green on both platforms.

## Quick start

Prerequisites: a configured Chromium M150 checkout and depot_tools as siblings of
this repo, plus full Xcode on macOS (see [Requirements](#requirements)).

```sh
scripts/build-lib.sh --release --ship     # 1. build the SDK -> dist/release/
scripts/build-samples.sh --both           # 2. build the samples against it
cd dist/release                           # 3. run from the dist dir
./minibrowser_dyn https://example.com
```

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

## The SDK

`scripts/build-lib.sh` (macOS) and `scripts/build-lib.ps1` (Windows, same flags)
produce `dist/<release|debug>/`:

| File | Purpose |
|---|---|
| `libminiblink2.dylib` / `.a` (or `miniblink2.dll` + import lib) | the whole engine as one library, exporting the `mb*` C API |
| `include/miniblink2/webview.h` | the embedder core (lifecycle, loads, paint, input, callbacks) |
| `include/miniblink2/automation.h` | the automation kit (waits, selectors, capture; these calls pump) |
| `blink_resources.pak`, `icudtl.dat`, `v8_context_snapshot.bin` | engine runtime data — ship next to your binary |
| `libEGL.dylib`, `libGLESv2.dylib` | ANGLE, loaded at runtime for WebGL |
| samples + `mb_shot` | see below |

Build profiles: `--debug` (full symbols), `--release` (`-O2`, assertions on), and
`--release --ship` (the publishable SDK: 88 MB dylib / 757 MB static archive,
optimized and stripped). Optional features are include-only flags, off by default:
`--webgpu`, `--video`, `--wasm`, `--webrtc`, `--ml`, `--turbofan`/`--maglev` (JIT
tiers; the default is V8 lite mode), `--av1-encode`, `--swiftshader`, `--icu-full`.
Sizes, tradeoffs, and the one-time platform bootstrap are documented in `BUILD.md`.

`scripts/package.sh` zips the SDK (`lib/` + `include/` + `resources/` + a README
with exact link lines) with portable install names, ready to distribute.

## Embedding

**Automation and capture hosts** (scrapers, screenshotters, test drivers) call
straight through: loads are synchronous, `mbWaitFor*` pumps to a condition, and
captures settle the page first. `mb_shot` and `mb_demo` are this shape.

**Interactive hosts** (a webview inside a real application) own a frame tick and
never let the engine pump their run loop:

```c
mbInitialize();
mbSetMaxUpdateTime(0.008);                // engine budget per tick
mbView* v = mbCreateView(w, h);
mbLoadURL(v, "https://example.org");

// every vsync tick:
mbUpdateAt(frame_time);                   // advance the world (never nests)
if (mbViewIsDirty(v))                     // damage-gated: skip clean frames
    if (mbRepaintToBitmap(v, buf, pw, ph, pitch))
        blit(buf);                        // premultiplied BGRA, sRGB
```

Everything an interactive embedder needs is push-based — navigation and load
lifecycle, tab metadata (URL / title / favicon), cursor and tooltip, history,
`window.open` as an adopted child view, `<select>` popups, JS dialogs, downloads,
and an OS-clipboard bridge. Creation-time options collect in an `mbViewConfig`
builder, so nothing depends on call ordering. The webview.h preamble documents the
wiring matrix per host type; damage can be consumed as dirty rects or, on macOS,
as zero-copy IOSurface compositor output.

The public API is **300 functions** across the two headers, each with a commented
contract covering threading, coordinates, and string conventions.

## Samples

A numbered set covering the API end to end; the sample code is OS-independent and
the windowing scaffold lives in `samples/compat/` (Cocoa and Win32 backends). See
`samples/README.md`.

| # | Sample | Shows |
|---|---|---|
| 1 | render-to-png | headless URL → PNG in five calls |
| 2/3 | basic / resizable app | windowed hosting, live relayout, HiDPI |
| 4 | javascript | native ↔ JS binding both ways |
| 5 | file loading | `file://` and memory-served virtual files |
| 6 | intro (C99) | headless paint + `mbEvalJS` read-back |
| 8 | minibrowser | a tabbed browser with HTML chrome, downloads, `window.open` tabs, and a DevTools endpoint |
| 9 | multi window | two windows/views, deferred work, history-replace loads |

## mb_shot

A headless render/scrape CLI over the same engine: URL or file in, PNG/JPEG/PDF or
structured text out, with flags for request configuration, interaction (fill,
click, drag, keys), synchronization (selectors, network idle, JS conditions), and
extraction (text, HTML, attributes, JSON via `--eval-json`). Every flag is
documented in `src/miniblink_host/tools/mb_shot.cc`.

```sh
mb_shot https://news.ycombinator.com out.png
mb_shot --fill "#q" "term" --click "#go" --wait-idle --eval-json "..." page.html out.png
```

![mb_shot](docs/demos/mb_shot.png)

## Architecture

```
┌─ miniblink2 public API (src/miniblink2) ────────────────┐
│  webview.h + automation.h — the extern "C" mb* API      │
└────────────── pure C, no Blink types ▼ ─────────────────┘
┌─ miniblink_host (GN target, src/miniblink_host) ────────┐
│  runtime/   engine bring-up (V8, scheduler, Initialize) │
│  platform/  blink::Platform, in-process GPU, compositor │
│  frame/     LocalFrameHost + per-frame mojo services    │
│  session/   browsing profiles (per-profile partitions)  │
│  loader/    URLLoader over vendored libcurl (+ ws/wss)  │
│  worker/    dedicated/shared worker hosts               │
│  media/     WebMediaPlayerImpl glue (audio out, MSE)    │
│  devtools/  per-view CDP sessions                       │
│  view/ widget/  WebView + non-compositing frame widget  │
├─────────────────────────────────────────────────────────┤
│  modern Blink + substrate (base, mojo, cc, skia, v8,    │
│  ANGLE, ffmpeg, BoringSSL, ICU…) — built as-is by GN    │
└─────────────────────────────────────────────────────────┘
```

The C ABI separates the build worlds: GN builds everything that touches
Blink/base/mojo C++ types; an application links only against the pure-C headers.
44 small, documented donor patches (`patches/`) adapt the Chromium tree; each
patch header explains its reason.

## Testing

`build.sh /path/to/chromium` runs the full battery: `mb_smoke` (279) +
`mb_smoke_r6` (24) over the C ABI, `mb_smoke_platform` (55), `mb_smoke_render`
(151), and `mb_shot_smoke.sh` (66) — 575 checks, green on macOS and Windows.
Wire-level network cases run hermetically by default against a bundled local echo
server (`MB_NET_TESTS=0` opts out; `MB_NET_HOST` targets an explicit host).

## Requirements

- **macOS arm64** (full Xcode with the Metal toolchain) or **Windows x64**
  (VS 2022 Build Tools; the one-time bootstrap is in `BUILD.md`).
- A **Chromium M150 checkout** (`chromium-150.0.7871.24`) and **depot_tools** as
  siblings of this repo (override with `CHROMIUM=` / `DEPOT=`):

  ```
  <parent>/
  ├── miniblink2/                    (this repo)
  ├── chromium-150.0.7871.24/        (donor source tree)
  └── depot_tools/
  ```

## Documentation

- `BUILD.md` — build profiles, feature flags, platform bootstrap
- `RELEASE.md` — release notes per tagged version
- `docs/design/` — API design log and backlog
- The header preambles are the API reference: `include/miniblink2/webview.h`,
  `include/miniblink2/automation.h`

## Credits

The classic engine, the original `wke` API design (which shaped the `mb` API),
and years of groundwork are **weolar**'s —
<https://github.com/weolar/miniblink49> · <http://miniblink.net>. This project is
an independent re-implementation of that embedding model on modern Blink.
