// webview.h — the INTERACTIVE-EMBEDDER core of the miniblink2 C API (mb* ABI):
// engine/view lifecycle, loads + lifecycle callbacks, interactive paint,
// input, resource interception, JS bridge, cookies, navigation. The
// automation/testing surface lives in automation.h (which includes this).
//
// This is the ONLY surface an embedding application links
// against. Everything below it is GN-built C++ that touches Blink/base/mojo.
// Pure C, no Blink types leak across this boundary.
//
// Status: Phase 1 v0 (render-to-bitmap, no input/JS-interaction yet).

#ifndef MINIBLINK2_WEBVIEW_H_
#define MINIBLINK2_WEBVIEW_H_

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

// ---- Version handshake -------------------------------------------------------
// Callable BEFORE mbInitialize — a dlopen-ing host verifies what it bound at
// load time instead of failing at the first missing symbol (or worse, silently
// running a mismatched pair). Returned strings are static; do not free.
// Engine (this library) version, e.g. "0.4.0-dev".
MB_EXPORT const char* mbVersion(void);
// Header/ABI compatibility number; bumped on any breaking change to this API.
// A host built against MB_API_VERSION N should refuse an engine reporting < N.
#define MB_API_VERSION 1
MB_EXPORT int mbApiVersion(void);
// The upstream Chromium the engine embeds, e.g. "150.0.7871.24".
MB_EXPORT const char* mbChromiumVersion(void);

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

// INTERACTIVE hosts: the re-entrancy-safe update slice. Several mb* calls drive
// a nested run loop (loads, waits, paints, pumps), and on shared-main-thread
// platforms host callbacks (timers, display links) can fire INSIDE them and
// call back into the engine. mbUpdate is the safe tick for that world:
//  - engine off the stack: runs pending engine work (one mbPumpMessages slice),
//    then drains callbacks queued by mbDefer;
//  - engine ON the stack (this tick fired mid-engine-call): returns immediately
//    instead of nesting — the outermost call's next mbUpdate does the work.
// Call it from the host's frame tick instead of mbPumpMessages.
MB_EXPORT void mbUpdate(void);

// Bound each mbUpdate slice to at most `seconds` of dispatch (e.g. 0.005 for
// a 5 ms budget at 60fps): when the budget is spent the slice returns and the
// remaining work runs on the NEXT update, so one busy page cannot hold the
// host's frame tick. 0 (the default) drains to idle. Applies to mbUpdate
// only — mbPumpMessages and the automation waits intentionally run to idle.
MB_EXPORT void mbSetMaxUpdateTime(double seconds);

// mbUpdate with the host's display-refresh timestamp (seconds, in the
// CACurrentMediaTime / mach monotonic domain - a CADisplayLink or
// CVDisplayLink timestamp passes straight through). requestAnimationFrame
// callbacks are stamped with THIS time instead of whenever the pump ran, so
// animation-driven content advances frame-aligned. Behaves exactly like
// mbUpdate otherwise (re-entrancy-safe, budget-bounded).
MB_EXPORT void mbUpdateAt(double frame_time_seconds);

// Release as much memory as possible: broadcasts critical memory pressure
// (blink's caches, decoded images and fonts listen) and triggers a full V8
// GC. Call when the host UI goes hidden/idle; content re-decodes lazily on
// the next paint.
MB_EXPORT void mbPurgeMemory(void);

// Log a coarse memory summary (V8 heap, malloc footprint) to stderr —
// before/after bookends for mbPurgeMemory.
MB_EXPORT void mbLogMemoryUsage(void);

// Host log sink: route the engine's diagnostic log (base logging — LOG(ERROR)
// and friends, including mbLogMemoryUsage's summary) to a callback instead of
// stderr, so it lands in the HOST's logging. `level` is 0 info / 1 warning /
// 2 error / 3 fatal; `message` is a single NUL-terminated line without the
// trailing newline. Process-wide; may be set before mbInitialize. The callback
// may fire on any engine thread — keep it thread-safe and cheap. NULL restores
// stderr.
typedef void (*mbLogCallback)(void* userdata, int level, const char* message);
MB_EXPORT void mbOnLogMessage(mbLogCallback cb, void* userdata);

// 1 while any engine-entering mb* call is on the current stack. Hosts that must
// gate their own work (e.g. skip a blit when a pump tick landed inside a load)
// can poll this instead of tracking call depth themselves.
MB_EXPORT int mbInEngineCall(void);

// Run  when the engine is next OFF the stack: immediately if it
// already is, otherwise from the tail of the next mbUpdate. Use for work that
// must ENTER the engine (create a view, start a load) but was scheduled from a
// callback firing inside an engine call — calling in directly from there would
// re-enter blink. Engine-side replacement for host-rolled deferral queues.
typedef void (*mbDeferredCallback)(void* userdata);
MB_EXPORT void mbDefer(mbDeferredCallback cb, void* userdata);

