// miniblink-modern public C ABI — the GN<->CMake seam.
//
// This is the ONLY surface the CMake-built outer shell (wke/mb, port/) links
// against. Everything below it is GN-built C++ that touches Blink/base/mojo.
// Pure C, no Blink types leak across this boundary.
//
// Status: Phase 1 v0 (render-to-bitmap, no input/JS-interaction yet).

#ifndef MINIBLINK_HOST_CAPI_MB_CAPI_H_
#define MINIBLINK_HOST_CAPI_MB_CAPI_H_

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

#if defined(_WIN32)
#define MB_EXPORT __declspec(dllexport)
#else
#define MB_EXPORT __attribute__((visibility("default")))
#endif

typedef struct mbView mbView;

// Process-wide engine bring-up / teardown. Call mbInitialize() once on the
// thread that will be Blink's main thread, before any other call.
// Internally: builds the mb_platform, an (empty for now) mojo::BinderMap, then
// blink::CreateMainThreadAndInitialize(...) + CreateMainThreadIsolate().
MB_EXPORT int  mbInitialize(void);
MB_EXPORT void mbShutdown(void);

// Run pending main-thread tasks (loading, parsing, lifecycle). Call between
// load and paint, and in the host's event loop.
MB_EXPORT void mbPumpMessages(void);

// Drive the engine for ~ms of real time so setTimeout / async work runs.
MB_EXPORT void mbWait(mbView*, int ms);

// Pump until the first element matching the CSS selector exists, or timeout_ms
// elapses. Returns 1 if it appeared, 0 on timeout. (Puppeteer-style waitForSelector;
// lets a capture wait for JS-rendered / delayed content.)
MB_EXPORT int mbWaitForSelector(mbView*, const char* css_selector, int timeout_ms);

// Pump until the JS expression `js_expr` evaluates truthy, or timeout_ms elapses
// (Puppeteer-style waitForFunction). Exceptions count as falsey. Returns 1 if it
// became truthy, 0 on timeout. Generalizes mbWaitForSelector — wait on any
// condition, e.g. "window.appReady" or "document.querySelectorAll('.row').length>5".
MB_EXPORT int mbWaitForFunction(mbView*, const char* js_expr, int timeout_ms);

// View lifecycle. A view owns one WebView + main LocalFrame + WebFrameWidget.
MB_EXPORT mbView* mbCreateView(int width, int height);
MB_EXPORT void    mbDestroyView(mbView*);
MB_EXPORT void    mbResize(mbView*, int width, int height);

// Content entry points.
//   mbLoadHTML  — render an in-memory document (no network). First render proof.
//   mbLoadURL   — fetch via libcurl (mb_url_loader) then render.
MB_EXPORT void mbLoadHTML(mbView*, const char* utf8_html, const char* base_url);
MB_EXPORT void mbLoadURL(mbView*, const char* utf8_url);

// Execute JavaScript in the page's main frame (host-driven scripting).
MB_EXPORT void mbRunJS(mbView*, const char* utf8_script);

// Set a script that runs on every new document BEFORE the page's own scripts
// (like Puppeteer's evaluateOnNewDocument). Use it to set globals, stub or
// override APIs, or install a harness the page then observes. Pass NULL/empty
// to clear. Set before navigating.
MB_EXPORT void mbSetInitScript(mbView*, const char* utf8_script);

// Synthesize a left mouse click (down+up) at (x,y) in the view.
MB_EXPORT void mbSendMouseClick(mbView*, int x, int y);

// Click the center of the first element matching `css_selector` (Puppeteer-style
// page.click). Resolves the element's box in the page then clicks its center.
// Returns 1 on success, 0 if nothing matches or the element has no box
// (display:none / zero-size). Pair with mbWaitForSelector for dynamic content.
MB_EXPORT int mbClickSelector(mbView*, const char* css_selector);

// Fill the first <input>/<textarea> matching `css_selector` with `utf8_text`
// (Playwright-style fill): focuses it, sets the value via the native setter so
// frameworks like React observe it, and fires input+change. Returns 1 on
// success, 0 if nothing matches. Pair with mbWaitForSelector for dynamic forms.
MB_EXPORT int mbFillSelector(mbView*, const char* css_selector, const char* utf8_text);

// Move the mouse pointer to (x,y): updates :hover state, fires mouseover/mousemove.
MB_EXPORT void mbSendMouseMove(mbView*, int x, int y);

// Set the device pixel ratio (HiDPI / retina). The view keeps laying out in CSS px
// but window.devicePixelRatio reports `scale` and paint output is rasterized at
// `scale`x. Allocate paint/PNG buffers at logical_width*scale x logical_height*scale.
MB_EXPORT void mbSetDeviceScaleFactor(mbView*, float scale);

// Override the User-Agent (navigator.userAgent + outgoing HTTP requests). Call
// before mbLoadURL/mbLoadHTML so it applies to that navigation. Pass NULL/empty to
// restore the built-in default.
MB_EXPORT void mbSetUserAgent(mbView*, const char* utf8_ua);

// Capture with a transparent background (1) or opaque white (0, default). With
// transparency on, areas the page does not paint keep alpha 0 in the output.
MB_EXPORT void mbSetTransparentBackground(mbView*, int transparent);

// Enable (1, default) or disable (0) automatic image loading. Disabling speeds up
// text/HTML scraping. Call before mbLoadURL/mbLoadHTML.
MB_EXPORT void mbSetLoadImages(mbView*, int enabled);

// Emulate the prefers-color-scheme media feature: dark (1) or light (0, default),
// so pages render their dark theme. Call before mbLoadURL/mbLoadHTML.
MB_EXPORT void mbSetDarkMode(mbView*, int dark);

