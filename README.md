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
| **Cutting-edge CSS**: `:has()`, native nesting, `@container`, `color-mix()`, `oklch()` | ✅ |
| **Web Components**: Custom Elements v1 + Shadow DOM (encapsulated) | ✅ |
| **JavaScript** (V8) + DOM mutation → style recalc → relayout → repaint | ✅ |
| `<canvas>` 2D drawing via JS (shapes, gradients, text → skia) | ✅ |
| `mbLoadURL("file://…")` — load a document from disk | ✅ |
| Image decode + SVG rendering (data: URIs **and external files**) | ✅ |
| **Subresource loading** (external `<link>` CSS, `<img>`) via a `blink::URLLoader` | ✅ |
| In-process `MimeRegistry` (so `file://` stylesheets validate) | ✅ |
| **HTTP/HTTPS loading of live websites** via system libcurl | ✅ |
| Paint readback to a BGRA8888 bitmap + PNG; **PDF export** (paginated, via Blink print) | ✅ |
| Input events (click, move/hover, type, scroll), HiDPI, custom UA | ✅ |
| **DOM storage** (`localStorage` / `sessionStorage`, in-memory) | ✅ |
| `requestAnimationFrame` (serviced without a compositor) | ✅ |
| Observers: Mutation, **Intersection** (forced past offscreen throttling), Resize | ✅ |
| Web/CSS Animations advance (clock serviced); `XMLHttpRequest` + `fetch` | ✅ |
| `structuredClone`; Web Crypto (`SubtleCrypto.digest`, in a secure-context page) | ✅ |
| Console capture (`console.log`/`warn`/`error` → host) | ✅ |
| Init scripts (`evaluateOnNewDocument`: run JS before page scripts) | ✅ |
| Isolated-world eval (content-script model: separate globals, shared DOM) | ✅ |
| **Cookies**: HTTP jar + JS `document.cookie`; JS→jar bridge; jar export (`mbGetCookies`) | ✅ |
| Custom request headers + default `Accept-Language` | ✅ |
| On-screen window, GPU compositing | ⏳ roadmap |
| IndexedDB (API present; `open()` degrades gracefully via `onerror` — needs an in-process `IDBFactory` backing store wired through the frame broker) | ⏳ roadmap |
| Dedicated Web Workers (`new Worker(...)` constructs but the worker thread isn't serviced — needs worker-thread infrastructure) | ⏳ roadmap |

## Tool: `mb_shot` (headless HTML → PNG)

The deliverable example app — a standalone headless screenshot renderer:

```sh
mb_shot \
  # request config
  [--user-agent UA] [--header "N: V"] [--proxy URL] [--insecure] [--no-follow] \
  [--block SUBSTR] [--load-cookies FILE] [--save-cookies FILE] [--post BODY] \
  [--no-images] [--dark] [--lang L,L2] [--tz Area/City] \
  # interact
  [--fill CSS TEXT] [--click CSS] [--drag FROM TO] [--dispatch CSS EVT] [--press KEY] \
  # synchronize
  [--wait-selector CSS] [--wait-visible CSS] [--wait-hidden CSS] [--wait-eval JS] [--wait-idle] [--wait-ms N] \
  # prepare the view
  [--css STYLES] [--auto-scroll] [--scroll-to Y] [--scroll-to-selector CSS] \
  # extract (to stdout)
  [--title] [--url] [--cookies URL] [--local-storage KEY] [--session-storage KEY] \
  [--text] [--html] [--html-for CSS] [--eval JS] [--eval-json JS] [--value CSS] [--checked CSS] [--count CSS] [--visible CSS] \
  [--rect CSS] [--style CSS PROP] [--text-all CSS] [--attr CSS NAME] [--attr-all CSS NAME] \
  [--requests] [--console] [--headers] \
  # capture
  [--full] [--scale N] [--mobile] [--clip x,y,w,h | --selector CSS] [--transparent] \
  # assert (scripting)
  [--require CSS] \
  <input.html | file://URL | http(s)://URL> <out.png> [width height]
```

`--mobile` presets a phone emulation in one flag: a 390×844 viewport, `devicePixelRatio`
3, and an iPhone Safari User-Agent — so responsive sites serve their mobile layout
(width media queries track the view size; an explicit width/height, `--scale`, or
`--user-agent` each still overrides its part). Example: `mb_shot --mobile https://news.site shot.png`.

For scripting, `--require CSS` makes the run **assert** the page contains a match for
`CSS` after all waits/interaction — exit `3` if it doesn't (the capture is still
written for debugging). This turns "the data is here" vs "the page didn't load / the
element never appeared" into a reliable exit code, which the warn-only `--wait-*` flags
and a successful-but-empty local-file load otherwise don't signal. (Exit codes: `0` ok,
`1` load/capture failed — including a missing local input file, `2` usage error,
`3` `--require` unmet.)