// View lifecycle. A view owns one WebView + main LocalFrame + WebFrameWidget.
// ---- Sessions (browsing profiles) -------------------------------------------
// A session isolates everything origin-keyed (DOM storage, IndexedDB, OPFS,
// storage buckets, locks; cookies join in stage 2) per profile. Sessions are
// capability handles: whoever holds the mbSession* controls the profile.
// persist_path == NULL -> EPHEMERAL (memory only). Non-NULL -> the profile
// roots at <persist_path>/<name>/ (durability lands in stage 3; the identity
// and layout are already stable). The mode is fixed for the session's life.
typedef struct mbSession mbSession;
MB_EXPORT mbSession* mbCreateSession(const char* name, const char* persist_path);
// Safe with views still open: the handle detaches, the profile tears down
// when the last view bound to it is destroyed.
MB_EXPORT void mbDestroySession(mbSession*);
// The implicit shared EPHEMERAL profile plain mbCreateView uses. Never
// destroy it.
MB_EXPORT mbSession* mbDefaultSession(void);
// Create a view inside `session` (falls back to the default session if NULL).
// A view's session is fixed before its first navigation commits.
MB_EXPORT mbView* mbCreateViewInSession(int width, int height, mbSession*);
MB_EXPORT mbSession* mbViewGetSession(mbView*);
// Wipe this profile: cookies, IndexedDB, OPFS. Live documents DOM storage
// (local/sessionStorage) is blink-internal and not reachable service-side -
// clear it per document via JS if needed (mbClearStorage in automation.h).
MB_EXPORT void mbSessionClearStorage(mbSession*);
// Durability barrier: write a PERSISTENT profile cookies/IndexedDB/OPFS
// under its persist dir now (also happens at final teardown). No-op for
// ephemeral profiles. localStorage is not persisted (see above).
MB_EXPORT void mbSessionFlush(mbSession*);

MB_EXPORT mbView* mbCreateView(int width, int height);
MB_EXPORT void    mbDestroyView(mbView*);
MB_EXPORT void    mbResize(mbView*, int width, int height);

// Compositing (experimental). When enabled, the NEXT mbCreateView attaches its widget
// COMPOSITING — blink drives cc into an in-process software viz::Display — instead of the
// default non-compositing software-paint path. Process-global; default OFF (screenshots
// are unchanged). mbViewFrameSinkRequested returns how many frame sinks blink's compositor
// has pulled (>0 once the view is shown + the message loop pumped), or -1 if the view is
// not compositing.
MB_EXPORT void    mbSetCompositingEnabled(int on);
MB_EXPORT int     mbViewFrameSinkRequested(mbView*);
// Drive one synchronous compositor frame (compositing views only; no-op otherwise) — a headless
// compositing widget has no browser begin-frame source, so frames are pulled explicitly.
MB_EXPORT void    mbViewComposite(mbView*);
// The SkColor (0xAARRGGBB) at (x,y) in the compositor's last captured frame, or 0 if not
// compositing / nothing composited. For verifying the cc -> viz -> bitmap path.
MB_EXPORT unsigned int mbViewCompositorPixel(mbView*, int x, int y);

// Content entry points.
//   mbLoadHTML  — render an in-memory document (no network). First render proof.
//   mbLoadURL   — fetch via libcurl (mb_url_loader) then render.
MB_EXPORT void mbLoadHTML(mbView*, const char* utf8_html, const char* base_url);
MB_EXPORT void mbLoadURL(mbView*, const char* utf8_url);

// Push notification of load completion — the real Blink DidFinishLoad signal (the
// main document's `load` event, all subresources done), not a poll or a fixed timer.
// Register a callback with mbOnLoadFinish (pass NULL to clear); it fires during the
// load call (the synchronous load pumps the message loop). mbIsLoadFinished queries
// the same state (1 once the current navigation's load has finished, 0 after a new
// load starts), so callers can wait on the real finish instead of a fixed delay.
typedef void (*mbLoadFinishCallback)(mbView*, void* userdata);
MB_EXPORT void mbOnLoadFinish(mbView*, mbLoadFinishCallback, void* userdata);

// Fires when a MAIN-FRAME navigation commits: the previous document is gone
// and the new one starts loading subresources. Completes the lifecycle set
// (begin -> DOMContentLoaded -> finish, or begin -> fail). `url` is the
// committed document URL, valid only for the duration of the call.
typedef void (*mbBeginLoadingCallback)(mbView*, void* userdata, const char* url);
MB_EXPORT void mbOnBeginLoading(mbView*, mbBeginLoadingCallback, void* userdata);

// Fires when a TOP-LEVEL load fails before producing a document (file unread-
// able, fetch error). `error` is a short description (may be empty). The
// load-finish callback still fires right after — mbIsLoadFinished stays the
// single completion signal; this adds the failure reason.
typedef void (*mbFailLoadingCallback)(mbView*, void* userdata, const char* url,
                                      const char* error);
MB_EXPORT void mbOnFailLoading(mbView*, mbFailLoadingCallback, void* userdata);
MB_EXPORT int  mbIsLoadFinished(mbView*);

// Fires when the main document's DOMContentLoaded event dispatches — the DOM is parsed and
// deferred scripts have run, but subresources/images may still be loading. This is the
// "page interactive" signal and is EARLIER than mbOnLoadFinish (which waits for `load` /
// all subresources), matching Puppeteer/Playwright's 'domcontentloaded' wait. NULL clears.
typedef void (*mbDOMContentLoadedCallback)(mbView*, void* userdata);
MB_EXPORT void mbOnDOMContentLoaded(mbView*, mbDOMContentLoadedCallback, void* userdata);

// Navigation policy/notification: the callback fires for each PAGE-initiated main-frame
// navigation (link click, location= assignment, form submit, JS redirect) with its
// target `url`, BEFORE it commits. Return 1 to allow, 0 to BLOCK it — so you can stop
// popups / redirects / the page navigating away, or just observe navigations. Host-
// driven mbLoadURL does NOT route through here (it is your own action). NULL clears it.
typedef int (*mbNavigationCallback)(mbView*, void* userdata, const char* url);
MB_EXPORT void mbOnNavigation(mbView*, mbNavigationCallback, void* userdata);

