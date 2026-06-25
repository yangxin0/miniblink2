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
// thread that will be Blink's main thread, before any other call. It is idempotent:
// extra calls return success and reuse the running engine.
// Internally: builds the mb_platform, an (empty for now) mojo::BinderMap, then
// blink::Initialize(...) (which creates the main-thread V8 isolate).
// mbShutdown() is a safe no-op: Blink's global init is one-time per process and
// cannot be re-created, so the engine stays resident until the process exits (a later
// mbInitialize reuses it). It exists for API symmetry; you may also just exit.
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

// Like mbWaitForSelector but waits for the first match to be actually VISIBLE
// (checkVisibility — not display:none / visibility:hidden / opacity:0), not just
// present in the DOM. Returns 1 once it's shown, 0 on timeout. Use for content
// that mounts hidden then fades/toggles in (modals, lazy panels, spinners).
MB_EXPORT int mbWaitForVisibleSelector(mbView*, const char* css_selector,
                                       int timeout_ms);

// The inverse of mbWaitForVisibleSelector: wait until the first match is NOT
// visible — gone from the DOM or hidden (display:none / visibility:hidden /
// opacity:0). The "wait for the loading spinner to disappear" primitive before
// scraping. Returns 1 once gone/hidden, 0 on timeout.
MB_EXPORT int mbWaitForSelectorHidden(mbView*, const char* css_selector,
                                      int timeout_ms);

// Wait until no new subresource request has been recorded for `idle_ms`
// (Puppeteer's networkidle) — let an SPA's deferred fetches (XHR/fetch, lazy
// images) settle before scraping/capturing. Returns 1 once the network is quiet,
// 0 if `timeout_ms` elapses while still busy. Reads the process-wide request log,
// so clear it (mbClearRequestLog) before the navigation to scope it to this page.
MB_EXPORT int mbWaitForNetworkIdle(mbView*, int idle_ms, int timeout_ms);

// View lifecycle. A view owns one WebView + main LocalFrame + WebFrameWidget.
MB_EXPORT mbView* mbCreateView(int width, int height);
MB_EXPORT void    mbDestroyView(mbView*);
MB_EXPORT void    mbResize(mbView*, int width, int height);

// Content entry points.
//   mbLoadHTML  — render an in-memory document (no network). First render proof.
//   mbLoadURL   — fetch via libcurl (mb_url_loader) then render.
MB_EXPORT void mbLoadHTML(mbView*, const char* utf8_html, const char* base_url);
MB_EXPORT void mbLoadURL(mbView*, const char* utf8_url);

// Download `url` to `dest_path`: fetch through the engine network stack and write the
// body to disk WITHOUT rendering it as a document. Honors the interception layer
// (mbRewriteUrl / mbBlockUrl / mbMockResponse / mbSetRequestCallback / mbSetResponseCallback)
// and, for http(s), the view's user-agent, extra + per-URL headers, cookies and proxy.
// Works for http(s), file:// and data: URLs. Returns 1 on success, 0 on fetch/write failure.
MB_EXPORT int mbDownloadURL(mbView*, const char* url, const char* dest_path);

// Push notification of load completion — the real Blink DidFinishLoad signal (the
// main document's `load` event, all subresources done), not a poll or a fixed timer.
// Register a callback with mbOnLoadFinish (pass NULL to clear); it fires during the
// load call (the synchronous load pumps the message loop). mbIsLoadFinished queries
// the same state (1 once the current navigation's load has finished, 0 after a new
// load starts), so callers can wait on the real finish instead of a fixed delay.
typedef void (*mbLoadFinishCallback)(mbView*, void* userdata);
MB_EXPORT void mbOnLoadFinish(mbView*, mbLoadFinishCallback, void* userdata);
MB_EXPORT int  mbIsLoadFinished(mbView*);

// Navigation policy/notification: the callback fires for each PAGE-initiated main-frame
// navigation (link click, location= assignment, form submit, JS redirect) with its
// target `url`, BEFORE it commits. Return 1 to allow, 0 to BLOCK it — so you can stop
// popups / redirects / the page navigating away, or just observe navigations. Host-
// driven mbLoadURL does NOT route through here (it is your own action). NULL clears it.
typedef int (*mbNavigationCallback)(mbView*, void* userdata, const char* url);
MB_EXPORT void mbOnNavigation(mbView*, mbNavigationCallback, void* userdata);

// New-window notification: the callback fires when the page requests a new window
// (window.open() or a target=_blank activation), with the requested `url` and window
// `name`. It is a notification only — the popup is not auto-created (window.open returns
// null), so an embedder can decide what to do (e.g. load `url` in this or a new view).
// NULL clears it.
typedef void (*mbNewWindowCallback)(mbView*, void* userdata, const char* url,
                                    const char* name);
