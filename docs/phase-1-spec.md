# Phase 1 — Minimal in-process host (`miniblink_host`) + libcurl loader

**Status:** spec (2026-06-23). **Depends on:** P0 render proof (build in flight).
**Goal:** render a real page to a bitmap through a C ABI, with no browser process —
turning the `frame_test_helpers` blueprint (see `interface-surface.md`) into real,
non-test, libcurl-backed code.

## Milestone (definition of done)
1. `mbLoadHTML("<h1>hello</h1>")` → `mbPaintToBitmap` → a PNG showing rendered text.
2. `mbLoadURL("http://<static page>")` fetched **via libcurl** → rendered to bitmap.
No JS interaction, no input, no GPU yet (those are P2/P3/P4).

## Host file layout (`src/miniblink_host/`)
Each maps 1:1 to an `interface-surface.md` row.

| File | Implements | From blueprint |
|---|---|---|
| `runtime/mb_runtime.{h,cc}` | `blink::CreateMainThreadAndInitialize(Platform*, BinderMap*)`, main-thread message loop, scheduler bring-up, `WebAgentGroupScheduler` construction | `frame_test_helpers.cc:426`, `public/web/blink.h:73` |
| `platform/mb_platform.{h,cc}` | `blink::Platform` subclass — threads, fonts (skia), clipboard, resource bundle, time | `TestingPlatformSupport` (`platform/testing/testing_platform_support.h:58`) |
| `frame/mb_view_client.{h,cc}` | `WebViewClient` (minimal) | test client in `frame_test_helpers` |
| `frame/mb_frame_client.{h,cc}` | `WebLocalFrameClient` — navigation/loading callbacks | `TestWebFrameClient` |
| `widget/mb_widget.{h,cc}` | `WebFrameWidget` host side; `InitializeCompositing`; drive synchronous composite | `TestWebFrameWidget` (`:689,:1112`), `SimCompositor` |
| `compositor/mb_sw_frame_sink.{h,cc}` | software `cc::LayerTreeFrameSink` → raster into an `SkBitmap` | `AllocateNewLayerTreeFrameSink` (`:1112`) |
| `loader/mb_url_loader.{h,cc}` | libcurl-backed loader at the `URLLoader` level; feeds `DidReceiveResponse/Data/FinishLoading` | `SimNetwork : URLLoaderTestDelegate` (`sim_network.h:22`) |
| `view/mb_webview.{h,cc}` | owns WebView+main frame+widget; does the `WebView::Create` (`:778`) + `CreateMainFrame` (`:489`) handshake with mostly-null browser handles | both embedding calls |
| `capi/mb_capi.{h,cc}` | `extern "C"` ABI (the GN↔CMake seam) | — |
| `BUILD.gn` | GN target: `shared_library("miniblink_host")` deps → `//third_party/blink/renderer/controller`, `//third_party/blink/renderer/core`, `//cc`, `//skia`, `//base`, `//mojo` | — |

## C ABI v0 (`capi/mb_capi.h`) — the seam
```c
typedef struct mbView mbView;
int   mbInitialize(void);                 // CreateMainThreadAndInitialize once
mbView* mbCreateView(int w, int h);
void  mbLoadHTML(mbView*, const char* utf8_html, const char* base_url);
void  mbLoadURL(mbView*, const char* url);          // libcurl
void  mbResize(mbView*, int w, int h);
void  mbPumpMessages(void);                          // run main-thread tasks
int   mbPaintToBitmap(mbView*, void* out_bgra, int width, int height, int stride);
void  mbDestroyView(mbView*);
void  mbShutdown(void);
```
CMake side (wke/mb, port/) links only this header + the host shared lib.

## Build strategy (and how "standalone" is achieved)
- **Iterate fast IN the donor tree:** develop the host as GN target `//miniblink_host`
  physically inside `chromium-150.../` so it links Blink directly (GN can't reach outside
  its source root). Fastest edit-compile-render loop.
- **Then make it standalone (copy step the user asked for):**
  1. copy `//miniblink_host/*` → `miniblink-modern/src/miniblink_host/`.
  2. vendor what the standalone build needs into `miniblink-modern/src/`:
     - blink **public headers** (`third_party/blink/public/**`) + the small set of
       non-public headers the host includes, under original paths.
     - prebuilt component **dylibs** from donor `out/Release` (blink, base, cc, skia, v8…)
       into `miniblink-modern/vendor/lib/` (documented manifest).
  3. standalone build = GN (or a thin ninja/CMake) target in miniblink-modern linking
     those headers + dylibs. Truly self-contained source tree; binary deps vendored.
  - Honest note: a *source* copy of all of Blink+substrate is the whole tree; "standalone"
    here = our source + vendored prebuilt engine libs + public headers, not a from-scratch
    rebuild of Chromium.

## Risks
- Software `LayerTreeFrameSink` may need more of viz than expected → if so, lift
  `SimCompositor`'s exact path verbatim first, optimize later.
- `blink::Platform` surface is wide; implement lazily — stub a method, run, fill what
  Blink actually calls (CHECK failures will name them).
- Scheduler/message-loop bring-up order is finicky — copy `frame_test_helpers` setup order.