// URL-changed notification: fires on EVERY main-frame commit (host load, page-initiated
// navigation, server redirect, reload) with the new `url` — track where the view is
// (redirects, SPA-style location changes, form-submit landings). NULL clears it.
typedef void (*mbUrlChangedCallback)(mbView*, void* userdata, const char* url);
MB_EXPORT void mbOnUrlChanged(mbView*, mbUrlChangedCallback, void* userdata);

// Title-changed notification: fires whenever the main document's title changes — the
// initial <title> and every dynamic document.title write — with the new `title` (UTF-8).
// Lets automation track tab titles / progress indicators. NULL clears it.
typedef void (*mbTitleChangedCallback)(mbView*, void* userdata, const char* title);
MB_EXPORT void mbOnTitleChanged(mbView*, mbTitleChangedCallback, void* userdata);

// Favicon-changed notification: fires when the main document reports its favicon URL(s)
// — `favicon_urls` is newline-separated (the standard <link rel=icon> first, then any
// apple-touch-icon etc.), absolute URLs. Completes the browser tab-metadata trio with
// mbOnUrlChanged / mbOnTitleChanged. NULL clears it.
typedef void (*mbFaviconChangedCallback)(mbView*, void* userdata, const char* favicon_urls);
MB_EXPORT void mbOnFaviconChanged(mbView*, mbFaviconChangedCallback, void* userdata);

// Download diversion: when a top-level navigation (mbLoadURL to an http(s)/data: URL)
// returns a DOWNLOAD — Content-Disposition: attachment, or a non-renderable MIME — and a
// callback is registered, the response is handed to it (url, mime, suggested filename, and
// the body bytes — NOT NUL-safe, use `len`) INSTEAD of being rendered, so a download link
// saves a file rather than committing garbage. NULL clears it (default: render as before).
typedef void (*mbDownloadCallback)(mbView*, void* userdata, const char* url,
                                   const char* mime, const char* filename,
                                   const char* data, int len);
MB_EXPORT void mbOnDownload(mbView*, mbDownloadCallback, void* userdata);

// New-window notification: the callback fires when the page requests a new window
// (window.open() or a target=_blank activation), with the requested `url` and window
// `name`. It is a notification only — the popup is not auto-created (window.open returns
// null), so an embedder can decide what to do (e.g. load `url` in this or a new view).
// NULL clears it.
typedef void (*mbNewWindowCallback)(mbView*, void* userdata, const char* url,
                                    const char* name);
MB_EXPORT void mbOnNewWindow(mbView*, mbNewWindowCallback, void* userdata);

// ---- Pointer-UI state (page -> host) -----------------------------------------
// An offscreen view has no OS window, so the engine must TELL the host when the
// page wants a different pointer: an I-beam over selectable text, a hand over a
// link, resize arrows at a draggable edge. The callback fires with an
// MB_CURSOR_* code whenever blink changes the cursor for this view (on change
// only, not per mouse move). The values mirror blink's cursor-type enum;
// unlisted/custom cursors report MB_CURSOR_POINTER. NULL clears.
#define MB_CURSOR_POINTER      0   /* default arrow */
#define MB_CURSOR_CROSS        1
#define MB_CURSOR_HAND         2   /* links */
#define MB_CURSOR_IBEAM        3   /* text */
#define MB_CURSOR_WAIT         4
#define MB_CURSOR_HELP         5
#define MB_CURSOR_EAST_RESIZE  6
#define MB_CURSOR_NORTH_RESIZE 7
#define MB_CURSOR_NORTH_EAST_RESIZE 8
#define MB_CURSOR_NORTH_WEST_RESIZE 9
#define MB_CURSOR_SOUTH_RESIZE 10
#define MB_CURSOR_SOUTH_EAST_RESIZE 11
#define MB_CURSOR_SOUTH_WEST_RESIZE 12
#define MB_CURSOR_WEST_RESIZE  13
#define MB_CURSOR_NS_RESIZE    14
#define MB_CURSOR_EW_RESIZE    15
#define MB_CURSOR_NESW_RESIZE  16
#define MB_CURSOR_NWSE_RESIZE  17
#define MB_CURSOR_COL_RESIZE   18
#define MB_CURSOR_ROW_RESIZE   19
#define MB_CURSOR_MOVE         29
#define MB_CURSOR_VERTICAL_TEXT 30
#define MB_CURSOR_CELL         31
#define MB_CURSOR_CONTEXT_MENU 32
#define MB_CURSOR_ALIAS        33
#define MB_CURSOR_PROGRESS     34
#define MB_CURSOR_NO_DROP      35
#define MB_CURSOR_COPY         36
#define MB_CURSOR_NONE         37
#define MB_CURSOR_NOT_ALLOWED  38
#define MB_CURSOR_ZOOM_IN      39
#define MB_CURSOR_ZOOM_OUT     40
#define MB_CURSOR_GRAB         41
#define MB_CURSOR_GRABBING     42
typedef void (*mbCursorChangedCallback)(mbView*, void* userdata, int cursor);
MB_EXPORT void mbOnCursorChanged(mbView*, mbCursorChangedCallback, void* userdata);

// Hover tooltip (the <element title="..."> bubble a browser window would show).
// Fires with the UTF-8 text when a tooltip should appear and with "" when it
// should hide. The pointer is valid only during the call. NULL clears.
typedef void (*mbTooltipChangedCallback)(mbView*, void* userdata,
                                         const char* text);