MB_EXPORT void mbOnNewWindow(mbView*, mbNewWindowCallback, void* userdata);

// Host-driven POST navigation: POST `body` to an http(s) `url` with `content_type`
// (NULL/empty -> application/x-www-form-urlencoded) and commit the response as the
// document. `body` is NUL-terminated (text bodies — form-encoded or JSON). After
// it returns, mbGetHttpStatus / mbGetResponseHeaders / mbGetURL reflect the POST.
MB_EXPORT void mbPostURL(mbView*, const char* url, const char* body,
                         const char* content_type);
// Like mbPostURL but takes an explicit `body_len`, so binary bodies with embedded
// NUL bytes (protobuf, multipart, raw uploads) post intact instead of truncating
// at the first NUL. `content_type` may be null (defaults to form-urlencoded).
MB_EXPORT void mbPostURLData(mbView*, const char* url, const char* body,
                             int body_len, const char* content_type);

// Execute JavaScript in the page's main frame (host-driven scripting).
MB_EXPORT void mbRunJS(mbView*, const char* utf8_script);

// Set a script that runs on every new document BEFORE the page's own scripts
// (like Puppeteer's evaluateOnNewDocument). Use it to set globals, stub or
// override APIs, or install a harness the page then observes. Pass NULL/empty
// to clear. Set before navigating.
MB_EXPORT void mbSetInitScript(mbView*, const char* utf8_script);

// Append a <style> element holding `css` to the document head (like Puppeteer's
// addStyleTag) — restyle or hide elements (cookie banners, ads, fixed headers)
// before a screenshot. Returns 1 on success, 0 on failure. Applies to the
// current document; re-apply after a navigation.
MB_EXPORT int mbInsertCSS(mbView*, const char* css);

// Synthesize a left mouse click (down+up) at (x,y) in the view.
MB_EXPORT void mbSendMouseClick(mbView*, int x, int y);
// General click at (x,y): `button` 0=left/1=middle/2=right; `modifiers` is a bitmask
// 1=ctrl 2=shift 4=alt 8=meta — so the page sees e.button + e.ctrlKey/shiftKey/altKey/
// metaKey. Left fires `click`; middle/right fire `auxclick` (right also `contextmenu`).
MB_EXPORT void mbSendMouseClickEx(mbView*, int x, int y, int button, int modifiers);

// Press / release the left mouse button at (x,y) as separate events. Down ->
// mbSendMouseMove(...) -> ... -> Up performs a DRAG (the moves in between carry
// the held button, so e.buttons==1), which mbSendMouseClick can't express; a Down
// and Up at the same point equals a click. Pair with mbGetElementRect to drag by
// selector (slider thumbs, canvas drawing, drag-and-drop reordering).
MB_EXPORT void mbSendMouseDown(mbView*, int x, int y);
MB_EXPORT void mbSendMouseUp(mbView*, int x, int y);

// Single-finger touch tap (touchstart+touchend) at (x,y) — fires touch-only
// handlers (mobile nav menus, tap targets) that mouse events don't, with a
// populated touches[0].clientX/Y. Touch events are enabled in the engine.
MB_EXPORT void mbSendTouchTap(mbView*, int x, int y);

// One-finger touch swipe from (x1,y1) to (x2,y2): touchstart + interpolated
// touchmoves + touchend — touch scroll / swipe gestures (carousels, pull-to-
// refresh, mobile drawers). Like a mouse drag but for touch handlers.
MB_EXPORT void mbSendTouchSwipe(mbView*, int x1, int y1, int x2, int y2);

// Click the center of the first element matching `css_selector` (Puppeteer-style
// page.click). Resolves the element's box in the page then clicks its center.
// Returns 1 on success, 0 if nothing matches or the element has no box
// (display:none / zero-size). Pair with mbWaitForSelector for dynamic content.
MB_EXPORT int mbClickSelector(mbView*, const char* css_selector);

// Mouse-drag the center of the first `from_selector` match to the center of the
// first `to_selector` match (Puppeteer dragAndDrop): press, glide through
// interpolated moves (carrying the held button), release. Drives mouse-based drag
// widgets (sliders, sortable lists, map panning); does NOT trigger HTML5 native
// drag-and-drop. Both elements must be in the viewport. Returns 1 if both matched.
MB_EXPORT int mbDragSelector(mbView*, const char* from_selector,
                             const char* to_selector);

// Dispatch a synthetic bubbling, cancelable DOM event of `type` (e.g. "mouseover",
// "focus", "submit", or a custom event name) on the first element matching
// `css_selector` — trigger handlers that mbClickSelector / mbFillSelector don't.
// Returns 1 if an element matched, 0 if not.
MB_EXPORT int mbDispatchEvent(mbView*, const char* css_selector, const char* type);

