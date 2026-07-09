// mb_window — the tiny cross-platform window host the mb samples build on.
//
// miniblink2 is deliberately WINDOWLESS: the engine renders offscreen and the
// host owns the window, the run loop, and input. This helper is the samples-
// level analog of Ultralight's AppCore layer — it lives HERE, in samples/, and
// never in the SDK, because a couple hundred lines per platform is all a real
// host needs (see IMPROVEMENT.md round 5, "App convenience layer: noted, not
// adopted"). Backends: compat/mac/mb_window.mm (Cocoa) and
// compat/win/mb_window.cc (Win32); the samples themselves are OS-independent.
//
// What each window wires (one offscreen mbView per window):
//   - blit: mbRepaintToBitmap (the fast interactive repaint) into the window,
//     damage-gated with mbViewIsDirty so clean frames cost nothing;
//   - input: native mouse/wheel/keyboard -> the mbSend* trusted-event calls;
//   - resize: window resize -> mbResize (logical CSS px; HiDPI raster via
//     mbSetDeviceScaleFactor at the display's scale);
//   - pointer UI: mbOnCursorChanged -> the native cursor (I-beam over text,
//     hand over links).
// MbRunApp() drives ALL windows from one frame tick — advance the engine, then
// blit only dirty views — the canonical interactive loop from webview.h.
#ifndef MB_SAMPLES_MB_WINDOW_H_
#define MB_SAMPLES_MB_WINDOW_H_

#include "miniblink2/webview.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MbWindow MbWindow;

// A window hosting a fresh offscreen mbView, `width`x`height` LOGICAL (CSS) px,
// rendered at the display's scale. Call mbInitialize() before the first one.
// Windows stay alive until the user closes them; closing the last one ends
// MbRunApp. Returns NULL on failure.
MbWindow* MbWindowCreate(const char* title, int width, int height);

// The window's engine view (load into it, bind JS, etc.).
mbView* MbWindowView(MbWindow* w);

// Update the native window title (UTF-8).
void MbWindowSetTitle(MbWindow* w, const char* title);

// Activate the app, start the shared frame tick, run the native event loop
// until the last window closes. Honors MB_SAMPLE_AUTOEXIT_MS (exit 0 after
// N ms) so samples can smoke-run unattended.
void MbRunApp(void);

// ---- Browser-shaped window (sample 8) -----------------------------------------
// A window split into a fixed-height CHROME strip (its own mbView, created by
// the scaffold - render your toolbar/tab-bar HTML into it) above a swappable
// PAGE area. Input routes by position; the page area's pointer follows the
// page view's engine-pushed cursor; window resizes flow into both views'
// mbResize (and the optional resize callback, so a host can resize hidden
// tabs lazily).
typedef struct MbBrowserWindow MbBrowserWindow;
MbBrowserWindow* MbBrowserWindowCreate(const char* title, int width,
                                       int height, int chrome_height);
// The chrome strip's engine view (bind your UI callbacks, load your UI HTML).
mbView* MbBrowserWindowChrome(MbBrowserWindow* w);
// Show `page` in the page area (NULL = blank). The scaffold resizes it to the
// current page area and registers its cursor push. The host keeps ownership.
void MbBrowserWindowSetPage(MbBrowserWindow* w, mbView* page);
// Current page-area size in logical CSS px (size new tab views with this).
void MbBrowserWindowPageSize(MbBrowserWindow* w, int* out_w, int* out_h);
// Native tooltip over the page area ("" or NULL hides) - wire it to
// mbOnTooltipChanged.
void MbBrowserWindowSetTooltip(MbBrowserWindow* w, const char* utf8_text);
// Fires after a window resize with the new page-area size.
typedef void (*MbBrowserResizeFn)(MbBrowserWindow*, int page_w, int page_h,
                                  void* userdata);
void MbBrowserWindowOnResize(MbBrowserWindow* w, MbBrowserResizeFn fn,
                             void* userdata);

// Post fn(userdata) to the MAIN thread's frame tick (thread-safe; at most one
// frame of latency). For backends like sample 8's CDP socket threads, which
// must enter the engine but may not do so off-thread.
void MbPostToMain(void (*fn)(void* userdata), void* userdata);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MB_SAMPLES_MB_WINDOW_H_