MB_EXPORT void mbOnTooltipChanged(mbView*, mbTooltipChangedCallback, void* userdata);

// window.close(): the page asked to close its window (a close button, an OAuth
// flow finishing). Notification only — the engine does nothing; the host
// decides whether to hide or destroy the view. NULL clears.
typedef void (*mbRequestCloseCallback)(mbView*, void* userdata);
MB_EXPORT void mbOnRequestClose(mbView*, mbRequestCloseCallback, void* userdata);

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

// Persistent per-view USER-origin stylesheet: injected engine-side into every
// document this view commits — survives navigations, participates in the
// cascade at user-agent level, and never appears in the page's DOM (page JS
// cannot see or remove it). Use for host presentation (scrollbars, background,
// theming) instead of splicing <style> into the HTML. NULL or "" clears it.
// Contrast mbInsertCSS: a one-shot DOM <style> append to the CURRENT document.
MB_EXPORT void mbSetUserStylesheet(mbView*, const char* css);

// Per-view generic font-family defaults (what CSS "serif"/"sans-serif"/
// "monospace" and unstyled text resolve to), for all scripts without a more
// specific mapping. NULL/"" leaves that family unchanged. Set before (or
// after - applies on next style recalc) loading; the engine defaults are
// Times/Courier/Helvetica, a poor fit for CJK-heavy content.
MB_EXPORT void mbSetFontFamilies(mbView*, const char* standard,
                                 const char* fixed, const char* serif,
                                 const char* sans_serif);

// Per-character font fallback: when no mapped font covers a character (a rare
// CJK ideograph, a symbol), the callback is consulted BEFORE the platform
// cascade with the codepoint (UTF-32), the run's weight (100-900) and italic
// flag. Return 1 and write a font family name (UTF-8) into out[out_cap] to
// use that family; return 0 to defer to the engine's platform fallback. The
// named family must actually cover the character or the answer is ignored (no
// tofu from a wrong answer). Process-wide; called on the engine thread during
// layout — keep it fast (cache your answers). NULL clears. The dynamic
// counterpart of mbSetFontFamilies' static defaults.
typedef int (*mbFontFallbackCallback)(void* userdata, unsigned int codepoint,
                                      int weight, int italic,
                                      char* out, int out_cap);
MB_EXPORT void mbSetFontFallbackCallback(mbFontFallbackCallback, void* userdata);

// ---- DevTools (CDP) --------------------------------------------------------
// One Chrome-DevTools-Protocol session per view (stage A of the inspector
// plan in IMPROVEMENT.md). Attach starts the session; Send dispatches ONE
// client command (CDP JSON, must carry "id" and "method"); responses and
// notifications arrive on the callback as CDP JSON, delivered from the task
// queue - keep calling mbUpdate for messages to flow. Bridge the pipe to a
// WebSocket + /json discovery endpoint and ordinary Chrome connects as the
// frontend. Returns 1 on attach success; a second attach fails (one session).
typedef void (*mbDevToolsMessageCallback)(mbView*, void* userdata,
                                          const char* json, int len);
MB_EXPORT int  mbDevToolsAttach(mbView*, mbDevToolsMessageCallback, void* userdata);
MB_EXPORT void mbDevToolsSend(mbView*, const char* json, int len);
MB_EXPORT void mbDevToolsDetach(mbView*);

// Debugger pause/resume notification. Fires with paused=1 when the inspector
// parks the main thread at a breakpoint (Debugger.pause, a `debugger`
// statement, an exception with pause-on-exceptions) and paused=0 on resume.
// While paused, the main thread sits in a nested inspector loop: mbUpdate and
// the paint calls will not advance the page — stop the frame tick and show a
// "paused in debugger" state instead of treating it as a hang. The callback
// fires from INSIDE that nested loop: keep it cheap and do not call blocking
// mb* entry points from it (mbDevToolsSend of interrupt-class commands like
// Debugger.resume is safe — it rides the IO session). May be registered
// before or after mbDevToolsAttach; survives detach/re-attach; NULL clears.
typedef void (*mbDevToolsPausedCallback)(mbView*, void* userdata, int paused);
MB_EXPORT void mbOnDevToolsPaused(mbView*, mbDevToolsPausedCallback,
                                  void* userdata);

// ---- <select> popup menus ---------------------------------------------------
// The engine renders no native popups: clicking a <select> (or any menulist
// chooser) surfaces the menu to the HOST via this callback — show your own
// menu UI at (x,y,width,height) (view coordinates, the element's anchor rect),
// then reply exactly once with mbSelectPopupCommit (indices into the delivered
// items array — group headers/separators count in that index space) or
// mbSelectPopupCancel. Replying is REQUIRED for the page to proceed (blink
// waits for PopupDidHide); destroying the view cancels automatically. With no
// callback registered the menu cancels immediately (the legacy no-op, done
// cleanly). item/info pointers are valid only during the callback — copy what
// you keep. A second popup opening replaces an unanswered first (it cancels).

#define MB_POPUP_ITEM_OPTION           0
#define MB_POPUP_ITEM_CHECKABLE_OPTION 1
#define MB_POPUP_ITEM_GROUP            2  /* <optgroup> header, not selectable */
#define MB_POPUP_ITEM_SEPARATOR        3
#define MB_POPUP_ITEM_SUBMENU          4

