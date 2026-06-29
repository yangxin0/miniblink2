# samples — macOS wke sample apps

Runnable Cocoa GUI sample apps that drive the **modern** miniblink engine through the
`wke` C compatibility API, ported from the miniblink49 macOS port (`port/mac/`).

| Sample | What it is |
|--------|------------|
| **`wkexe`** | A generic "open a file or URL" browser — the macOS port of the classic Windows `wkexe/` runner. Resolves its argument to a URL (a full `scheme://` is used as-is; otherwise it looks for a local `index.html`/`main.html`/`wkexe.html`), opens an NSWindow (normal or `--transparent`/frameless), and wires the usual callbacks (page title → window title, etc.). |
| **`minibrowser`** | A minimal browser **with chrome**: a toolbar with ◀ ▶ ⟳ buttons and an address bar, plus the page view. Demonstrates `wkeGoBack`/`wkeGoForward`/`wkeReload`/`wkeLoadURL`, URL/title/loading callbacks, and console routing. |

Both host an **off-screen** `wkeWebView` inside an `NSWindow`: each frame they pull the
engine's rendered RGBA pixels via `wkePaint()` and blit them with CoreGraphics (at the
Retina backing scale for crisp output), and forward Cocoa mouse/keyboard/scroll events to
the engine via `wkeFireMouseEvent` / `wkeFireKeyDownEvent` / etc. The engine itself has no
native-window path — these samples *are* the window host.

## Build

The samples link against `libminiblink_host.dylib` (which contains the wke layer), so build
the host lib first, then the samples:

```sh
# 1. build the engine + wke layer (from the project root)
./build.sh /path/to/chromium-150.0.7871.24

# 2. build the samples
./samples/build.sh /path/to/chromium-150.0.7871.24
```

`samples/build.sh` compiles each `*_main.mm` and places the binary **in the donor tree's
`out/Release/`**, next to the engine's runtime data (`icudtl.dat`, the resource paks, the
v8 snapshot) and the dylib — the engine loads ICU/resources relative to the executable, so
a sample built elsewhere aborts with `icudtl.dat not found`. (The donor-tree path defaults
to `/Users/yangxin/dennis/chrome/chromium-150.0.7871.24` if omitted.)

## Run

```sh
cd /path/to/chromium-150.0.7871.24/out/Release
./wkexe https://example.com
./wkexe -t index.html              # frameless / transparent, local file
./minibrowser https://example.com  # with address bar + back/forward
```

## Modern-port notes

The originals targeted miniblink49's old blink-53 engine; a few things changed for the M150
engine (see the `NOTE (modern port)` comments in the sources):

- **No `wkeOnPaintUpdated` / `wkeRepaintIfNeeded`.** The modern wke renders *synchronously*,
  so there is no async paint signal — a 60 fps `NSTimer` drives `setNeedsDisplay:` and
  `drawRect:` pulls fresh pixels via `wkePaint()`.
- **No JS polyfills.** miniblink49 injected `IntersectionObserver` / `ResizeObserver` /
  `customElements` / `requestIdleCallback` / `navigator.permissions` shims via
  `wkeOnDidCreateScriptContext`; M150 ships all of those natively, so that whole block is gone.
- **No UA spoof.** The engine already reports a real Chrome 150 UA (plus UA-CH client hints),
  so the old `Chrome/132` override was dropped.
- **No `wkeFireContextMenuEvent`** in the modern wke subset (right-click still sends a normal
  mouse event).
