# miniblink-modern

A **standalone, single-process embedder of modern Blink** (Chromium M150 / V8 15) — a
hand-written tiny "content layer" that boots the real Blink engine in-process and renders
HTML/CSS/JavaScript to a bitmap through a small C ABI. **No CEF**, no separate browser
process, no Mojo IPC across processes.

It is the spiritual successor to [miniblink49](https://github.com/weolar/miniblink49)
(whose Blink froze at ~M47/2015), rebuilt against the M150 engine. The old miniblink
embedding model — call straight into `WebViewImpl` — no longer exists in modern Blink
(everything routes through Mojo + `//content`). This project provides the *minimum* host
that satisfies modern Blink so it runs without the full browser.

## What works today (all screenshot-verified)

| Subsystem | Status |
|---|---|
| Build modern Blink as GN libraries, link into a standalone host via a C ABI | ✅ |
| Engine boot in-process: V8 isolate + Oilpan/cppgc + main-thread scheduler | ✅ |
| WebView + main LocalFrame + (non-compositing) WebFrameWidget | ✅ |
| HTML parsing, UA stylesheet, CSS cascade | ✅ |
| Fonts + text (CoreText), real glyph rasterization | ✅ |
| **Modern CSS**: Grid, Flexbox+gap, gradients, border-radius, box-shadow, 2D transforms | ✅ |
| **JavaScript** (V8) + DOM mutation → style recalc → relayout → repaint | ✅ |
| `<canvas>` 2D drawing via JS (shapes, gradients, text → skia) | ✅ |
| `mbLoadURL("file://…")` — load a document from disk | ✅ |
| Image decode + SVG rendering (data: URIs **and external files**) | ✅ |
| **Subresource loading** (external `<link>` CSS, `<img>`) via a `blink::URLLoader` | ✅ |
| In-process `MimeRegistry` (so `file://` stylesheets validate) | ✅ |
| **HTTP/HTTPS loading of live websites** via system libcurl | ✅ |
| Paint readback to a BGRA8888 bitmap + PNG | ✅ |
| On-screen window, input events, GPU compositing | ⏳ roadmap |

## Tool: `mb_shot` (headless HTML → PNG)

The deliverable example app — a standalone headless screenshot renderer:

```sh
mb_shot <input.html | file://URL> <out.png> [width height]
```

Rendered by `mb_shot` from an HTML file (gradient, CSS grid, translucent cards, a
rotated card, and JS-injected text — all modern Blink, headless, no CEF):

![mb_shot](docs/demos/mb_shot.png)

**Live websites over HTTPS**, fetched via system libcurl and rendered by modern Blink.
`mb_shot https://news.ycombinator.com out.png` — the real Hacker News front page, with its
external `news.css`, `hn.js`, and SVG/image subresources all loaded through the host:

![hacker news](docs/demos/hacker-news.png)

`mb_shot https://example.com out.png`:

![live website](docs/demos/live-website.png)

Verified rendering a sweep of diverse real sites (example.org, danluu, gnu.org, lite.cnn,
Hacker News, rust-lang, Wikipedia, MDN, w3.org, python.org — **10/10**), including
`fetch()`-heavy, web-font, and `<video>`-containing pages. A handful of minimal blink
compatibility shims for the non-compositing offscreen widget live in `patches/` (applied
by `build.sh`).

### Demos

Modern CSS (grid + flexbox + gradient + transform + shadow) — none of which M47 could render:

![modern css](docs/demos/modern-css.png)

JavaScript mutating the DOM (bg→blue, text→"JS WORKS"):

![javascript](docs/demos/javascript.png)

`file://` load + inline SVG `<img>` decode in a flex row:

![file and image](docs/demos/file-and-image.png)

`<canvas>` 2D drawn via JavaScript (rects, arc, linear gradient, text):

![canvas 2d](docs/demos/canvas-2d.png)

## Architecture

```
┌─ outer shell (CMake, this project) ─────────────────────┐
│  wke/mb public C API  +  port/<platform> host window    │  ← future
└──────────────────── links the C ABI ▼ ─────────────────┘
┌─ miniblink_host (GN target, src/miniblink_host) ────────┐
│  mb_capi      extern "C" ABI (the seam)                 │
│  mb_runtime   engine bring-up (V8 snapshot, ThreadPool, │
│               ResourceBundle, scheduler, blink::Initialize)
│  mb_platform  blink::Platform (locale, broker, resources)│
│  mb_view*     WebView::Create + CreateMainFrame handshake│
│  mb_widget    non-compositing frame widget               │
│  paint        GetPaintRecord().Playback → SkBitmap       │
├─────────────────────────────────────────────────────────┤
│  modern Blink + substrate (base, mojo, cc, skia, v8…)   │  built as-is by GN
└─────────────────────────────────────────────────────────┘
```

The **C ABI** dissolves the GN↔CMake build mismatch: GN builds everything that touches
Blink/base/mojo C++ types; the outer shell links only the pure-C `mb_capi.h`.

See `docs/interface-surface.md` for the exact minimal Blink embedding surface, and
`PROGRESS.md` for the full build journal (every fix, file:line-cited).

## Public C ABI (`src/miniblink_host/capi/mb_capi.h`)

```c
int   mbInitialize(void);                 // boot the engine (once)
mbView* mbCreateView(int w, int h);
void  mbLoadHTML(mbView*, const char* html, const char* base_url);
void  mbLoadURL(mbView*, const char* url);          // file:// today
void  mbRunJS(mbView*, const char* script);         // host -> page: drive it
int   mbEvalJS(mbView*, const char* script, char* out, int cap);  // host <- page: read back
void  mbSendMouseClick(mbView*, int x, int y);      // synthesize a click
void  mbSendMouseMove(mbView*, int x, int y);       // move pointer: hover + mousemove
void  mbSendText(mbView*, const char* text);        // type UTF-8 into the focused element
void  mbSendScroll(mbView*, int x, int y, int dx, int dy);  // scroll the page (dy>0 = down)
int   mbPaintToBitmap(mbView*, void* bgra, int w, int h, int stride);
int   mbSavePng(mbView*, const char* path, int w, int h);  // render -> PNG file
void  mbResize(mbView*, int w, int h);
void  mbDestroyView(mbView*);
void  mbShutdown(void);
```

## Build

Currently built as a GN target inside a configured Chromium M150 checkout (the engine is
too large to vendor as source). See `build.sh` and `docs/phase-1-spec.md`. The
"standalone" deliverable = this project's source + the GN-built `libminiblink_host.dylib`
+ `blink_resources.pak` (vendored next to the binary).

Requirements: a Chromium M150 source tree with a component `out/Release`
(`is_component_build=true`), macOS arm64, the matching `blink_resources.pak`.

```sh
./build.sh /path/to/chromium-150.x.y.z   # stages host into the tree, gn gen, ninja, runs the suite
```

`mb_smoke` is a 14-case capability test suite (HTML/DOM, JS, CSS computed style, UA
stylesheet, the `mbRunJS`+`mbEvalJS` bridge, `<canvas>` getImageData, external `<link>`
CSS via the subresource loader, paint-to-bitmap, synthesized click, typed text (ASCII +
UTF-8 accent/CJK/emoji), programmatic scroll, mouse-move/hover, and embedded-NUL document
integrity) — it prints PASS/FAIL per case and exits non-zero on any failure, so it doubles
as a regression test.