typedef struct mbSelectPopupItem {
  const char* label;   // UTF-8
  int type;            // MB_POPUP_ITEM_*
  int enabled;
  int checked;
} mbSelectPopupItem;

typedef struct mbSelectPopupInfo {
  int struct_size;     // = sizeof(mbSelectPopupInfo), ABI versioning
  int x, y, width, height;  // anchor rect of the <select>, view coordinates
  double font_size;    // the element's font size (px) — size your menu like it
  int selected_index;  // currently selected item index, -1 = none
  int right_aligned;
  int allow_multiple;  // multi-<select>: commit may carry several indices
  int item_count;
  const mbSelectPopupItem* items;
} mbSelectPopupInfo;

typedef void (*mbSelectPopupCallback)(mbView*, void* userdata,
                                      const mbSelectPopupInfo* info);
MB_EXPORT void mbOnSelectPopup(mbView*, mbSelectPopupCallback, void* userdata);
// Commit the pending popup with the chosen item indices (usually one; several
// only when allow_multiple). count==0 cancels. Returns 1 if a popup was
// pending, 0 otherwise.
MB_EXPORT int  mbSelectPopupCommit(mbView*, const int* indices, int count);
MB_EXPORT void mbSelectPopupCancel(mbView*);

// Synthesize a left mouse click (down+up) at (x,y) in the view.
// ---- Typed input events -----------------------------------------------------
// Structured alternatives to the positional mbSend* shorthands: explicit
// fields, precise units, room to grow without another trailing parameter.
// Set struct_size = sizeof(the struct); it versions the ABI (a newer engine
// can tell how much of the struct the caller filled).

#define MB_MOUSE_MOVE 0
#define MB_MOUSE_DOWN 1
#define MB_MOUSE_UP   2

typedef struct mbMouseEvent {
  int struct_size;  // = sizeof(mbMouseEvent)
  int type;         // MB_MOUSE_MOVE / MB_MOUSE_DOWN / MB_MOUSE_UP
  int x, y;         // logical (CSS) px in the view
  int button;       // 0 left, 1 middle, 2 right (ignored for MOVE)
  int click_count;  // DOWN/UP: 1 = single, 2 = double-click
  int modifiers;    // bitmask: 1 ctrl, 2 shift, 4 alt, 8 meta
} mbMouseEvent;
MB_EXPORT void mbSendMouseEvent(mbView*, const mbMouseEvent*);

typedef struct mbWheelEvent {
  int struct_size;         // = sizeof(mbWheelEvent)
  int x, y;                // logical px
  float delta_x, delta_y;  // precise pixel deltas, DOM sign (deltaY>0 = content down)
  int precise;             // 1 = trackpad-style precise deltas
  int phase;               // RESERVED, must be 0 (no compositor gesture generator)
  int modifiers;           // bitmask as above
} mbWheelEvent;
// Returns 1 if a blocking listener consumed the wheel (called preventDefault) -
// the host then suppresses its default scroll.
MB_EXPORT int mbSendWheelEvent(mbView*, const mbWheelEvent*);

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

// Write the full scrollable document size (logical px; >= the viewport) into
// *w/*h (either may be NULL). For a full-page screenshot: mbGetContentSize ->
// mbResize(view, w, h) -> mbPaintToBitmap. Returns 1 on success.
MB_EXPORT int mbGetContentSize(mbView*, int* w, int* h);

// Write the current viewport size in logical (CSS) px into *w/*h (either may be
// NULL) — window.innerWidth/Height, the read-back peer of mbResize/mbCreateView
// (DPR-independent). Distinct from mbGetContentSize (the full scrollable doc).
// Returns 1 on success. (Needs a committed document.)
MB_EXPORT int mbGetViewSize(mbView*, int* w, int* h);

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

// Move the mouse pointer to (x,y): updates :hover state, fires mouseover/mousemove.
MB_EXPORT void mbSendMouseMove(mbView*, int x, int y);

// Dispatch a TRUSTED mouse-wheel at (x,y) with DOM-convention pixel deltas
// (deltaY>0 = scroll down, deltaX>0 = right). Fires the page's `wheel` handlers with
// event.isTrusted == true and the correct deltaX/deltaY, THEN scrolls the document
// viewport by the deltas — UNLESS a (non-passive) handler calls preventDefault, which
// suppresses the scroll, exactly like a real browser. Covers both wheel-driven UIs
// (map/canvas zoom, scroll hijacking, "load more on scroll") and plain page scrolling.
// `modifiers` bitmask: 1=ctrl 2=shift 4=alt 8=meta (ctrl+wheel = pinch-zoom intent).
// (The native compositor wheel->scroll path is absent in this non-compositing widget,
// so the scroll is applied programmatically to the document viewport.)
MB_EXPORT void mbSendWheel(mbView*, int x, int y, int deltaX, int deltaY, int modifiers);

// Set the device pixel ratio (HiDPI / retina). The view keeps laying out in CSS px
// but window.devicePixelRatio reports `scale` and paint output is rasterized at
// `scale`x. Allocate paint/PNG buffers at logical_width*scale x logical_height*scale.
MB_EXPORT void mbSetDeviceScaleFactor(mbView*, float scale);

// PAGE ZOOM (Ctrl+/Ctrl- in a browser): re-lay-out the whole page at `factor` (1.0 =
// 100%, 1.5 = 150%, 0.75 = 75%) — text AND layout scale, so screenshots capture the zoom.
// Layout-level (no compositor). Distinct from mbSetDeviceScaleFactor (HiDPI raster
// crispness at the same CSS layout). mbGetZoomFactor returns the current factor.
MB_EXPORT void mbSetZoomFactor(mbView*, float factor);
MB_EXPORT float mbGetZoomFactor(mbView*);