// Move the pointer onto the first element matching `css_selector` (its center),
// generating mousemove + mouseover/mouseenter and applying :hover — to open
// hover menus, reveal tooltips, etc. Returns 1 on success, 0 if nothing matches
// or the element has no box.
MB_EXPORT int mbHoverSelector(mbView*, const char* css_selector);

// Scroll the first element matching `css_selector` to the viewport center
// (Element.scrollIntoView). Returns 1 on success, 0 if nothing matches. The
// click/double-click/right-click/hover selector ops already do this internally,
// so a below-the-fold target is clickable; call this directly to trigger lazy
// loading or to frame an element before a screenshot.
MB_EXPORT int mbScrollIntoView(mbView*, const char* css_selector);

// Double-click the first element matching `css_selector` (its center), firing a
// dblclick — for text selection, expand/collapse, inline edit. Returns 1 on
// success, 0 if nothing matches or the element has no box.
MB_EXPORT int mbDoubleClickSelector(mbView*, const char* css_selector);

// Right-click the first element matching `css_selector` (its center), firing a
// contextmenu event — for right-click menus. Returns 1 on success, 0 otherwise.
MB_EXPORT int mbRightClickSelector(mbView*, const char* css_selector);

// Focus / blur the first element matching `css_selector` (HTMLElement.focus() /
// .blur()), firing focus/focusin or blur/focusout. Blur is commonly what
// triggers form-field validation. Returns 1 on success, 0 if nothing matches.
MB_EXPORT int mbFocusSelector(mbView*, const char* css_selector);
MB_EXPORT int mbBlurSelector(mbView*, const char* css_selector);

// Write the full scrollable document size (logical px; >= the viewport) into
// *w/*h (either may be NULL). For a full-page screenshot: mbGetContentSize ->
// mbResize(view, w, h) -> mbPaintToBitmap. Returns 1 on success.
MB_EXPORT int mbGetContentSize(mbView*, int* w, int* h);

// Write the current viewport size in logical (CSS) px into *w/*h (either may be
// NULL) — window.innerWidth/Height, the read-back peer of mbResize/mbCreateView
// (DPR-independent). Distinct from mbGetContentSize (the full scrollable doc).
// Returns 1 on success. (Needs a committed document.)
MB_EXPORT int mbGetViewSize(mbView*, int* w, int* h);

// Fill the first <input>/<textarea> matching `css_selector` with `utf8_text`
// (Playwright-style fill): focuses it, sets the value via the native setter so
// frameworks like React observe it, and fires input+change. Returns 1 on
// success, 0 if nothing matches. Pair with mbWaitForSelector for dynamic forms.
MB_EXPORT int mbFillSelector(mbView*, const char* css_selector, const char* utf8_text);

// Set an <input type=file>'s selected files from disk PATHS (newline-separated for a
// `multiple` input) — the privileged op a page's own script is forbidden to do, enabling
// file-upload automation. The bytes are read into an in-memory blob (so .size, FileReader
// and form submit work) and the change event fires. Returns 1 on success, 0 if
// `css_selector` doesn't match a file input or no valid path was given.
MB_EXPORT int mbSetFileForSelector(mbView*, const char* css_selector,
                                   const char* paths_newline);

// Handle JavaScript dialogs (alert/confirm/prompt) instead of the headless default
// (alert no-op, confirm=false, prompt=null). The callback is invoked synchronously for
// each dialog: `type` is 0=alert / 1=confirm / 2=prompt, `message` the dialog text,
// `default_value` the prompt default. Return 1 to ACCEPT (confirm OK / prompt entered)
// or 0 to DISMISS (confirm Cancel / prompt null); for an accepted prompt, write the
// entered text into out_value (capacity out_cap). Lets automation capture dialog text
// and drive confirm/prompt. Pass NULL to restore the defaults. Per view.
typedef int (*mbJsDialogCallback)(int type, const char* message,
                                  const char* default_value, char* out_value,
                                  int out_cap, void* userdata);
MB_EXPORT void mbSetJsDialogCallback(mbView*, mbJsDialogCallback cb, void* userdata);

// Select the option of the <select> matching `css_selector` whose value OR
// visible text equals `value`, firing input+change (Puppeteer page.select).
// Returns 1 on success, 0 if no <select> or no matching option.
MB_EXPORT int mbSelectOption(mbView*, const char* css_selector, const char* value);

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

// Read the effective User-Agent (the override if set, else the built-in default)
// into `out` (NUL-terminated, truncated to out_cap). Returns the full length.
MB_EXPORT int mbGetUserAgent(mbView*, char* out, int out_cap);

// Route all network fetches through a proxy (process-wide; no view param). `proxy`
// is a libcurl proxy string: "http://host:port", "socks5://host:port", or
// "host:port" (http assumed). NULL or "" forces a direct connection (overriding
// *_proxy env vars); never calling this honors libcurl's env-var defaults. Affects
// http(s) only — file:// and data: are served directly. Set before navigating.
MB_EXPORT void mbSetProxy(const char* proxy);