`--full` captures the entire document height (the view is resized to the page's
`scrollHeight` before rendering, capped at 20000px), like Puppeteer's `fullPage` — e.g.
`mb_shot --full https://go.dev out.png` produces a 1200×3969 image of the whole page
instead of just the 1200×900 viewport.

`--scale N` renders at a device pixel ratio of N: the page lays out at `[width height]`
CSS px but `window.devicePixelRatio == N` and the PNG is `width*N × height*N` — retina-crisp
text and 2x `srcset`/`min-resolution` media-query selection. The flags compose, e.g.
`mb_shot --full --scale 2 https://go.dev out.png` → a 2400×7938 whole-page @2x capture.

`--clip x,y,w,h` captures only that logical rectangle; `--selector CSS` captures only the
bounding box of the first element matching the selector (an element screenshot) — e.g.
`mb_shot --selector "#card" page.html card.png` writes a PNG sized exactly to that element.
Clip/selector compose with `--scale` (the output is `w*N × h*N`).

`--transparent` captures with a transparent background (Puppeteer's `omitBackground`): areas
the page doesn't paint keep alpha 0, so the PNG can be composited over other content.

`--wait-selector CSS` waits (driving timers/async) until an element matching the selector
exists before capturing — for JS-rendered content (Puppeteer's `waitForSelector`); `--wait-ms
N` just settles the page for N ms. Both compose with the capture options. `--fill CSS TEXT`
types `TEXT` into the field matching the selector (firing `input`/`change`, so frameworks
react), and `--click CSS` clicks the matching element before capturing (e.g. fill a search
box then click submit, expand a menu, dismiss a banner). `--fill` runs before `--click`.
`--drag FROM TO` mouse-drags one element's center onto another's (slider, sortable,
map pan); `--dispatch CSS EVT` fires a synthetic DOM event (e.g. `mouseover` to open
a hover menu, or a custom event) that click/fill don't. `--press KEY` presses a named
non-text key as a trusted event so its default action fires — `Enter` to submit
(`--fill q --press Enter`), `Tab` to advance focus, `Escape`, arrows; it runs last in
the interact phase, after `--fill`/`--click`.

Beyond `--wait-selector`, the synchronization set covers the appear/disappear/quiet
lifecycle: `--wait-visible CSS` waits until the element is actually shown (not just
present — `display:none`/`opacity:0` don't count), `--wait-hidden CSS` waits until it
goes away (the "spinner disappeared" signal), `--wait-eval JS` waits until an arbitrary
JS expression is truthy (Puppeteer's `waitForFunction`, e.g. `window.appReady` or
`items.length>10` — any condition a selector can't express), and `--wait-idle` waits
until the page stops making network requests (Puppeteer's `networkidle`, for SPAs that
lazy-fetch).
`--css STYLES` injects a stylesheet (hide cookie banners / ads / sticky headers before
a shot), `--auto-scroll` scrolls the page to trigger lazy-loaded / infinite-scroll
content, and `--scroll-to-selector CSS` brings a specific element into the viewport
(in context, vs `--selector`'s element-only clip). `--user-agent UA` (alias `--ua`) overrides the
User-Agent (many sites serve different markup to mobile vs desktop), and `--block
SUBSTR` (repeatable) drops any request whose URL contains the substring (ads,
trackers, images) for faster, cleaner captures.

The extraction flags read structured data to stdout: `--value CSS` (a control's live
`.value`), `--checked CSS` (checkbox/radio `1`/`0`), `--count CSS` (number of matches,
`querySelectorAll` length), `--visible CSS` (`1`/`0`/`-1`),
`--rect CSS` (`x,y,w,h`), `--style CSS PROP` (a resolved computed style), `--attr CSS
NAME` (the first match's attribute — an `href`/`src`/`content`), `--text-all
CSS` / `--attr-all CSS NAME` (a JSON array across *all* matches — one-shot list
scraping), and `--requests` (the subresource URLs the page fetched).

`--console` prints the page's captured console output (`console.log`/`warn`/`error`) to
stderr — useful for debugging a page or scripting against its logs. `--headers` prints the
server's HTTP response headers (Content-Type, caching, custom/API headers) to stderr.

`--title` prints `document.title` and `--url` prints the current document URL (the
landing URL after any redirects) to stdout — the basic page-metadata fields. `--cookies
URL` prints the jar's cookies for that origin (`name=value; name2=value2`) — the
inspection peer of `--set-cookie`/`--save-cookies`, e.g. read a session token after a
login flow. `--local-storage KEY` / `--session-storage KEY` print a Web Storage value
for the document's origin (an SPA's auth token / app state); an absent key prints an
empty line and warns on stderr. `--text`
prints the page's visible text (post-JS `document.body.innerText`) to stdout, so
`mb_shot` doubles as a simple scraper/text extractor. `--html` prints the rendered
(post-JS) DOM as serialized HTML — useful for SPAs whose fetched source is near-empty —
and `--html-for CSS` prints just the first match's `outerHTML` (one fragment: an article
body, a table, a card) instead of the whole document.
`--eval JS` runs an arbitrary JS expression against the settled page and prints its string
result to stdout — the whole scripting surface from the command line, e.g.
`mb_shot --eval "document.querySelectorAll('.item').length" page.html out.png` for a count,
or reading a computed style / attribute. Compose with `--fill`/`--click`/`--wait-*` to
interact first, then extract. `--eval-json JS` is the structured-scraping variant — it
`JSON.stringify`s the expression, so an object or array comes back as real JSON instead
of `[object Object]` or a lossy comma-join; e.g.
`mb_shot --eval-json "[...document.querySelectorAll('.item')].map(e=>({t:e.textContent,href:e.querySelector('a').href}))"`
yields a JSON array of records ready to pipe into `jq`.

`--no-images` disables network image loading (faster text/HTML scraping; inline `data:`
images are unaffected). `--dark` emulates `prefers-color-scheme: dark` so pages render their
dark theme. `--lang "fr-FR,fr,en"` sets `navigator.language(s)` for locale-aware pages, and
`--tz "America/New_York"` overrides the timezone for `Date`/`Intl`. `--proxy
"http://host:port"` (or `socks5://host:port`) routes all network fetches through a proxy.
`--load-cookies FILE` / `--save-cookies FILE` restore and persist the cookie jar (Netscape
format) so a login survives across runs — log in once with `--save-cookies`, then reuse it
with `--load-cookies` on later runs. `--insecure` skips TLS certificate verification (like
`curl -k`), for sites with self-signed, expired, or otherwise invalid certs.
`--no-follow` stops at a 3xx redirect instead of following it, so `--headers`
shows the `Location` (resolve a shortener / inspect a redirect without following).

The output format follows the file extension: `.png` (lossless, alpha), `.jpg`/`.jpeg`
(quality 90, much smaller), or `.pdf` (a paginated US-Letter PDF via Blink's print path) —
e.g. `mb_shot https://example.com out.jpg` or `mb_shot article.html article.pdf`.

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
┌─ wke compatibility layer (src/wke) ─────────────────────┐
│  the classic miniblink `wke` C API on modern Blink      │
└──────────── wraps the mb_capi ABI ▼ ────────────────────┘
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

See `PROGRESS.md` for the current state, plan, and build/test commands; the full
per-tick build journal lives in the git history.

## Public C ABI (`src/miniblink_host/capi/mb_capi.h`)

108 functions; the header has the full, commented signatures. The canonical flow —
boot, render, read back, screenshot, shut down:

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

A complete, runnable C-ABI example (fill → read value → dispatch a custom event →
wait for network idle → scrape text/HTML → request log → element screenshot) is
`src/miniblink_host/tools/mb_demo.cc` (the `mb_demo` target) — the C counterpart
to `wke_demo`.

Grouped overview (see `mb_capi.h` for the exact signatures):

- **Lifecycle / pump:** `mbInitialize` `mbShutdown` `mbCreateView` `mbDestroyView`
  `mbResize` `mbPumpMessages` `mbWait` `mbWaitForSelector` `mbWaitForFunction`
  `mbWaitForVisibleSelector` (waits for actual visibility, not just existence)
  `mbWaitForSelectorHidden` (waits for gone/hidden — the spinner-disappeared signal)
  `mbWaitForNetworkIdle` (waits for fetches to settle — Puppeteer networkidle)
- **Load / navigation:** `mbLoadHTML` `mbLoadURL` `mbPostURL` `mbReload`
  `mbGoBack`/`mbGoForward`/`mbCanGoBack`/`mbCanGoForward` `mbGetURL` `mbGetTitle`
  `mbGetHttpStatus` `mbGetResponseHeaders`
- **Scripting:** `mbRunJS` `mbSetInitScript` `mbInsertCSS` (addStyleTag) `mbEvalJS`
  `mbEvalJSEx` (value + JS
  type) `mbEvalJSIsolated` `mbDrainConsole` `mbJsBindFunction` (native C function
  callable from JS; returns string/number/boolean/null/JSON-object)
- **Scraping:** `mbGetText` `mbGetHTML` `mbGetTextForSelector`
  `mbGetAllTextForSelector` (JSON array, all matches)
  `mbGetAllValueForSelector` (JSON array of live values — form serialization)
  `mbGetHtmlForSelector` (element outerHTML) `mbSetHtmlForSelector` (set innerHTML)
  `mbGetAttribute`
  `mbGetAllAttributeForSelector` (JSON array of an attr, all matches)
  `mbSetAttribute` `mbGetValueForSelector` (live `.value`) `mbGetCheckedForSelector`
  (`.checked`) `mbIsVisibleForSelector` `mbGetComputedStyle` `mbCountSelector`
  `mbGetElementRect` `mbGetContentSize` `mbGetViewSize` (viewport read-back)
- **Input:** `mbSendMouseClick` `mbSendMouseDown`/`mbSendMouseUp` (drag)
  `mbSendMouseMove` `mbSendTouchTap`/`mbSendTouchSwipe` (touch) `mbSendText` `mbSendKey`
  `mbSendScroll` `mbScrollTo` `mbScrollToBottom` (auto-scroll to load lazy content);
  by selector `mbClickSelector`
  `mbDoubleClickSelector` `mbRightClickSelector` `mbHoverSelector`
  `mbFocusSelector` `mbBlurSelector` `mbFillSelector` `mbSelectOption`
  `mbDispatchEvent` (synthetic DOM events) `mbDragSelector` (drag from→to)
  `mbScrollIntoView`
- **Capture / output:** `mbPaintToBitmap` `mbPaintRectToBitmap` `mbSavePng`
  `mbSavePngRect` `mbSaveElementPng` (one element by selector) `mbSavePdf`
  `mbEncodePng` (in-memory PNG bytes)
- **Cookies / session:** `mbGetCookies` `mbGetCookie` (one by name) `mbGetAllCookies`
  (whole jar) `mbSetCookie` `mbClearCookies` `mbSaveCookies`/`mbLoadCookies` (file jar)
  `mbGetLocalStorage`/`mbSetLocalStorage` `mbGetSessionStorage`/`mbSetSessionStorage`
  `mbClearStorage` (origin-scoped Web Storage — auth/state injection + reset)
- **Network config:** `mbSetProxy` `mbSetIgnoreCertErrors` `mbSetFollowRedirects`
  `mbSetExtraHeaders` `mbSetUserAgent` `mbGetUserAgent` `mbSetLoadImages`
  `mbGetRequestLog`/`mbClearRequestLog` (subresource fetch log)
  `mbBlockUrl`/`mbClearUrlBlocks` (block URLs by substring — ads/trackers/images)
- **Page config:** `mbSetDeviceScaleFactor` `mbSetTransparentBackground`
  `mbSetDarkMode` `mbSetLocale` `mbSetTimezone` `mbSetFocus` (window focus)

## wke compatibility layer (`src/wke/wke.h`)

A drop-in subset of [miniblink](https://github.com/weolar/miniblink49)'s classic
`wke` C API, implemented on top of `mb_capi`, so an existing headless `wke` app
runs on modern Blink with the original signatures (`utf8`, `wkeWebView`, `jsValue`,
…). It is built into `libminiblink_host`; an embedder includes just `wke/wke.h`.

Supported today (every item verified by `wke_smoke` — 96 default cases, plus
over-the-network cases under `MB_NET_TESTS=1`). Functions marked *(ext)* are
port extensions over `mb_capi` beyond the classic `wke` surface:

- **Lifecycle / load:** `wkeInitialize`/`wkeFinalize`, `wkeCreateWebView`/
  `wkeDestroyWebView`, `wkeLoadURL`/`wkeLoadHTML`/`wkeLoadHtmlWithBaseUrl`
  (base origin → relative URLs + secure context), `wkePostURL`, `wkeReload`, the
  loading-state pollers (`wkeIsLoading`, `wkeIsLoadingCompleted`,
  `wkeIsLoadingSucceeded`, `wkeIsLoadingFailed`, `wkeIsDocumentReady`).
- **Geometry / rendering:** `wkeResize`, `wkeGetWidth`/`wkeGetHeight`/`wkeWidth`/
  `wkeHeight`, `wkeGetContentWidth`/`wkeGetContentHeight`,
  `wkeSetTransparent`/`wkeIsTransparent`,
  `wkeSetZoomFactor`/`wkeGetZoomFactor`, `wkeSetEditable`, `wkeSetDarkMode` *(ext)*,
  `wkeSetDeviceScaleFactor` *(ext)*, `wkeScrollTo` *(ext)*, `wkeScrollToBottom`
  (auto-scroll to load lazy content) *(ext)*, `wkeSetFocus`/`wkeKillFocus`.
- **Capture / output:** `wkePaint` (into a caller BGRA buffer), and *(ext)*
  `wkePaintRect` (a region → BGRA buffer), `wkeSavePng`/`wkeSavePngRect`
  `wkeSaveElementPng` (one element by selector) (PNG/JPEG by extension),
  `wkeSavePdf`, `wkeEncodePng` (in-memory bytes).
- **Accessors / view-state:** `wkeGetURL`/`wkeGetTitle`/`wkeGetSource`/`wkeGetText`,
  `wkeSetUserAgent`/`wkeGetUserAgent`, `wkeSetLoadImages`, `wkeSetName`/`wkeGetName`,
  `wkeSetUserKeyValue`/`wkeGetUserKeyValue`.
- **Navigation:** `wkeCanGoBack`/`wkeGoBack`/`wkeCanGoForward`/`wkeGoForward`.
- **Input:** `wkeFireMouseEvent`, `wkeFireMouseWheelEvent`, `wkeFireKeyDownEvent`/
  `wkeFireKeyUpEvent`/`wkeFireKeyPressEvent`.
- **Scripting (full string-backed `jsValue` model):** `wkeRunJS` + `wkeGlobalExec`;
  classify `jsTypeOf` + `jsIsNumber`/`String`/`Boolean`/`Object`/`Array`/`Function`/
  `Undefined`/`Null`/`True`/`False`; coerce `jsToInt`/`jsToFloat`/`jsToDouble`/
  `jsToBoolean`/`jsToTempString`/`jsToString` (JSON for objects); construct `jsInt`/
  `jsDouble`/`jsBoolean`/`jsString`/`jsUndefined`/`jsNull`; read `jsGetLength`/
  `jsGetAt`/`jsGet`/`jsGetGlobal`/`jsGetKeys`; build `jsEmptyObject`/`jsEmptyArray`
  + `jsSet`/`jsSetAt`/`jsSetGlobal`; call `jsCall`/`jsCallGlobal`; plus
  `wkeSetInitScript` (evaluateOnNewDocument), `wkeInsertCSS` (addStyleTag) *(ext)*,
  `wkeRunJsInIsolatedWorld` (content-script eval: own globals, shared DOM) *(ext)*,
  and `wkeOnJsBridge` (page↔host bridge) *(ext)*.
- **DOM automation** *(ext, Puppeteer-style)* — query `wkeCountSelector`/
  `wkeGetTextForSelector`/`wkeGetAllTextForSelector`/`wkeGetHtmlForSelector`/
  `wkeSetHtmlForSelector`/`wkeGetAttribute`/`wkeSetAttribute`/
  `wkeGetAllAttributeForSelector`/`wkeGetValueForSelector`/
  `wkeGetAllValueForSelector`/`wkeGetCheckedForSelector`/`wkeIsVisibleForSelector`/
  `wkeGetElementRect`/`wkeGetComputedStyle`; act `wkeClickSelector`/
  `wkeDoubleClickSelector`/`wkeRightClickSelector`/`wkeHoverSelector`/
  `wkeFocusSelector`/`wkeBlurSelector`/`wkeFillSelector`/`wkeSelectOption`/
  `wkeDispatchEvent` (synthetic events)/`wkeDragSelector` (drag from→to)/
  `wkeScrollIntoView`; wait `wkeWaitForSelector`/`wkeWaitForFunction`/
  `wkeWaitForVisibleSelector`/`wkeWaitForSelectorHidden`/`wkeWaitForNetworkIdle`.
- **Storage** *(ext)* — `wkeGetLocalStorage`/`wkeSetLocalStorage`,
  `wkeGetSessionStorage`/`wkeSetSessionStorage` (origin-scoped Web Storage).
- **Networking:** cookies `wkeGetCookie`/`wkeSetCookie`/`wkeGetAllCookie`/
  `wkePerformCookieCommand` + jar persistence `wkeSetCookieJarPath`; `wkeSetProxy`
  (HTTP/SOCKS + auth); and *(ext)* `wkeSetExtraHeaders`, `wkeSetLocale`/
  `wkeSetTimezone`, `wkeSetFollowRedirects`, `wkeSetIgnoreCertErrors`,
  `wkeGetHttpStatusCode`/`wkeGetResponseHeaders`,
  `wkeGetRequestLog`/`wkeClearRequestLog` (subresource fetch log),
  `wkeBlockUrl`/`wkeClearUrlBlocks` (block URLs by substring).
- **Callbacks:** `wkeOnLoadingFinish`, `wkeOnTitleChanged`, `wkeOnConsole`,
  `wkeOnDocumentReady` (+ `wkeString`/`wkeGetString`).

A complete, runnable example of the automation surface (fill → select → click →
wait → scrape → screenshot) is `src/wke/wke_demo.cc` (the `wke_demo` target).

```c
#include "wke/wke.h"

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
    wkePaint(wv, bits, w * 4);                 // … then encode/save bits …
    free(bits);
}
wkeDestroyWebView(wv);
wkeFinalize();
```

Loading is synchronous here, so a `wke` app can poll `wkeIsLoadingCompleted`
(always true after `wkeLoadURL` returns) instead of waiting on a message loop.
The `jsValue` object model is implemented as a JS-side slot store (objects/arrays
are parked in the page and navigated by `jsGet`/`jsGetAt`/`jsCall`), not raw V8
handles. Native function binding (`wkeJsBindFunction`) is supported: a C function
is installed on `window` (via v8 `CreateDataProperty` at document-element-available
— the public `Object::Set` API traps in this sandboxed build) and called
synchronously from JS, reading args with `jsArg`/`jsArgCount` (each argument
carries its JS type, so `jsTypeOf`/`jsIsNumber` are accurate). The returned
`jsValue`'s type is preserved too (number/boolean/null/string), so JS gets a real
value back — e.g. `window.fn(2,3) + 1` does arithmetic.

## Build

Currently built as a GN target inside a configured Chromium M150 checkout (the engine is
too large to vendor as source). See `build.sh` and `PROGRESS.md`. The
"standalone" deliverable = this project's source + the GN-built `libminiblink_host.dylib`
+ `blink_resources.pak` (vendored next to the binary).

Requirements: a Chromium M150 source tree with a component `out/Release`
(`is_component_build=true`), macOS arm64, the matching `blink_resources.pak`.

```sh
./build.sh /path/to/chromium-150.x.y.z   # stages host into the tree, gn gen, ninja, runs the suite
```

`mb_smoke` is a 179-check capability + regression suite covering
HTML/DOM, JS, CSS computed style, UA stylesheet, the `mbRunJS`+`mbEvalJS` bridge,
`<canvas>` getImageData, external `<link>` CSS via the subresource loader,
paint-to-bitmap, synthesized click, typed text (ASCII + UTF-8 accent/CJK/emoji),
programmatic scroll, mouse-move/hover, embedded-NUL document integrity, full-page
capture (resize → reflow → render below the fold), HiDPI (devicePixelRatio +
resolution media queries), User-Agent, clip/region capture, transparent
background, wait-for-selector/function, DOM storage, `requestAnimationFrame`,
observer delivery (Mutation/Intersection/Resize), time-based animation (WAAPI/XHR),
console capture, the selector automation set (click/dblclick/right-click/hover/
focus/blur/fill/select/scrollIntoView), computed-style/attribute/text scraping,
cookie jar save/load, PDF/PNG/in-memory-PNG export, and native function binding
(`mbJsBindFunction`). It prints PASS/FAIL per case and exits non-zero on any
failure, so it doubles as a regression test. A handful of over-the-network checks
(POST, the cookie jar, request headers, proxy, redirect/cert toggles, image
loading, HTTP status) are opt-in via `MB_NET_TESTS=1`, kept out of the default
run so an unreachable host can't make it crawl. The `wke` layer has its own
`wke_smoke` (96 default checks) and a runnable `wke_demo` example.

`build.sh` also runs `src/miniblink_host/test/mb_shot_smoke.sh`, a CLI regression
test that drives the `mb_shot` binary against a local fixture and asserts the exact
stdout of the extraction flags (`--title`/`--count`/`--attr`/`--text`/`--eval`/
`--visible`/`--value`/`--checked`/`--style`/`--html`/`--html-for`/`--rect`/`--local-storage`/
`--session-storage`/`--url`/`--cookies`/`--click`/`--press`/`--wait-eval`) plus the
bad-size guard, the capture modes (PNG dimensions for default/`--scale`/`--clip`/
`--selector`, and `.jpg`/`.pdf` output formats read from the file header), and an
end-to-end `fill`→`click`→`wait-selector`→`eval` integration flow (the canonical
scrape, extracting result rows as JSON) — coverage the C++ suites can't give the
command-line tool itself. It runs 62 deterministic offline cases by default;
`MB_NET_TESTS=1` adds reachability-gated live-network cases (loading `example.com`;
`--header`/`--post` echoed by httpbin; `--no-follow` stopping at a 3xx; `--insecure`
loading a self-signed cert site).