// Run a blink EDITING command on the focused frame — the classic webview editor ops for
// hosting editable content (contenteditable / inputs): "SelectAll", "Copy", "Cut",
// "Paste", "Undo", "Redo", "Delete", "Bold", "Italic", "Unselect", "InsertText", etc.
// Copy/Cut write the in-process clipboard (mbGetClipboard); Paste reads it. Returns 1 if
// the command applied, 0 otherwise. DOM/editing layer (no compositor).
MB_EXPORT int mbExecuteEditCommand(mbView*, const char* command);
// Editor command that takes a VALUE: "InsertText"/"InsertHTML" (insert at the caret / over
// the selection), "FontName", "FontSize", "ForeColor", "CreateLink", "Indent", etc.
// Returns 1 if applied. (Needs a focused editable, like mbExecuteEditCommand.)
MB_EXPORT int mbExecuteEditCommandValue(mbView*, const char* command, const char* value);

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

// 1 when the page has VISIBLE keyboard focus — the focused element accepts text
// (an <input>/<textarea>/contenteditable with a caret) — 0 otherwise. Distinct
// from window focus (mbSetFocus): a host with global hotkeys routes keystrokes
// to the page only while this is 1, and keeps its own shortcuts otherwise.
MB_EXPORT int mbHasInputFocus(mbView*);

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

// Like mbSendKey but WITH a modifier bitmask (1=ctrl 2=shift 4=alt 8=meta), and `key` may
// be a named key ("ArrowRight", "Home", "Enter", ...) OR a single character ("a", "1").
// Sends a real trusted key event, so keyboard SHORTCUTS fire (Ctrl+A select-all, Ctrl+S,
// app hotkeys) and Shift+Arrow extends a selection. No typed character is produced when a
// command modifier (ctrl/alt/meta) is held. No-op for an unknown multi-char key name.
MB_EXPORT void mbSendKeyEx(mbView*, const char* key, int modifiers);

// Dispatch a standalone key RELEASE (a `keyup` event) for a Win32 virtual-key code,
// so page handlers that watch for key release fire. Use when driving key down/up
// separately rather than as one press (mbSendKey already includes the release).
MB_EXPORT void mbSendKeyUp(mbView*, int windows_key_code);

// Type text via an input method (IME) into the FOCUSED editable: `composing` previews
// the in-progress reading (fires compositionstart/compositionupdate), `committed`
// inserts the final text (fires compositionend + input) — for testing CJK / accented
// input. Either may be NULL/empty. Focus an input first (e.g. mbFocusSelector).
MB_EXPORT void mbSendIme(mbView*, const char* composing, const char* committed);

// Synthesize a gesture scroll at (x,y) by (dx,dy) pixels. Positive dy scrolls
// the page downward (toward larger window.scrollY), matching natural intent.
MB_EXPORT void mbSendScroll(mbView*, int x, int y, int dx, int dy);

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

// Network control: block any fetched URL containing `substring` (failed with
// ERR_BLOCKED_BY_CLIENT instead of loaded) — suppress ads / trackers / images /
// analytics for faster, cleaner scrapes and screenshots. mbBlockUrl registers a
// substring (call repeatedly for several); mbClearUrlBlocks removes all. Process-
// wide, applied at the loader. Set before navigating to affect that page's loads.
MB_EXPORT void mbBlockUrl(const char* substring);
MB_EXPORT void mbClearUrlBlocks(void);

// Block (or unblock) a whole RESOURCE TYPE by its fetch destination string: "image",
// "font", "style", "script", "media"/"audio"/"video", "iframe", "track", ... A scrape can
// skip heavy classes (block "image"+"font"+"media") to load text-only pages fast without
// listing URLs. blocked!=0 blocks, 0 unblocks. Process-wide. Blocked requests fail with
// ERR_BLOCKED_BY_CLIENT, like mbBlockUrl.
MB_EXPORT void mbBlockResourceType(const char* type, int blocked);

// Dynamic per-request hook: a process-wide callback invoked for EVERY request URL
// the loader handles (alongside the static block/mock/rewrite tables), so you can
// inspect and decide at runtime instead of pre-registering substrings. Return nonzero
// to BLOCK the request (failed with ERR_BLOCKED_BY_CLIENT), zero to allow. The
// callback runs on the main thread during the load. Pass NULL to clear.
typedef int (*mbRequestCallback)(const char* url, void* userdata);
MB_EXPORT void mbSetRequestCallback(mbRequestCallback cb, void* userdata);

// Richer per-request hook for inspection/monitoring: like mbSetRequestCallback but the
// callback also gets the request `method` (GET/POST/...), the request `headers`
// ("\n"-joined "Name: value" lines), and the POST/PUT `body` (`body`/`body_len`; empty for
// GET) — so an embedder can monitor exactly what API calls a page makes, not just URLs.
// Return nonzero to BLOCK, zero to allow. Setting either request callback replaces the
// other (one slot). NULL clears it.
typedef int (*mbRequestCallbackEx)(const char* url, const char* method,
                                   const char* headers, const char* body,
                                   int body_len, void* userdata);
MB_EXPORT void mbSetRequestCallbackEx(mbRequestCallbackEx cb, void* userdata);