// Skip TLS certificate verification for all network fetches when `ignore` != 0
// (the equivalent of curl -k / Puppeteer ignoreHTTPSErrors); 0 restores the
// secure default. Process-wide (no view param). For scraping/testing sites with
// self-signed, expired, or otherwise invalid certificates.
MB_EXPORT void mbSetIgnoreCertErrors(int ignore);

// Follow HTTP 3xx redirects (the default) when `follow` != 0, or stop at the
// redirect response when 0 — so after mbLoadURL, mbGetHttpStatus returns the 30x
// code and mbGetResponseHeaders exposes the Location header, letting a caller
// resolve a URL shortener or inspect a redirect chain without following it.
// Process-wide (no view param). Set before navigating.
MB_EXPORT void mbSetFollowRedirects(int follow);

// Capture with a transparent background (1) or opaque white (0, default). With
// transparency on, areas the page does not paint keep alpha 0 in the output.
MB_EXPORT void mbSetTransparentBackground(mbView*, int transparent);

// Enable (1, default) or disable (0) automatic image loading. Disabling speeds up
// text/HTML scraping. Call before mbLoadURL/mbLoadHTML.
MB_EXPORT void mbSetLoadImages(mbView*, int enabled);

// Emulate the prefers-color-scheme media feature: dark (1) or light (0, default),
// so pages render their dark theme. Call before mbLoadURL/mbLoadHTML.
MB_EXPORT void mbSetDarkMode(mbView*, int dark);

// Set the view's window-focus state: focused (1, the default) or blurred (0).
// Clearing it makes document.hasFocus() false and blurs the active element, as
// if the window lost focus; setting it restores focus. For simulating focus/blur.
MB_EXPORT void mbSetFocus(mbView*, int focused);

// Bind a native C function callable from JS as window[name](...). JS arguments
// are coerced to UTF-8 strings (argc/argv); argtypes[i] reports each arg's JS
// type (0=string,1=number,2=boolean,3=null,4=undefined,5=object,6=array,
// 7=function). The function returns a UTF-8 string and may set *out_type to
// choose the JS return type (0=string default, 1=number, 2=boolean, 3=null,
// 4=undefined, 5=json — the string is JSON.parse'd into an object/array/value).
// Synchronous — JS receives the return value inline. The binding is installed
// into each new document's main world (call before navigating). `userdata` is
// passed back to every invocation.
typedef const char* (*mbJsNativeFn)(void* userdata, int argc, const char** argv,
                                    const int* argtypes, int* out_type);
MB_EXPORT void mbJsBindFunction(mbView*, const char* name, mbJsNativeFn fn,
                                void* userdata);

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

// Press a named non-text key as a real (trusted) key event, so default actions
// fire: "Enter" (submit a form / activate), "Tab" (move focus), "Escape",
// "Backspace", "Delete", "ArrowLeft/Up/Right/Down", "Home", "End", "PageUp",
// "PageDown". Unlike a JS-dispatched event, this triggers the browser default
// action. No-op for an unknown key name.
MB_EXPORT void mbSendKey(mbView*, const char* key_name);

// Dispatch a standalone key RELEASE (a `keyup` event) for a Win32 virtual-key code,
// so page handlers that watch for key release fire. Use when driving key down/up
// separately rather than as one press (mbSendKey already includes the release).
MB_EXPORT void mbSendKeyUp(mbView*, int windows_key_code);

// Synthesize a gesture scroll at (x,y) by (dx,dy) pixels. Positive dy scrolls
// the page downward (toward larger window.scrollY), matching natural intent.
MB_EXPORT void mbSendScroll(mbView*, int x, int y, int dx, int dy);

// Scroll the layout viewport to an ABSOLUTE offset (x, y) in CSS px
// (window.scrollTo). Unlike mbSendScroll's relative gesture or a full-page
// resize, this captures the real viewport at a position — fixed/sticky elements
// render correctly. Pair with mbPaintToBitmap / mbSavePng for a viewport shot.
MB_EXPORT void mbScrollTo(mbView*, int x, int y);

// Auto-scroll to the bottom, settling between steps so IntersectionObserver /
// lazy-load handlers append their content, until the page stops growing or
// `max_steps` is reached (<=0 -> default 20). Use before mbSavePng(--full) or a
// scrape so deferred images / infinite-scroll items materialize. Returns the
// number of steps that grew the page (0 = a static page).
MB_EXPORT int mbScrollToBottom(mbView*, int max_steps);

// Write the HTTP cookie jar's cookies for `url` ("name=value; name2=value2") into
// `out` (NUL-terminated, up to out_cap). For exporting a session after a login flow.
// Returns the full length in bytes; empty for non-http(s) URLs.
MB_EXPORT int mbGetCookies(mbView*, const char* url, char* out, int out_cap);