// Set navigator.language / navigator.languages (a comma-separated list, e.g.
// "fr-FR,fr,en"), for JS that localizes by the user's languages. Before navigating.
MB_EXPORT void mbSetLocale(mbView*, const char* utf8_languages);

// Override the timezone for Date and Intl (an IANA id, e.g. "America/New_York"),
// so time-dependent UIs render deterministically. Process-global.
MB_EXPORT void mbSetTimezone(mbView*, const char* iana_tz);

// Set extra HTTP request headers added to the navigation and its subresources:
// newline-separated "Name: Value" lines. Call before mbLoadURL. (A default
// Accept-Language is sent unless one is provided here.) Pass NULL/empty to clear.
MB_EXPORT void mbSetExtraHeaders(mbView*, const char* utf8_headers);

// Type ASCII text into the focused element (synthesized key events).
MB_EXPORT void mbSendText(mbView*, const char* utf8_text);

// Synthesize a gesture scroll at (x,y) by (dx,dy) pixels. Positive dy scrolls
// the page downward (toward larger window.scrollY), matching natural intent.
MB_EXPORT void mbSendScroll(mbView*, int x, int y, int dx, int dy);

// Write the HTTP cookie jar's cookies for `url` ("name=value; name2=value2") into
// `out` (NUL-terminated, up to out_cap). For exporting a session after a login flow.
// Returns the full length in bytes; empty for non-http(s) URLs.
MB_EXPORT int mbGetCookies(mbView*, const char* url, char* out, int out_cap);

// Write the committed main document's URL (the final URL after any redirects)
// into `out` (NUL-terminated, up to out_cap). Returns the full length in bytes.
MB_EXPORT int mbGetURL(mbView*, char* out, int out_cap);

// Write the current document's title into `out` (NUL-terminated, up to out_cap).
// Returns the full length in bytes.
MB_EXPORT int mbGetTitle(mbView*, char* out, int out_cap);

// Write the page's visible text (document.body.innerText) into `out`. Returns the
// full length in bytes (call with out=NULL/out_cap=0 first to size the buffer).
MB_EXPORT int mbGetText(mbView*, char* out, int out_cap);

// Write the rendered (post-JS) DOM as serialized HTML
// (document.documentElement.outerHTML) into `out`. Returns the full length in
// bytes (size first with out=NULL/out_cap=0 — pages can be large).
MB_EXPORT int mbGetHTML(mbView*, char* out, int out_cap);

// Re-navigate to the current document's URL, re-fetching it. No-op for in-memory
// documents (about:blank, data:, mbLoadHTML content).
MB_EXPORT void mbReload(mbView*);

// Host-driven back/forward over the main frame's navigation history (captures
// both host-initiated loads and page-initiated link/location/form navigations).
// mbGoBack/mbGoForward return 1 if they navigated, 0 if there was no entry.
MB_EXPORT int mbCanGoBack(mbView*);
MB_EXPORT int mbCanGoForward(mbView*);
MB_EXPORT int mbGoBack(mbView*);
MB_EXPORT int mbGoForward(mbView*);

// Drain captured page console output (console.log/warn/error) into `out`
// (NUL-terminated, up to out_cap bytes; one "level: text" line per message), and
// clear the buffer. Returns the full output length in bytes.
MB_EXPORT int mbDrainConsole(mbView*, char* out, int out_cap);

// Evaluate JS and write its result (coerced to string) into `out` (NUL-terminated,
// up to out_cap bytes). Returns the full result length in bytes (may exceed out_cap-1,
// indicating truncation). Lets the host read data back from the page (e.g. document.title).
MB_EXPORT int mbEvalJS(mbView*, const char* utf8_script, char* out, int out_cap);

// Like mbEvalJS, but runs in a dedicated ISOLATED world: the script has its own
// JS globals (separate from the page and from mbRunJS/mbEvalJS's main world) yet
// shares the same DOM — the content-script model, for automation that must not
// collide with or be observed by page script.
MB_EXPORT int mbEvalJSIsolated(mbView*, const char* utf8_script, char* out, int out_cap);

// Synchronously composite the current frame and copy pixels out as BGRA8888.
// 'out_bgra' must hold height*stride bytes. Returns 1 on success, 0 otherwise.
MB_EXPORT int mbPaintToBitmap(mbView*,
                              void* out_bgra,
                              int width,
                              int height,
                              int stride);

// Render the current frame and encode it to `path`. The image format follows the
// extension: .jpg/.jpeg -> JPEG (quality 90), anything else -> PNG. Returns 1 on success.
MB_EXPORT int mbSavePng(mbView*, const char* path, int width, int height);

// Print the document to a multi-page PDF (US Letter) at `path`. Returns 1 on success.
MB_EXPORT int mbSavePdf(mbView*, const char* path);

// Render just the logical rect (x,y,w,h) of the page to a PNG (e.g. an element
// screenshot). The output image is (w*dsf x h*dsf) px. Returns 1 on success.
MB_EXPORT int mbSavePngRect(mbView*, const char* path, int x, int y, int w, int h);

// Composite the logical rect (x,y,w,h) into a caller-provided BGRA8888 buffer
// (w x h px, `stride` bytes/row; the device scale factor is not applied here).
MB_EXPORT int mbPaintRectToBitmap(mbView*, void* out_bgra, int x, int y, int w,
                                  int h, int stride);

#if defined(__cplusplus)
}  // extern "C"
#endif

#endif  // MINIBLINK_HOST_CAPI_MB_CAPI_H_