// Response hook: a process-wide callback invoked after a successful load (fetch / mock /
// file / data) with an opaque mbResponse handle, BEFORE the body reaches the page — so
// you can inspect the headers / status or REPLACE the response bytes (inject a script,
// strip content, rewrite a JSON payload). The handle is valid only for the duration of the
// callback; query it with mbResponseURL/Status/Headers/Body and replace the body with
// mbResponseSetBody (which updates the delivered length). Runs on the main thread inside
// the load. NULL clears it.
typedef struct mbResponse mbResponse;
typedef void (*mbResponseCallback)(mbResponse*, void* userdata);
MB_EXPORT void mbSetResponseCallback(mbResponseCallback cb, void* userdata);
MB_EXPORT const char* mbResponseURL(mbResponse*);
MB_EXPORT int mbResponseStatus(mbResponse*);
// Rewrite the HTTP status the page will see for this response (e.g. force 503 to test a
// page's retry/error path, or normalize an upstream 500 to 200). With mbResponseSetBody
// this lets the response hook fully fabricate a response dynamically — route.fulfill-like,
// but decided from the actual upstream response (vs the static mbMockResponse).
MB_EXPORT void mbResponseSetStatus(mbResponse*, int status);
// The raw response HEADER block (final response's header lines, "\r\n"-separated). Empty
// for non-http loads (data:/file:/mock). Read Content-Type, Set-Cookie, rate-limit, or any
// custom API header an app cares about. Valid only during the callback.
MB_EXPORT const char* mbResponseHeaders(mbResponse*);
// Inject or override a response header (case-insensitive name; any existing same-name line
// is replaced). For subresource/fetch/XHR loads the page's fetch Response.headers / XHR
// getResponseHeader see it; setting "Content-Type" also changes the delivered MIME (e.g.
// force a text/plain payload to parse as a stylesheet, or add a permissive CORS header).
MB_EXPORT void mbResponseSetHeader(mbResponse*, const char* name, const char* value);
// Returns the body bytes (NOT NUL-terminated; may contain NULs). *out_len gets the length.
MB_EXPORT const char* mbResponseBody(mbResponse*, int* out_len);
MB_EXPORT void mbResponseSetBody(mbResponse*, const char* body, int len);

// Notification hook: a process-wide callback fired when a page DISPLAYS a Notification
// (new Notification(title, {body, tag, icon})). The embedder gets the notification's
// fields and can surface it (a native toast / its own UI) — otherwise a page notification
// is invisible to the host. (Notification.permission is granted, onshow still fires.)
// Process-wide (the NotificationService is not view-scoped, like mbSetResponseCallback).
// NULL clears it. `tag`/`icon` may be "" when unset.
typedef void (*mbNotificationCallback)(void* userdata, const char* title,
                                       const char* body, const char* tag,
                                       const char* icon);
MB_EXPORT void mbOnNotificationShown(mbNotificationCallback cb, void* userdata);

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

// Dynamic request mock — like mbMockResponse but DECIDED per-request by a callback, for
// URLs you cannot pre-register as a fixed substring (e.g. compute a JSON body from the
// path, serve a different response per request). Consulted only when no static mock matches.
// The callback returns 1 to serve a computed response (call mbRequestMockResponse to fill
// it), or 0 to let the request fetch normally. One callback process-wide; NULL clears it.
typedef struct mbRequestMock mbRequestMock;
// Fill the response to serve (body bytes + length; content_type defaults text/html, status
// defaults 200). Valid only during the callback.
MB_EXPORT void mbRequestMockResponse(mbRequestMock*, const char* body, int len,
                                     const char* content_type, int status);
typedef int (*mbRequestMockCallback)(const char* url, mbRequestMock*, void* userdata);
MB_EXPORT void mbSetRequestMockCallback(mbRequestMockCallback, void* userdata);

// Per-VIEW dynamic request mock: same callback shape as
// mbSetRequestMockCallback but scoped to requests initiated by THIS view: its
// document (navigations), subresource loads, view-level fetch helpers
// (downloads), and worker main scripts — a dedicated worker uses its creating
// document's view; a shared worker uses the view of the connection that
// STARTS it (later connections reuse the running worker). Consulted after the
// static mock table and before the process-wide callback. Residual viewless
// fetches (a NESTED worker's script, fetches after the view is destroyed)
// fall through to the process-wide hook. Pass NULL to remove; removed
// automatically when the view is destroyed.
MB_EXPORT void mbOnRequestMock(mbView*, mbRequestMockCallback, void* userdata);

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

// In-process text clipboard shared with the page (navigator.clipboard / execCommand).
// mbSetClipboard makes the page's next navigator.clipboard.readText() / paste see `text`;
// mbGetClipboard writes the current clipboard text into `out` (NUL-terminated; size first
// with out=NULL) and returns its length — read what a page copied via writeText/copy.
// Process-wide. clipboard-read/write permission is granted so navigator.clipboard works.
MB_EXPORT void mbSetClipboard(const char* utf8_text);
MB_EXPORT int  mbGetClipboard(char* out, int out_cap);

// Write the committed main document's URL (the final URL after any redirects)
// into `out` (NUL-terminated, up to out_cap). Returns the full length in bytes.
MB_EXPORT int mbGetURL(mbView*, char* out, int out_cap);

// Write the current document's title into `out` (NUL-terminated, up to out_cap).
// Returns the full length in bytes.
MB_EXPORT int mbGetTitle(mbView*, char* out, int out_cap);

// Re-navigate to the current document's URL, re-fetching it. No-op for in-memory
// documents (about:blank, data:, mbLoadHTML content).
MB_EXPORT void mbReload(mbView*);