// Write the value of a single cookie `name` for `url` into `out` (vs mbGetCookies'
// whole jar) — e.g. read the session/auth cookie to check login state without
// parsing. Returns the value length (>=0), or -1 if `name` isn't set for `url`.
MB_EXPORT int mbGetCookie(mbView*, const char* url, const char* name, char* out,
                          int out_cap);

// Write the WHOLE cookie jar (every host, session + persistent) into `out` as a
// Netscape cookie file (NUL-terminated, up to out_cap; size first with
// out=NULL/out_cap=0). Returns the full length. For exporting an entire session
// in memory (no temp file) — the in-memory peer of mbSaveCookies.
MB_EXPORT int mbGetAllCookies(mbView*, char* out, int out_cap);

// Inject a cookie ("name=value[; Path=/; Domain=...; Secure]") into the HTTP jar
// for `url`'s origin — the inverse of mbGetCookies, for restoring a saved login
// session before navigating. No-op for non-http(s) URLs.
MB_EXPORT void mbSetCookie(mbView*, const char* url, const char* cookie);

// Erase all cookies from the HTTP jar (reset the session).
MB_EXPORT void mbClearCookies(mbView*);

// Persist the WHOLE cookie jar (all hosts, session + persistent) to a Netscape
// cookie file and load it back — for session reuse across process runs (log in
// once, reload the jar next run). Process-wide (the jar is shared; no view param).
// Return 1 on success, 0 on failure (unwritable path / missing file). The file is
// curl's native --cookie-jar format, so it interoperates with curl/wget.
MB_EXPORT int mbSaveCookies(const char* path);
MB_EXPORT int mbLoadCookies(const char* path);

// Network observability: the loader records every subresource URL it fetches
// (img, css, fetch/XHR, …). mbGetRequestLog writes them newline-separated,
// oldest first, into `out` and returns the full length (size first with
// out=NULL/out_cap=0); mbClearRequestLog empties the log (call before a load to
// scope it to that page). Process-wide and capped (oldest dropped past the cap).
MB_EXPORT int mbGetRequestLog(char* out, int out_cap);
MB_EXPORT void mbClearRequestLog(void);

// Network control: block any fetched URL containing `substring` (failed with
// ERR_BLOCKED_BY_CLIENT instead of loaded) — suppress ads / trackers / images /
// analytics for faster, cleaner scrapes and screenshots. mbBlockUrl registers a
// substring (call repeatedly for several); mbClearUrlBlocks removes all. Process-
// wide, applied at the loader. Set before navigating to affect that page's loads.
MB_EXPORT void mbBlockUrl(const char* substring);
MB_EXPORT void mbClearUrlBlocks(void);

// Dynamic per-request hook: a process-wide callback invoked for EVERY request URL
// the loader handles (alongside the static block/mock/rewrite tables), so you can
// inspect and decide at runtime instead of pre-registering substrings. Return nonzero
// to BLOCK the request (failed with ERR_BLOCKED_BY_CLIENT), zero to allow. The
// callback runs on the main thread during the load. Pass NULL to clear.
typedef int (*mbRequestCallback)(const char* url, void* userdata);
MB_EXPORT void mbSetRequestCallback(mbRequestCallback cb, void* userdata);

// Response hook: a process-wide callback invoked after a successful load (fetch / mock /
// file / data) with an opaque mbResponse handle, BEFORE the body reaches the page — so
// you can inspect or REPLACE the response bytes (inject a script, strip content, rewrite
// a JSON payload). The handle is valid only for the duration of the callback; query it
// with mbResponseURL/Status/Body and replace the body with mbResponseSetBody (which
// updates the delivered length). Runs on the main thread inside the load. NULL clears it.
typedef struct mbResponse mbResponse;
typedef void (*mbResponseCallback)(mbResponse*, void* userdata);
MB_EXPORT void mbSetResponseCallback(mbResponseCallback cb, void* userdata);
MB_EXPORT const char* mbResponseURL(mbResponse*);
MB_EXPORT int mbResponseStatus(mbResponse*);
// Returns the body bytes (NOT NUL-terminated; may contain NULs). *out_len gets the length.
MB_EXPORT const char* mbResponseBody(mbResponse*, int* out_len);
MB_EXPORT void mbResponseSetBody(mbResponse*, const char* body, int len);

// Response mocking — the signature interception feature. Any request whose URL
// CONTAINS `url_substring` is served `body` WITHOUT a real network fetch: run a
// page fully offline, or substitute an API/XHR/fetch response in tests/automation.
// `content_type` defaults to "text/html" when NULL/empty; `status` defaults to 200
// when <= 0. Register several (last matching wins on overlap); mbClearMocks removes
// all. Process-wide, applied at the loader, before the blocklist's blocked URLs.
// Set before navigating to affect that page's requests.
MB_EXPORT void mbMockResponse(const char* url_substring, const char* body,
                              const char* content_type, int status);
