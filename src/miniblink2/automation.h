// automation.h — the AUTOMATION/TESTING surface of the miniblink2 C API
// (mb* ABI): pump-driving waits, selector driving and scraping, screenshots/
// PDF, device emulation, storage snapshots, the request log. Several calls
// here PUMP the engine (mbWait, mbWaitFor*) — do not call them from an
// interactive host's frame tick; they are meant for headless drivers.
// Includes the embedder core (webview.h); there is no umbrella header —
// embedders include webview.h, drivers include automation.h.

#ifndef MINIBLINK2_AUTOMATION_H_
#define MINIBLINK2_AUTOMATION_H_

#include "webview.h"

#if defined(__cplusplus)
extern "C" {
#endif

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

// Robust networkidle0 wait: wait until THIS view has had nothing in flight and
// no newly-started request for `idle_ms`. Covers main-frame, child-frame, and
// subresource loads plus dedicated- and shared-worker traffic. Unrelated
// traffic in another view cannot disturb it; a shared-worker request counts
// for every view that was connected to the worker when that request STARTED (a
// client joining mid-request is counted from its next request). A slow request
// remains busy until completion. Returns 1 once quiet, 0 if `timeout_ms`
// elapses. `idle_ms <= 0` defaults to 500 ms; `timeout_ms <= 0` makes the
// deadline immediate, so the call returns 0. No request-log clearing is needed.
MB_EXPORT int mbWaitForNetworkIdle(mbView*, int idle_ms, int timeout_ms);

// Runaway-script guard. A single-process embedder shares the main thread with the
// page, so a synchronous infinite loop in page JS (e.g. `while(true){}`) would hang
// the process forever. Set the max wall-clock (ms) a single main-thread task may run
// before its JS is forcibly terminated; the embedder recovers and can load the next
// page. `ms <= 0` disables (the DEFAULT). Process-global; set once after mbInitialize.
// Only a single never-returning task is killed — slow async work (network waits across
// many short tasks) is never affected. Recommended 5000–10000 for untrusted pages.
MB_EXPORT void mbSetScriptTimeout(int ms);

// Download `url` to `dest_path`: fetch through the engine network stack and write the
// body to disk WITHOUT rendering it as a document. Honors the interception layer
// (mbRewriteUrl / mbBlockUrl / mbMockResponse / mbSetRequestCallback / mbSetResponseCallback)
// and, for http(s), the view's user-agent, extra + per-URL headers, cookies and proxy.
// Works for http(s), file:// and data: URLs. Returns 1 on success, 0 on fetch/write failure.
MB_EXPORT int mbDownloadURL(mbView*, const char* url, const char* dest_path);

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

// HTML5 NATIVE drag-and-drop (the peer of mbDragSelector's mouse drag): fire the
// DragEvent sequence dragstart -> dragenter -> dragover -> drop -> dragend on the
// matched elements, sharing one DataTransfer so a handler's setData()/getData()
// round-trips. Drives drag-to-upload, sortable lists, kanban boards and other
// widgets that listen on drag*/drop events (not mouse moves). Returns 1 if both
// selectors matched. (Events are isTrusted=false — app handlers fire; a few
// trusted-gesture-only behaviors won't.)
MB_EXPORT int mbDragDropSelector(mbView*, const char* from_selector,
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

// Select the option of the <select> matching `css_selector` whose value OR
// visible text equals `value`, firing input+change (Puppeteer page.select).
// Returns 1 on success, 0 if no <select> or no matching option.
MB_EXPORT int mbSelectOption(mbView*, const char* css_selector, const char* value);

// Device / mobile emulation for responsive rendering + screenshots (no compositor
// needed). `mobile` != 0 makes the page render as a touch device: matchMedia('(pointer:
// coarse)') and '(hover: none)' match, the <meta viewport> + mobile viewport are honored;
// mobile == 0 reverts to a desktop device (fine pointer + hover). `width`/`height` (when
// > 0) resize the view, and `deviceScaleFactor` (when > 0) sets window.devicePixelRatio.
// Unlike the DevTools EnableDeviceEmulation path, this drives only the layout-visible
// settings, so it works in the non-compositing headless widget.
MB_EXPORT void mbEmulateDevice(mbView*, int width, int height,
                               float deviceScaleFactor, int mobile);

// Override any CSS media feature so matchMedia() and @media rules evaluate to the
// requested value LIVE (re-runs the page's media queries). Examples:
//   mbEmulateMedia(v, "prefers-reduced-motion", "reduce");
//   mbEmulateMedia(v, "prefers-contrast", "more");
//   mbEmulateMedia(v, "forced-colors", "active");
// Pass an empty/NULL `value` to clear that feature; an empty/NULL `feature` clears
// all overrides. (A general form of mbSetDarkMode for accessibility/theme testing.)
MB_EXPORT void mbEmulateMedia(mbView*, const char* feature, const char* value);

// Override the CSS media TYPE — the DevTools `media` knob, distinct from the media
// FEATURES above. "print" makes @media print rules and matchMedia('print') apply
// while the page is still rendered to the screen, so a screenshot (mbSavePng) or
// PDF reflects the page's PRINT stylesheet; "screen" forces screen; ""/NULL clears
// the override (natural screen media). Re-runs the page's media queries live.
MB_EXPORT void mbEmulateMediaType(mbView*, const char* media_type);

// Set the page's visibility: visible (1, the default) or hidden (0). Drives
// document.visibilityState / document.hidden and fires the visibilitychange
// event, so a page pauses timers/video/polling when the embedder backgrounds it.
MB_EXPORT void mbSetVisibility(mbView*, int visible);

// Override the timezone for Date and Intl (an IANA id, e.g. "America/New_York"),
// so time-dependent UIs render deterministically. SCOPE: PROCESS-GLOBAL, despite the
// mbView* argument — it sets ICU's default timezone (and redetects it in the shared V8
// isolate), which every view shares, so the last call wins for ALL views. The mbView*
// selects nothing; it is kept only for call-site symmetry with the other per-view
// automation setters. Pass any live view. There is no per-view timezone (one process,
// one ICU default).
MB_EXPORT void mbSetTimezone(mbView*, const char* iana_tz);

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

// Default-profile cookie persistence: save the implicit default session's
// whole jar (all hosts, including session and persistent cookies) to a Netscape
// cookie file, or merge such a file back into that jar. Custom mbSession profiles
// are intentionally not included; persistent custom profiles use mbSessionFlush,
// while mbGetAllCookies(view) can export any profile in memory. Return 1 on success,
// 0 on failure (unwritable path / missing file). The file is curl's native
// --cookie-jar format, so it interoperates with curl/wget.
MB_EXPORT int mbSaveCookies(const char* path);
MB_EXPORT int mbLoadCookies(const char* path);

// Persist / restore the WHOLE in-memory IndexedDB store (every database, by name) to/from a
// private binary file — the IndexedDB peer of mbSaveCookies/mbSaveLocalStorage, for carrying
// app state (auth tokens, offline caches) across process runs. Process-wide (no view param).
// Return 1 on success, 0 on failure. Call mbLoadIndexedDB BEFORE the page opens its databases.
// Blob/File-valued records ARE captured (their bytes + metadata round-trip).
MB_EXPORT int mbSaveIndexedDB(const char* path);
MB_EXPORT int mbLoadIndexedDB(const char* path);

// Persist / restore the WHOLE in-memory OPFS tree (navigator.storage.getDirectory() — every
// origin/bucket scope, directories + file bytes) to/from a private binary file. The peer of
// mbSaveIndexedDB for the modern file-storage API. Process-wide (no view param). mbLoadOPFS
// MERGES onto the live tree (existing handles stay valid), so it can be called any time.
// Return 1 on success, 0 on failure.
MB_EXPORT int mbSaveOPFS(const char* path);
MB_EXPORT int mbLoadOPFS(const char* path);

// Network observability: the loader records every subresource URL it fetches
// (img, css, fetch/XHR, …). mbGetRequestLog writes them newline-separated,
// oldest first, into `out` and returns the full length (size first with
// out=NULL/out_cap=0); mbClearRequestLog empties the log (call before a load to
// scope it to that page). Process-wide and capped (oldest dropped past the cap).
MB_EXPORT int mbGetRequestLog(char* out, int out_cap);
MB_EXPORT void mbClearRequestLog(void);

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

// Network connectivity (process-wide): set navigator.onLine and fire the window
// online/offline events. online=0 simulates going offline (pages can show an
// offline banner, pause sync, etc.); online=1 (the default) restores connectivity.
// Affects every view. Note: does not block actual fetches — it only drives the
// JS-visible online state.
MB_EXPORT void mbSetOnline(int online);

// Persist localStorage across process runs: mbSaveLocalStorage snapshots the WHOLE
// localStorage for the current document's origin into `out` as a JSON object string
// (NUL-terminated; size first with out=NULL), returning its length. mbLoadLocalStorage
// restores such a snapshot (merging into the current store). Save to disk after a login,
// reload it next run to resume the session — the localStorage peer of mbSaveCookies.
// Needs a real (http/https) origin, like the other storage calls.
MB_EXPORT int mbSaveLocalStorage(mbView*, char* out, int out_cap);
MB_EXPORT void mbLoadLocalStorage(mbView*, const char* json);

// Write the page's visible text (document.body.innerText) into `out`. Returns the
// full length in bytes (call with out=NULL/out_cap=0 first to size the buffer).
MB_EXPORT int mbGetText(mbView*, char* out, int out_cap);

// Write the rendered (post-JS) DOM as serialized HTML
// (document.documentElement.outerHTML) into `out`. Returns the full length in
// bytes (size first with out=NULL/out_cap=0 — pages can be large).
MB_EXPORT int mbGetHTML(mbView*, char* out, int out_cap);

// Write the ACCESSIBILITY TREE (the "accessibility snapshot") as compact JSON into
// `out`; returns the full length in bytes (size first with out=NULL/out_cap=0). Each
// node is {"role":..,"name":..[,"value":..][,"checked":..][,"focused":true][,"url":..]
// [,"level":N][,"x":,"y":,"w":,"h":][,"children":[..]]} — the semantic view of the page
// used by testing tools and AI/automation agents (roles + accessible names, not raw DOM).
// "checked" (true/false/"mixed") appears on checkboxes/radios/switches; "focused":true on
// the focused node; "url" is a link's/image's destination; "level" is a heading's 1..6.
// The optional x/y/w/h are the node's frame-relative bounds (present when non-empty); they
// are widget/page coordinates, so a caller can click a node's center with mbSendMouseClick.
// Returns 0 if there is no document. No compositor needed.
MB_EXPORT int mbGetAXTree(mbView*, char* out, int out_cap);

// FIND-IN-PAGE. Search the page for `text`; returns the TOTAL number of matches (0 if
// none). Also selects/scrolls to the first match and highlights ALL matches (the find
// markers render into a screenshot). `match_case` != 0 for case-sensitive search. Uses
// blink's real TextFinder. Call mbStopFind to clear the selection + highlights.
MB_EXPORT int mbFindText(mbView*, const char* text, int match_case);
// Step to the next (forward!=0) / previous match of the last mbFindText search, scrolling
// it into view and making it the active highlighted match; wraps around at the ends.
// Returns 1 if a match is now active, 0 if there was no prior search or no matches.
MB_EXPORT int mbFindNext(mbView*, int forward);
// Bounds of the ACTIVE find match (after mbFindText/mbFindNext) in viewport CSS pixels —
// the same coordinates mbSendMouseClick takes, so you can click or crop-screenshot the
// match (e.g. mbPaintRectToBitmap). Any of x/y/w/h may be NULL. Returns 1 if a match is
// active (values written), 0 otherwise.
MB_EXPORT int mbGetFindActiveRect(mbView*, int* x, int* y, int* w, int* h);
MB_EXPORT void mbStopFind(mbView*);

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

// Drain captured page console output (console.log/warn/error) into `out`
// (NUL-terminated, up to out_cap bytes; one "level: text" line per message), and
// clear the buffer. Returns the full output length in bytes.
MB_EXPORT int mbDrainConsole(mbView*, char* out, int out_cap);

// Per-frame selector ops — the typed peers of mbFillSelector / mbGetTextForSelector
// scoped to the frame_index-th child frame (-1 = main frame), host-privileged so
// they reach a CROSS-ORIGIN iframe. mbFillSelectorInFrame fills with the same
// React-compatible value-set + input/change dispatch as mbFillSelector (returns 1
// on success, 0 if nothing matched). mbGetTextForSelectorInFrame reads the first
// match's innerText (same out-buffer contract as mbGetTextForSelector; returns -1
// if nothing matched or the frame index is out of range). DOM-only — no synthetic
// gesture, so an iframe form/text is reachable without cross-frame coord mapping.
MB_EXPORT int mbFillSelectorInFrame(mbView*, int frame_index,
                                    const char* css_selector,
                                    const char* utf8_text);
MB_EXPORT int mbGetTextForSelectorInFrame(mbView*, int frame_index,
                                          const char* css_selector, char* out,
                                          int out_cap);

// Synchronously composite the current frame and copy pixels out as BGRA8888.
// 'out_bgra' must hold height*stride bytes. Returns 1 on success, 0 otherwise.
MB_EXPORT int mbPaintToBitmap(mbView*,
                              void* out_bgra,
                              int width,
                              int height,
                              int stride);

// Render the current frame and encode it to `path`. The image format follows the
// extension: .jpg/.jpeg -> JPEG (quality 90), anything else -> PNG. Returns 1 on success.
// ALPHA: unlike the raw paint buffers (premultiplied — see the header-top pixel
// contract), encoded PNGs carry STRAIGHT alpha as the format requires; the encoder
// unpremultiplies. Files from mbSavePng/mbEncodePng are correct as-is.
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

// Whether mbSavePdf/mbSavePdfEx include background colors/images (Puppeteer's
// printBackground). Off by default (blink's "save ink" print look drops backgrounds);
// enable so the PDF matches the screen. Applies to subsequent mbSavePdf* calls.
MB_EXPORT void mbSetPrintBackground(mbView*, int enabled);
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

#endif  // MINIBLINK2_AUTOMATION_H_