// Cancel the main frame's in-flight load (provisional navigation or pending main
// resource). No-op when the frame is already idle.
MB_EXPORT void mbStopLoading(mbView*);

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

// Write a human-readable reason the last top-level load FAILED at the network /
// transport layer (e.g. "Couldn't resolve host name", "Couldn't connect to server",
// "SSL connect error", "Timeout was reached", "file not found or unreadable") into
// `out`. Returns the length in bytes (size first with out=NULL/out_cap=0); 0 (empty)
// if the last load SUCCEEDED — including HTTP 4xx/5xx, which still commit, so use
// mbGetHttpStatus for those. Complements mbGetHttpStatus: network- vs HTTP-level.
MB_EXPORT int mbGetLastError(mbView*, char* out, int out_cap);

// Host-driven back/forward over the main frame's navigation history (captures
// both host-initiated loads and page-initiated link/location/form navigations).
// mbGoBack/mbGoForward return 1 if they navigated, 0 if there was no entry.
MB_EXPORT int mbCanGoBack(mbView*);
MB_EXPORT int mbCanGoForward(mbView*);
MB_EXPORT int mbGoBack(mbView*);
MB_EXPORT int mbGoForward(mbView*);

// History-state push: fires with the new (can_go_back, can_go_forward) whenever
// the session history changes — commits, traversals, truncation — and only when
// one of the two flags actually flipped. Hosts enable/disable their nav buttons
// event-driven instead of polling mbCanGoBack/Forward per tick. NULL clears.
typedef void (*mbHistoryChangedCallback)(mbView*, void* userdata,
                                         int can_go_back, int can_go_forward);
MB_EXPORT void mbOnHistoryChanged(mbView*, mbHistoryChangedCallback, void* userdata);

// Live console push: the callback fires for EACH page console message as it happens
// (console.log/warn/error) with its `level` ("log"/"warn"/"error"/"verbose") and `message`
// text — react to errors/logs during a long-running script instead of polling
// mbDrainConsole afterward. NULL clears it. (Messages are still buffered for DrainConsole.)
typedef void (*mbConsoleCallback)(mbView*, void* userdata, const char* level,
                                  const char* message);
MB_EXPORT void mbOnConsoleMessage(mbView*, mbConsoleCallback cb, void* userdata);

// Richer console push for error monitoring: like mbOnConsoleMessage but also delivers the
// `source` URL, `line` number, and JS `stack`. UNCAUGHT EXCEPTIONS and unhandled promise
// rejections arrive here (blink reports them as console errors) with their message +
// source + line — so an embedder can monitor page health and locate failures. `stack` is
// the full JS call stack for console.* messages (a detailed-message opt-in is enabled);
// for plain thrown exceptions only source/line are available (stack is ""). `source`/
// `stack` are "" when not available. NULL clears it. Setting either console callback
// replaces the other (one slot).
typedef void (*mbConsoleCallbackEx)(mbView*, void* userdata, const char* level,
                                    const char* message, const char* source,
                                    int line, const char* stack);
MB_EXPORT void mbOnConsoleMessageEx(mbView*, mbConsoleCallbackEx cb, void* userdata);

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
// Returns the value length in bytes (JS typeof-style type probing).
MB_EXPORT int mbEvalJSEx(mbView*, const char* utf8_script, char* out_value,
                         int value_cap, char* out_type, int type_cap);

// Like mbEvalJS, but a THROWN exception is reported in out_exception (message
// plus source line) instead of being swallowed — an empty exception string
// means the script completed. Returns the result length (see mbEvalJS).
// Documented lifecycle: the JS world (and everything registered with
// mbJsBindFunction) resets on every navigation; re-establish bindings from
// mbOnBeginLoading / mbOnDOMContentLoaded. mbSetInitScript survives
// navigations — the engine re-injects it into each new document.
MB_EXPORT int mbEvalJSCatch(mbView*, const char* utf8_script,
                            char* out_value, int value_cap,
                            char* out_exception, int exc_cap);

// Like mbEvalJS, but runs in a dedicated ISOLATED world: the script has its own
// JS globals (separate from the page and from mbRunJS/mbEvalJS's main world) yet
// shares the same DOM — the content-script model, for automation that must not
// collide with or be observed by page script.
MB_EXPORT int mbEvalJSIsolated(mbView*, const char* utf8_script, char* out, int out_cap);

// Fast INTERACTIVE repaint: same BGRA8888 output as mbPaintToBitmap but WITHOUT the
// one-shot lifecycle settle (no nested task-queue drain). For a host that blits the
// view continuously (a windowed browser at ~60fps) — mbPaintToBitmap's per-call
// settle makes that crawl on live pages. Use mbPaintToBitmap for a one-shot capture.
MB_EXPORT int mbRepaintToBitmap(mbView*,
                                void* out_bgra,
                                int width,
                                int height,
                                int stride);

// 1 when blink has requested a new frame since the last successful paint
// (style/layout invalidation, animations, rAF) — 0 means the last painted frame
// is still current and a polling host can SKIP its mbRepaintToBitmap/blit for
// this tick. Snapshot semantics: painting clears the flag up front, so a
// request landing mid-paint marks the NEXT frame. Composited views (see
// mbSetCompositingEnabled) always report 1. New views start dirty.
MB_EXPORT int mbViewIsDirty(mbView*);
#if defined(__cplusplus)
}  // extern "C"
#endif

#endif  // MINIBLINK2_WEBVIEW_H_