MB_EXPORT void mbClearMocks(void);

// Request URL rewriting — the request-side counterpart to mocking. Before any
// fetch, the first occurrence of `from` in a request URL is replaced with `to`
// (host swap, http->https, point a CDN/API at a local mock). The rewrite is
// transparent: the page still sees its ORIGINAL URL as the response URL (so
// fetch()/XHR behave correctly). Register several (applied in order);
// mbClearUrlRewrites removes all. Process-wide, applied at the loader.
MB_EXPORT void mbRewriteUrl(const char* from, const char* to);
MB_EXPORT void mbClearUrlRewrites(void);

// Per-URL request header injection: add/override the outgoing http(s) header `name:
// value` for any request whose URL CONTAINS `url_substring` — e.g. send an Authorization
// or API-key header only to its own host (not leaked to every origin the page touches),
// or a per-domain User-Agent. Conditional on the URL, unlike the global mbSetExtraHeaders.
// Register several; mbClearRequestHeaders removes all. Process-wide, applied at the loader.
MB_EXPORT void mbSetRequestHeader(const char* url_substring, const char* name,
                                  const char* value);
MB_EXPORT void mbClearRequestHeaders(void);

// localStorage access for the current document's origin — inject an auth token /
// app state before an SPA boots (pair with mbSetInitScript), or read it back.
// mbGetLocalStorage writes the value of `key` into `out` and returns its length
// (>=0), or -1 if the key is absent OR storage is unavailable (opaque origin like
// about:blank — commit with an http(s) base URL). mbSetLocalStorage stores
// key=value and returns 1 on success, 0 on failure. Origin-scoped, like cookies.
MB_EXPORT int mbGetLocalStorage(mbView*, const char* key, char* out, int out_cap);
MB_EXPORT int mbSetLocalStorage(mbView*, const char* key, const char* value);

// sessionStorage peer of the above — identical contract, but the store is
// per-session (not persisted across runs). Same origin requirement.
MB_EXPORT int mbGetSessionStorage(mbView*, const char* key, char* out, int out_cap);
MB_EXPORT int mbSetSessionStorage(mbView*, const char* key, const char* value);

// Empty both Web Storage areas (localStorage + sessionStorage) for the current
// document's origin — reset app state between scrapes, or simulate a logout.
// Best-effort. The cookie-jar peer is mbClearCookies; together they reset a
// login session.
MB_EXPORT void mbClearStorage(mbView*);

// Geolocation override (process-wide): give navigator.geolocation a fixed position so
// location-aware pages work headlessly. After mbSetGeolocation, getCurrentPosition /
// watchPosition resolve to (latitude, longitude) with `accuracy` metres (and the
// permission is granted); mbClearGeolocation reverts to the default (denied -> the page
// gets a PERMISSION_DENIED error). Set before the page queries position.
MB_EXPORT void mbSetGeolocation(double latitude, double longitude, double accuracy);
MB_EXPORT void mbClearGeolocation(void);

// Persist localStorage across process runs: mbSaveLocalStorage snapshots the WHOLE
// localStorage for the current document's origin into `out` as a JSON object string
// (NUL-terminated; size first with out=NULL), returning its length. mbLoadLocalStorage
// restores such a snapshot (merging into the current store). Save to disk after a login,
// reload it next run to resume the session — the localStorage peer of mbSaveCookies.
// Needs a real (http/https) origin, like the other storage calls.
MB_EXPORT int mbSaveLocalStorage(mbView*, char* out, int out_cap);
MB_EXPORT void mbLoadLocalStorage(mbView*, const char* json);

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

// Write the visible text (innerText) of the first element matching `css_selector`
// into `out`. Returns the value's length in bytes (>=0), or -1 if no element
// matches. Size first with out=NULL/out_cap=0. Companion to mbGetElementRect:
// scrape one element's text without writing JS.
MB_EXPORT int mbGetTextForSelector(mbView*, const char* css_selector, char* out,
                                   int out_cap);

// Write the innerText of EVERY element matching `css_selector` as a JSON array
// string (e.g. ["row 1","row 2"]) into `out` — one call for list scraping
// instead of mbCountSelector + an :nth-of-type loop. JSON keeps embedded commas/
// newlines/quotes intact. Returns the length in bytes (>=0; "[]" when nothing
// matches), or -1 on an invalid selector. Size first with out=NULL/out_cap=0.
MB_EXPORT int mbGetAllTextForSelector(mbView*, const char* css_selector,
                                      char* out, int out_cap);

