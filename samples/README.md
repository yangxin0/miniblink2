# samples — macOS sample app

A minimal windowed browser built on the **miniblink2 `mb` C API** (`miniblink2.h`).

| Sample | What it shows |
|--------|---------------|
| **`minibrowser`** | A minimal browser **with chrome**: a toolbar with ◀ ▶ ⟳ buttons and an address bar, plus the page view. Demonstrates `mbGoBack`/`mbGoForward`/`mbReload`/`mbLoadURL`, the URL/title/load callbacks (`mbOnUrlChanged`/`mbOnTitleChanged`/`mbOnLoadFinish`), console routing (`mbOnConsoleMessage`), and trusted input (`mbSendMouse*`, `mbSendWheel`, `mbSendKey*`, `mbSendText`). |

It hosts an **off-screen** `mbView` inside an `NSWindow`: each frame it pulls the
engine's rendered BGRA pixels via `mbRepaintToBitmap()` — the fast interactive repaint
(no per-call lifecycle settle, unlike the one-shot `mbPaintToBitmap`) — and blits them
with CoreGraphics at the display's backing scale (Retina-crisp), forwarding Cocoa
mouse/keyboard/scroll events to the engine. The engine itself has no native-window
path — the sample *is* the window host.

## Build

Two ways, depending on which library you want to exercise:

```sh
# 1. against the DEV (component) build — fast iteration
./build.sh /path/to/chromium-150.0.7871.24    # engine first (from the project root)
./samples/build.sh /path/to/chromium-150.0.7871.24   # -> out/Release/minibrowser

# 2. against the packaged SDK in dist/<mode>/ — what consumers get
scripts/build-lib.sh --release --ship          # SDK first (dylib + .a)
scripts/build-samples.sh                       # -> dist/release/minibrowser_{dyn,static}
```

`samples/build.sh` places the binary **next to the engine's runtime data**
(`icudtl.dat`, the resource paks, the V8 snapshot) — the engine loads those relative
to the executable, so a sample built elsewhere aborts with `icudtl.dat not found`.

## Run

```sh
cd /path/to/out-or-dist
./minibrowser https://example.com  # with address bar + back/forward
```
