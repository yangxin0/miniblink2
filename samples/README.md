# samples — the mb C API by example (macOS + Windows)

The Ultralight-1.4 sample set, reimplemented on the **miniblink2 `mb` C API**
— same numbering, so the two SDKs can be compared side by side. The sample
code is **OS-independent**: everything platform-specific lives in the shared
scaffold under `compat/` (`compat/mb_window.h` is the interface;
`compat/mac/mb_window.mm` is the Cocoa backend, `compat/win/mb_window.cc` the
Win32 one). The scaffold is the samples-level analog of Ultralight's AppCore —
deliberately NOT part of the SDK, because the engine is windowless by design
and a couple hundred lines per platform is all a real host needs.

| # | Sample | What it shows |
|---|--------|---------------|
| 1 | `sample1_render_to_png` | Headless URL → PNG in five calls (`mbLoadURL`, `mbWaitForNetworkIdle`, `mbSavePng`). The production version of this idea is the `mb_shot` tool. |
| 2 | `sample2_basic_app` | A window showing an interactive in-memory page — the whole app is ~20 lines over the scaffold. |
| 3 | `sample3_resizable_app` | Window resizes → `mbResize` (logical CSS px) → live relayout; `devicePixelRatio` from `mbSetDeviceScaleFactor`. |
| 4 | `sample4_javascript` | The JS ⇄ native bridge: `mbJsBindFunction` (string + JSON returns), `mbOnWindowObjectReady` for host-computed setup before any page script. |
| 5 | `sample5_file_loading` | `file://` documents with `file://` subresources, plus a "virtual file" served from memory by `mbMockResponse` (assets in `assets/`). |
| 6 | `sample6_intro_c_api` | PLAIN C (`-std=c99`), headless: init → load → `mbPaintToBitmap` → pixel check → `mbEvalJS` read-back. Proves the headers are C-clean. |
| 7 | — | *Not ported*: Ultralight's OpenGL sample needs its GPUDriver abstraction (views render into host GL textures). miniblink2 renders to pixels; upload the BGRA buffer as a texture instead. |
| 8 | `minibrowser` | A browser **with chrome** (toolbar, address bar, ◀ ▶ ⟳): history, URL/title/console callbacks, engine-pushed cursors (`mbOnCursorChanged`), `window.open` handling, trusted input. macOS-only (Cocoa widgets). |
| 9 | `sample9_multi_window` | Two windows, two views: an editor pushes text to native (`mbJsBindFunction`) and the host re-renders a preview — `mbDefer` (never re-enter the engine from a JS callback) + `mbLoadHTMLEx(add_to_history=0)`. |

Windowed samples pull the engine's rendered BGRA pixels each frame via
`mbRepaintToBitmap()` — the fast interactive repaint, damage-gated with
`mbViewIsDirty` — and blit them natively (CoreGraphics / GDI), forwarding
mouse/keyboard/scroll as trusted `mbSend*` events. That loop IS the canonical
interactive host tick documented at the top of `webview.h`.

## Build — macOS

```sh
# 1. against the DEV (component) build — fast iteration
./build.sh /path/to/chromium-150.0.7871.24     # engine first (from the project root)
./samples/build.sh /path/to/chromium-150.0.7871.24   # -> out/Release/<samples>

# 2. against the packaged SDK in dist/<mode>/ — what consumers get
scripts/build-lib.sh --release                  # SDK first
scripts/build-samples.sh --dyn                  # -> dist/release/<samples>
```

## Build — Windows (x64)

From a "x64 Native Tools" VS prompt:

```bat
powershell -File scripts\build-lib.ps1 --release   & rem SDK first
powershell -File samples\build.ps1                 & rem -> dist\release\<samples>.exe
```

## Run

Run FROM the build/dist dir — the engine loads its runtime data (`icudtl.dat`,
the paks, the V8 snapshots) from beside the executable:

```sh
cd dist/release           # or out/Release, or dist\release on Windows
./sample2_basic_app
./sample1_render_to_png https://example.com shot.png
./minibrowser https://example.com
```

`MB_SAMPLE_AUTOEXIT_MS=1500` makes any windowed sample exit after 1.5 s
(smoke-run support).