// Write the outerHTML of the first element matching `css_selector` (the element
// plus its markup) into `out` — extract a fragment (article body, table, card) to
// re-parse, vs mbGetTextForSelector (plain text) or mbGetHTML (whole document).
// Returns the length in bytes (>=0) or -1 if no element matches. Size first with
// out=NULL/out_cap=0.
MB_EXPORT int mbGetHtmlForSelector(mbView*, const char* css_selector, char* out,
                                   int out_cap);

// Set the innerHTML of the first element matching `css_selector` (replace its
// contents) — template or redact a fragment before a capture. The write side of
// mbGetHtmlForSelector. Returns 1 if an element matched, 0 if not.
MB_EXPORT int mbSetHtmlForSelector(mbView*, const char* css_selector,
                                   const char* html);

// Write attribute `attr` of EVERY element matching `css_selector` as a JSON array
// string (e.g. ["/a","/b"] for all link hrefs) into `out` — list scraping of an
// attribute in one call. An element missing the attribute contributes JSON null
// (keeping index alignment with mbGetAllTextForSelector). Raw attribute value,
// not the resolved property. Returns the length (>=0; "[]" for no matches) or -1
// on an invalid selector. Size first with out=NULL/out_cap=0.
MB_EXPORT int mbGetAllAttributeForSelector(mbView*, const char* css_selector,
                                           const char* attr, char* out,
                                           int out_cap);

// Write the live .value of EVERY element matching `css_selector` as a JSON array
// string (e.g. ["alice","",  "on"]) into `out` — serialize a whole form's current
// state in one call, vs mbGetAllAttributeForSelector(...,"value") which gives the
// static initial attribute. A match with no value property contributes JSON null.
// Returns the length (>=0; "[]" for no matches) or -1 on an invalid selector.
MB_EXPORT int mbGetAllValueForSelector(mbView*, const char* css_selector,
                                       char* out, int out_cap);

// Write the value of attribute `attr` on the first element matching
// `css_selector` into `out`. Returns the value's length in bytes (>=0), or -1 if
// no element matches OR the attribute is absent (null). Size first with
// out=NULL/out_cap=0.
MB_EXPORT int mbGetAttribute(mbView*, const char* css_selector, const char* attr,
                             char* out, int out_cap);

// setAttribute(attr, value) on the first element matching `css_selector` (pass
// value="" for a bare boolean attribute like "disabled"). Returns 1 if an element
// matched, 0 if not. Sets the static HTML attribute — to set a control's live
// .value use mbFillSelector.
MB_EXPORT int mbSetAttribute(mbView*, const char* css_selector, const char* attr,
                             const char* value);

// Write the LIVE .value of the first element matching `css_selector` (what an
// <input>/<textarea>/<select> currently holds after typing or selection) into
// `out`. Distinct from mbGetAttribute, which reads the static "value" HTML
// attribute (the initial value). Returns the length in bytes (>=0), or -1 if no
// element matches or it has no value property. Size first with out=NULL/out_cap=0.
MB_EXPORT int mbGetValueForSelector(mbView*, const char* css_selector, char* out,
                                    int out_cap);

// The .checked state of the first element matching `css_selector`: 1 if checked,
// 0 if unchecked, -1 if no element matches or it isn't a checkable control
// (checkbox/radio). Pairs with mbClickSelector, which toggles a checkbox.
MB_EXPORT int mbGetCheckedForSelector(mbView*, const char* css_selector);

// Whether the first element matching `css_selector` is actually visible: 1 yes,
// 0 hidden (display:none, visibility:hidden, content-visibility, or opacity:0),
// -1 if no element matches. Existence != visibility — use after a toggle or a
// CSS transition, or to confirm an element mbWaitForSelector found is shown.
MB_EXPORT int mbIsVisibleForSelector(mbView*, const char* css_selector);

// Number of elements matching `css_selector` (querySelectorAll length). Returns
// the count (>=0; 0 is valid) or -1 for a null/invalid selector. Use with
// :nth-of-type(n)/:nth-child(n) selectors on mbGetTextForSelector/mbGetAttribute
// to scrape a list: count first, then read each index. Pairs with
// mbWaitForSelector to wait for "at least one" and mbWaitForFunction for "N".
MB_EXPORT int mbCountSelector(mbView*, const char* css_selector);

// Write the computed value of CSS `property` for the first element matching
// `css_selector` into `out` (getComputedStyle -> getPropertyValue, so values are
// resolved: color -> "rgb(r, g, b)", bold -> "700", display:none -> "none").
// Returns the value's length in bytes (>=0), or -1 if no element matches. Size
// first with out=NULL/out_cap=0. Use for visibility checks (display/visibility/
// opacity) and style assertions without writing JS.
MB_EXPORT int mbGetComputedStyle(mbView*, const char* css_selector,
                                 const char* property, char* out, int out_cap);

// Re-navigate to the current document's URL, re-fetching it. No-op for in-memory
// documents (about:blank, data:, mbLoadHTML content).
MB_EXPORT void mbReload(mbView*);

