// wke.h — miniblink `wke` C API, modern-Blink compatibility layer (FIRST SLICE).
//
// A drop-in subset of miniblink's classic wke API (github.com/weolar/miniblink49)
// implemented on top of the modern-M150 mb_capi host. Lets a headless wke app —
// init, create a view, load a page, poll loading state, read title/url, and paint
// to a BGRA buffer — run on modern Blink with the original wke signatures.
//
// Scope of this slice (no jsValue / callbacks yet): lifecycle, load, loading-state
// polling, dimensions, accessors, and pull-model paint. The async callback model
// (wkeOnLoadingFinish, wkeOnPaintBitUpdated) and the jsValue scripting layer
// (wkeRunJS) are deliberately deferred — load here is synchronous, so a poll of
// wkeIsLoadingCompleted (always true after wkeLoadURL returns) drives the flow.
//
// Self-contained: an embedder includes just this header and links libminiblink_host.

#ifndef MINIBLINK_WKE_WKE_H_
#define MINIBLINK_WKE_WKE_H_

#if defined(__cplusplus)
#include <cstdbool>
#define WKE_EXTERN_C extern "C"
#else
#include <stdbool.h>
#define WKE_EXTERN_C
#endif

#if defined(_WIN32)
#define WKE_API WKE_EXTERN_C __declspec(dllexport)
#else
#define WKE_API WKE_EXTERN_C __attribute__((visibility("default")))
#endif

// wke's char type (UTF-8 byte) and opaque web-view handle, matching upstream.
typedef char utf8;
typedef struct _tagWkeWebView* wkeWebView;

// Mouse-event message codes for wkeFireMouseEvent (Win32 WM_* values, matching
// upstream wkedefine.h).
enum {
  WKE_MSG_MOUSEMOVE = 0x0200,
  WKE_MSG_LBUTTONDOWN = 0x0201,
  WKE_MSG_LBUTTONUP = 0x0202,
  WKE_MSG_LBUTTONDBLCLK = 0x0203,
  WKE_MSG_RBUTTONDOWN = 0x0204,
  WKE_MSG_RBUTTONUP = 0x0205,
  WKE_MSG_RBUTTONDBLCLK = 0x0206,
  WKE_MSG_MBUTTONDOWN = 0x0207,
  WKE_MSG_MBUTTONUP = 0x0208,
  WKE_MSG_MBUTTONDBLCLK = 0x0209,
  WKE_MSG_MOUSEWHEEL = 0x020A,
};

// Modifier/button flags for the mouse-event `flags` argument.
enum {
  WKE_LBUTTON = 0x01,
  WKE_RBUTTON = 0x02,
  WKE_SHIFT = 0x04,
  WKE_CONTROL = 0x08,
  WKE_MBUTTON = 0x10,
};

// --- Lifecycle (process-wide init/teardown + per-view create/destroy) ----------
WKE_API void wkeInitialize(void);
WKE_API void wkeFinalize(void);
WKE_API wkeWebView wkeCreateWebView(void);
WKE_API void wkeDestroyWebView(wkeWebView webView);

// --- Loading -------------------------------------------------------------------
WKE_API void wkeLoadURL(wkeWebView webView, const utf8* url);
WKE_API void wkeLoadHTML(wkeWebView webView, const utf8* html);
WKE_API void wkeReload(wkeWebView webView);

// --- Loading state (poll these; load is synchronous in this slice) -------------
WKE_API bool wkeIsLoading(wkeWebView webView);
WKE_API bool wkeIsLoadingCompleted(wkeWebView webView);
WKE_API bool wkeIsLoadingSucceeded(wkeWebView webView);
WKE_API bool wkeIsLoadingFailed(wkeWebView webView);
WKE_API bool wkeIsDocumentReady(wkeWebView webView);

// --- Geometry ------------------------------------------------------------------
WKE_API void wkeResize(wkeWebView webView, int w, int h);
WKE_API int wkeGetWidth(wkeWebView webView);
WKE_API int wkeGetHeight(wkeWebView webView);
WKE_API int wkeWidth(wkeWebView webView);
WKE_API int wkeHeight(wkeWebView webView);

// --- Accessors / config --------------------------------------------------------
// The returned const utf8* is owned by the view and valid until the next call to
// the same getter on that view (the classic wke contract).
WKE_API const utf8* wkeGetURL(wkeWebView webView);
WKE_API const utf8* wkeGetTitle(wkeWebView webView);
WKE_API void wkeSetUserAgent(wkeWebView webView, const utf8* userAgent);

// --- Input ---------------------------------------------------------------------
// Deliver a mouse event at (x, y). `message` is one of the WKE_MSG_* codes;
// `flags` carries WKE_LBUTTON/etc. (currently advisory). Supported in this slice:
// MOUSEMOVE (move), LBUTTONUP (a left click — the press is implicit, as with a
// real button-up), and LBUTTONDBLCLK (double click). Returns true if handled.
WKE_API bool wkeFireMouseEvent(wkeWebView webView, unsigned int message, int x,
                               int y, unsigned int flags);

// --- Paint (pull model): render the view into a caller BGRA buffer -------------
// `bits` must hold width*height*4 bytes; `pitch` is the row stride in bytes
// (pass width*4 for a tightly-packed buffer).
WKE_API void wkePaint(wkeWebView webView, void* bits, int pitch);

#endif  // MINIBLINK_WKE_WKE_H_