// HTTP status code of the last top-level http(s) navigation (e.g. 200, 404, 500),
// or 0 if the last load was non-http (file://, data:, mbLoadHTML) or the network
// request failed before a response. Lets a caller distinguish a successful page
// from a 404/error page after mbLoadURL — more reliable than guessing from the
// body length.
MB_EXPORT int mbGetHttpStatus(mbView*);

// Write the raw response headers of the last top-level http(s) navigation into
// `out` (CRLF-separated "Name: Value" lines as the server sent them, including
// the status line). Returns the full length in bytes (size first with
// out=NULL/out_cap=0); 0 for a non-http load or a failed fetch. Lets a caller
// read Content-Type, Content-Length, caching, or custom/API headers.
MB_EXPORT int mbGetResponseHeaders(mbView*, char* out, int out_cap);

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

// Sub-frame (iframe) targeting. mbGetFrameCount returns the number of direct child
// frames (top-level iframes) of the main document. mbEvalJSInFrame evals in the
// frame_index-th child frame's context (0-based, document order; -1 = the main
// frame) — host-privileged, so it reads even a CROSS-ORIGIN iframe whose content
// the parent's `iframe.contentDocument` can't (same-origin policy). Same out-buffer
// contract as mbEvalJS. Returns "" for an out-of-range index.
MB_EXPORT int mbGetFrameCount(mbView*);
MB_EXPORT int mbEvalJSInFrame(mbView*, int frame_index, const char* utf8_script,
                              char* out, int out_cap);

// Like mbEvalJS, but ALSO reports the JS typeof the result via `out_type` (one of
// "number"/"string"/"boolean"/"object"/"function"/"undefined"/"array"/"null"),
// captured from the SAME single evaluation (no re-run, so side effects fire once).
// Returns the value length in bytes. Used to back wke's jsTypeOf.
MB_EXPORT int mbEvalJSEx(mbView*, const char* utf8_script, char* out_value,
                         int value_cap, char* out_type, int type_cap);

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

// Render the current frame to a width×height PNG held in memory (no temp file) —
// for embedders that serve the bytes (over HTTP, into a DB, etc.). On success
// sets *out_data to the encoded PNG bytes and returns the length; returns 0 on
// failure. The bytes are owned by the view and remain valid only until the next
// mbEncodePng on this view or mbDestroyView — copy them out before either.
MB_EXPORT int mbEncodePng(mbView*, int width, int height,
                          const unsigned char** out_data);

// Print the document to a multi-page PDF (US Letter) at `path`. Returns 1 on success.
MB_EXPORT int mbSavePdf(mbView*, const char* path);
// Like mbSavePdf with an explicit page geometry. `width_pt`/`height_pt` are the page size
// in POINTS (72/in) — e.g. Letter 612x792, A4 595x842; <=0 falls back to Letter. `landscape`
// (nonzero) swaps width/height. `scale` is the content scale (1.0 = 100%, clamped 0.1–5;
// <=0 -> 1.0). `margin_pt` is a uniform margin in points (0 = none). Returns 1 on success.
MB_EXPORT int mbSavePdfEx(mbView*, const char* path, double width_pt, double height_pt,
                          int landscape, double scale, double margin_pt);

// Render just the logical rect (x,y,w,h) of the page to a PNG (e.g. an element
// screenshot). The output image is (w*dsf x h*dsf) px. Returns 1 on success.
MB_EXPORT int mbSavePngRect(mbView*, const char* path, int x, int y, int w, int h);

// Screenshot just the first element matching `css_selector` to `path` (PNG/JPEG
// by extension) — Puppeteer's elementHandle.screenshot. Scrolls the element into
// view and clips its bounding box (no view resize). Returns 1 on success, 0 if no
// element matches or it has no box (display:none/zero-size). An element larger
// than the viewport is captured to its visible extent.
MB_EXPORT int mbSaveElementPng(mbView*, const char* css_selector, const char* path);

// Composite the logical rect (x,y,w,h) into a caller-provided BGRA8888 buffer
// (w x h px, `stride` bytes/row; the device scale factor is not applied here).
// Write the first element matching `css_selector`'s viewport-relative bounding
// box (logical px) into *x/*y/*w/*h (any may be NULL). Returns 1 if matched, else
// 0. Compose with mbPaintRectToBitmap for an element screenshot, or with
// mbSendMouseClick for a precise click.
MB_EXPORT int mbGetElementRect(mbView*, const char* css_selector, int* x, int* y,
                               int* w, int* h);

MB_EXPORT int mbPaintRectToBitmap(mbView*, void* out_bgra, int x, int y, int w,
                                  int h, int stride);

#if defined(__cplusplus)
}  // extern "C"
#endif

#endif  // MINIBLINK_HOST_CAPI_MB_CAPI_H_
