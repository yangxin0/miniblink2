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
//
// THREADING CONTRACT (applies to the whole mb* ABI unless a function says
// otherwise): the engine lives on the thread that called mbInitialize — call
// every mb* function from that thread. Callbacks fire on it too unless their
// own documentation explicitly names another sequence (notably logging and
// clipboard callbacks). The API is not thread-safe; marshal cross-thread work
// yourself (e.g. via mbDefer from the engine thread).
//
// COORDINATE/PIXEL CONTRACT: sizes and positions are LOGICAL (CSS) px unless
// stated otherwise; physical px = logical px * device scale factor
// (mbSetDeviceScaleFactor). Pixel buffers are BGRA8888, PREMULTIPLIED alpha,
// sRGB — with mbSetTransparentBackground, composite them as premultiplied
// (do not treat the channels as straight alpha). PNG exports (mbSavePng /
// mbEncodePng and friends) are the exception: the encoder converts to
// straight alpha, as the PNG format requires — those files are correct as-is.
//
// WHAT TO WIRE, BY HOST TYPE (everything else is optional; all rows are
// per-view unless marked process-wide):
//
//   |                          | screenshot/scrape host | interactive host |
//   |--------------------------|------------------------|------------------|
//   | mbInitialize             | required               | required         |
//   | mbPumpMessages / waits   | required               | avoid (use ↓)    |
//   | mbUpdate / mbUpdateAt    | not needed             | required         |
//   | mbSetMaxUpdateTime       | not needed             | recommended      |
//   | mbRepaintToBitmap+IsDirty| not needed             | required         |
//   | mbPaintToBitmap / SavePng| required               | avoid (settles)  |
//   | input (mbSend*)          | optional (selectors)   | required         |
//   | mbOnCursor/TooltipChanged| not needed             | recommended      |
//   | mbOnSelectPopup          | not needed             | required         |
//   | mbSetJsDialogCallback    | recommended            | recommended      |
//   | mbOnLoadFinish etc.      | recommended            | recommended      |
//
// INTERACTIVE FRAME TICK — the canonical host loop (a display-link/vsync
// callback; error handling elided):
//
//   // once, at startup:
//   mbInitialize();
//   mbSetMaxUpdateTime(0.008);              // 8 ms engine budget per tick
//   mbView* v = mbCreateView(w, h);
//   mbLoadURL(v, "https://example.org");
//
//   // every vsync tick (timestamp in the CADisplayLink/mach domain):
//   mbUpdateAt(frame_time);                 // advance the world (never nests)
//   if (mbViewIsDirty(v)) {                 // damage-gated: skip clean frames
//     if (mbRepaintToBitmap(v, buf, w*dsf, h*dsf, w*dsf*4))
//       blit(buf);                          // premultiplied BGRA, sRGB
//   }
//
// Keep "advance the world" (mbUpdate*) and "snapshot pixels" (repaint)
// separate; never call the settling one-shot capture calls from this loop.

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
//
// Engine (this library) version, e.g. "0.5.0". MB_VERSION is the compile-time
// string this header shipped with — log both (compiled-against vs loaded) when
// diagnosing a dlopen'd engine. It is DERIVED from the numeric components so the
// string and the #if-checkable numbers (e.g. #if MB_VERSION_MINOR >= 5) can never
// drift apart — there is one source of truth, not a hand-kept duplicate.
#define MB_VERSION_MAJOR 0
#define MB_VERSION_MINOR 5
#define MB_VERSION_PATCH 0
#define MB_STRINGIZE_(x) #x
#define MB_STRINGIZE(x) MB_STRINGIZE_(x)
#define MB_VERSION                                                       \
  MB_STRINGIZE(MB_VERSION_MAJOR)                                         \
  "." MB_STRINGIZE(MB_VERSION_MINOR) "." MB_STRINGIZE(MB_VERSION_PATCH)
MB_EXPORT const char* mbVersion(void);

// Compatibility is TWO independent axes, not one overloaded number (the old
// "refuse an engine reporting < N" rule was wrong in both directions: a NEWER
// number can mean a BREAKING ABI, and two builds sharing a number can still
// differ in which symbols exist):
//   MB_ABI_EPOCH  — bumped on any BINARY-INCOMPATIBLE change (a field reordered/
//                   removed, or an existing exported signature/calling convention
//                   changed). Host and engine are ABI-compatible ONLY when their
//                   epochs are EQUAL. A higher epoch is not "more compatible" — it
//                   is a different binary ABI.
//   MB_API_LEVEL  — bumped on any purely ADDITIVE change (new function/enum/flag).
//                   Monotonic: an engine of level N provides every symbol a header
//                   of level <= N declares, so a host needs engine level >= its
//                   own build level for the symbols it references to resolve.
#define MB_ABI_EPOCH 1
// PRE-RELEASE: the project has not shipped, so neither number has ever been
// consumed by an external binary and no compatibility is owed across commits —
// entry points change in place. The two-axis scheme above is the contract the
// FIRST release will freeze; until then both stay at 1.
#define MB_API_LEVEL 1
MB_EXPORT int mbAbiEpoch(void);
MB_EXPORT int mbApiLevel(void);
// Original name for the additive level; kept as an alias so existing callers and
// the value MB_API_VERSION still compile and mean exactly mbApiLevel().
#define MB_API_VERSION MB_API_LEVEL
MB_EXPORT int mbApiVersion(void);

// One-call handshake for a dlopen-ing host: returns 1 iff the loaded engine is
// safe to use against the header this host compiled with — the SAME ABI epoch AND
// an API level AT LEAST the host's. Call it right after dlopen with the header's
// own MB_ABI_EPOCH / MB_API_LEVEL:
//   if (!mbCheckCompat(MB_ABI_EPOCH, MB_API_LEVEL)) { /* refuse this engine */ }
// A 0 means refuse rather than risk a mismatched-ABI crash or a missing-symbol
// failure at the first newer call.
MB_EXPORT int mbCheckCompat(int abi_epoch, int api_level);

// Feature capability probe. Some symbols exist in every build but only DO
// something when the engine was compiled with the matching GN flag (e.g.
// navigator.gpu is always present but returns null without --webgpu). Returns 1
// if `feature` is compiled into THIS engine, else 0 (also 0 for an unknown name),
// so a host lights up optional behavior from the actual capability instead of
// inferring it from a failed call. Recognized names: "webgpu", "wasm", "webrtc".
MB_EXPORT int mbHasFeature(const char* feature);

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
// Destroy every view before the host stops pumping the engine loop; mbShutdown neither
// drains pending view work nor tears views down. It does release any worker thread
// still parked on an engine-thread rendezvous (its fetch fails as blocked), so a host
// that calls mbShutdown before stopping its loop cannot strand worker threads.
MB_EXPORT int  mbInitialize(void);
MB_EXPORT void mbShutdown(void);

// Register a CUSTOM URL SCHEME (e.g. "app") the host serves itself: parsed as
// a STANDARD URL (host/path/origin semantics, relative resolution), treated as
// a secure context, fetch()/XHR-capable — and served EXCLUSIVELY through the
// interception stack (mbMockResponse, mbSetRequestMockCallback,
// mbOnRequestMock), never the network. The missing piece of a virtual-
// filesystem story: pages live at app://assets/index.html and every request
// under it lands in your mock callback. Unmocked custom-scheme requests fail
// cleanly. MUST be called BEFORE mbInitialize (scheme registries are consulted
// at engine bring-up); scheme is [A-Za-z][A-Za-z0-9+.-]*, case-insensitive.
// Cross-origin use (fetching app:// from an https page) is not supported.
MB_EXPORT void mbRegisterCustomScheme(const char* scheme);

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
// RECOMMENDED for interactive hosts: set a budget of roughly half your frame
// period (0.008 at 60 fps — what the Glyph host runs); the drain-to-idle
// default is right only for hosts that tick outside a latency-sensitive loop.
MB_EXPORT void mbSetMaxUpdateTime(double seconds);

// mbUpdate with the host's display-refresh timestamp (seconds, in the
// CACurrentMediaTime / mach monotonic domain - a CADisplayLink or
// CVDisplayLink timestamp passes straight through). requestAnimationFrame
// callbacks are stamped with THIS time instead of whenever the pump ran, so
// animation-driven content advances frame-aligned. Behaves exactly like
// mbUpdate otherwise (re-entrancy-safe, budget-bounded).
MB_EXPORT void mbUpdateAt(double frame_time_seconds);

// Per-display ticking for multi-monitor hosts: rAF timestamps for THIS view
// come from `frame_time_seconds` (same CACurrentMediaTime domain as
// mbUpdateAt) instead of the process-global mbUpdateAt time. Stamp each view
// from ITS display's vsync callback — a view on a 120 Hz panel and one on a
// 60 Hz panel then advance their animations on their own display's cadence,
// where the global time alone would stamp both with whichever callback ran
// last. The timestamp applies to the view's following repaints (rAF runs
// during the paint drive tick); <= 0 clears the override (back to
// mbUpdateAt's time, then wall clock). Single-display hosts don't need this.
//
//   // per display D, in D's CADisplayLink callback:
//   mbUpdateAt(ts_D);                        // run ready engine work once
//   for (view on D) {
//     mbViewSetFrameTime(view, ts_D);        // this view ticks in D's domain
//     if (mbViewIsDirty(view)) { mbRepaintToBitmap(...); /* blit dirty rect */ }
//   }
MB_EXPORT void mbViewSetFrameTime(mbView*, double frame_time_seconds);

// Release as much memory as possible: broadcasts critical memory pressure
// (blink's caches, decoded images and fonts listen) and triggers a full V8
// GC. Call when the host UI goes hidden/idle; content re-decodes lazily on
// the next paint.
MB_EXPORT void mbPurgeMemory(void);

// Log a coarse memory summary (V8 heap, malloc footprint) to stderr —
// before/after bookends for mbPurgeMemory.
MB_EXPORT void mbLogMemoryUsage(void);

// ---- Memory budget knobs -----------------------------------------------------
// mbPurgeMemory shrinks AFTER the fact; these CAP the steady-state footprint —
// for hosts (menu-bar apps, kiosks) that pick an embedded engine precisely for
// its envelope. All process-wide.
// Cap the decoded-image cache (the biggest steady-state consumer on image-heavy
// pages). 0 restores the engine default (32 MB). Call after mbInitialize.
MB_EXPORT void mbSetImageCacheSize(size_t bytes);
// Cap the rasterized-glyph (font) cache. 0 restores the engine default (2 MB).
// Call after mbInitialize.
MB_EXPORT void mbSetFontCacheSize(size_t bytes);
// Cap the JS heap (V8 old generation) of every isolate. MUST be called BEFORE
// mbInitialize — it is an isolate-creation parameter; afterwards the call is
// refused with a logged warning. 0 = engine default (sized from machine RAM).
// A page whose live JS exceeds the cap gets a JS out-of-memory, not unbounded
// growth.
MB_EXPORT void mbSetJsHeapLimit(size_t bytes);

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

// Run when the engine is next OFF the stack: immediately if it
// already is, otherwise from the tail of the next mbUpdate. Use for work that
// must ENTER the engine (create a view, start a load) but was scheduled from a
// callback firing inside an engine call — calling in directly from there would
// re-enter blink. Engine-side replacement for host-rolled deferral queues.
typedef void (*mbDeferredCallback)(void* userdata);
MB_EXPORT void mbDefer(mbDeferredCallback cb, void* userdata);

// View lifecycle. A view owns one WebView + main LocalFrame + WebFrameWidget.
// ---- Sessions (browsing profiles) -------------------------------------------
// A session isolates cookies and origin-keyed state (local/sessionStorage,
// IndexedDB, OPFS, storage buckets, locks) per profile. Sessions are
// capability handles: whoever holds the mbSession* controls the profile.
// persist_path == NULL or empty means EPHEMERAL (memory only). Otherwise the
// profile roots at <persist_path>/<name>/. The full directory is canonicalized
// and carries a stable identity marker, so relative, symlink, case, and Unicode-
// normalization aliases resolve to one profile while a copied directory becomes
// an independent clone. The mode is fixed for the session's life.
//
// A persistent `name` must be a portable single path component: NULL/empty, ".",
// "..", path separators, control chars, the Windows-illegal characters (: * ? " < > |), a
// trailing dot/space, or a Windows reserved device name (CON/PRN/NUL/COM1.../LPT1...) are
// rejected. An ephemeral session ignores `name` for storage and accepts anything
// (NULL/empty becomes "unnamed").
typedef struct mbSession mbSession;
// Create a session. Returns NULL on failure (invalid persistent name,
// engine not initialized, unwritable profile directory).
MB_EXPORT mbSession* mbCreateSession(const char* name, const char* persist_path);
// Like mbCreateSession but reports WHY creation failed. On failure returns
// NULL and, if out_status != NULL, writes an mbSessionCreateStatus: MB_SESSION_INVALID_NAME
// for a non-portable persistent profile name (see above), MB_SESSION_ERROR for
// engine-not-initialized / other. MB_SESSION_OK (0) on success.
// Kept int-compatible so both `int*` callers of the draft API and callers using
// the named status typedef compile. The C ABI remains an int-sized status pointer.
typedef int mbSessionCreateStatus;
enum {
  MB_SESSION_OK = 0,
  MB_SESSION_INVALID_NAME = 1,
  MB_SESSION_ERROR = 2
};
MB_EXPORT mbSession* mbCreateSessionEx(const char* name, const char* persist_path,
                                       mbSessionCreateStatus* out_status);
// Safe with views (or a view-config) still referencing this session: the handle
// is reference-counted, so mbDestroySession only marks it for teardown. The
// profile — and this handle — are torn down when the last view and view-config
// referencing it are destroyed, and until then mbViewGetSession keeps returning a
// valid handle. After you call mbDestroySession you must not use the handle again
// yourself (a view may outlive your reference, but that is the engine's to track).
MB_EXPORT void mbDestroySession(mbSession*);
// The implicit shared EPHEMERAL profile plain mbCreateView uses. Never
// destroy it.
MB_EXPORT mbSession* mbDefaultSession(void);
// Create a view inside `session` (falls back to the default session if NULL).
// The profile is bound during construction, before Blink creates the Page or
// any frame/worker, and is immutable for the view's lifetime.
MB_EXPORT mbView* mbCreateViewInSession(int width, int height, mbSession*);
MB_EXPORT mbSession* mbViewGetSession(mbView*);
// Wipe this profile's cookie jar/page-cookie mirror, local/sessionStorage
// backend, IndexedDB, and OPFS. Live localStorage caches are invalidated.
// Blink cannot invalidate an already-live sessionStorage cache from its backend,
// so such a document may retain cached entries until it navigates or is destroyed;
// use mbClearStorage for immediate per-document clearing when that matters.
MB_EXPORT void mbSessionClearStorage(mbSession*);
// Durability barrier: write a PERSISTENT profile cookies/IndexedDB/OPFS
// under its persist dir now (also happens at final teardown). DOM storage is
// currently memory-only. No-op for ephemeral profiles.
MB_EXPORT void mbSessionFlush(mbSession*);
// Extended form: like mbSessionFlush but returns 1 on success (including
// the no-op flush of an ephemeral profile — nothing to persist), 0 if the profile
// directory or any store could not be written (permissions, full disk), so a host can
// surface a failed save instead of silently losing data. Added alongside — NOT changing —
// mbSessionFlush's void ABI.
MB_EXPORT int mbSessionFlushEx(mbSession*);
// Introspection — a host juggling several handles can ask which is which.
// mbSessionGetName / mbSessionGetPersistPath write into out (NUL-terminated,
// truncated to out_cap; size first with out=NULL) and return the full length.
// The persist path is the profile's root directory ("<persist_path>/<name>");
// empty (returns 0) for an ephemeral profile. mbSessionIsPersistent returns 1
// for a disk-backed profile, 0 for ephemeral.
MB_EXPORT int mbSessionGetName(mbSession*, char* out, int out_cap);
MB_EXPORT int mbSessionIsPersistent(mbSession*);
MB_EXPORT int mbSessionGetPersistPath(mbSession*, char* out, int out_cap);

MB_EXPORT mbView* mbCreateView(int width, int height);
MB_EXPORT void    mbDestroyView(mbView*);
MB_EXPORT void    mbResize(mbView*, int width, int height);

// ---- Creation-time view config ------------------------------------------------
// The one-call answer to the "call this before the first load" / "affects the
// NEXT mbCreateView" timing footguns: collect creation-time choices in an
// opaque config object, then create the view with all of them applied before
// any document exists. An OPAQUE BUILDER (create → set fields → create view →
// destroy), not a struct: new setters can be added without touching any
// signature or ABI. Unset fields keep the engine defaults. The config is
// reusable (create several views from one) and is yours to destroy; destroying
// it does not affect views created from it. The per-field runtime setters keep
// working after creation — this is the primary path, not the only one.
typedef struct mbViewConfig mbViewConfig;
MB_EXPORT mbViewConfig* mbCreateViewConfig(void);
MB_EXPORT void mbDestroyViewConfig(mbViewConfig*);
// The session (browsing profile) the view is created into (NULL = default) —
// same effect as mbCreateViewInSession.
MB_EXPORT void mbViewConfigSetSession(mbViewConfig*, mbSession*);
// Per-view compositing at creation (the widget attach is creation-fixed).
// Replaces the process-global mbSetCompositingEnabled latch for this path.
MB_EXPORT void mbViewConfigSetCompositing(mbViewConfig*, int on);
// The rest mirror the runtime setters, applied before the first document:
MB_EXPORT void mbViewConfigSetTransparentBackground(mbViewConfig*, int transparent);
MB_EXPORT void mbViewConfigSetDeviceScaleFactor(mbViewConfig*, float scale);
MB_EXPORT void mbViewConfigSetEnableJavascript(mbViewConfig*, int enabled);
MB_EXPORT void mbViewConfigSetLoadImages(mbViewConfig*, int enabled);
MB_EXPORT void mbViewConfigSetDarkMode(mbViewConfig*, int dark);
MB_EXPORT void mbViewConfigSetUserAgent(mbViewConfig*, const char* utf8_ua);
MB_EXPORT void mbViewConfigSetLocale(mbViewConfig*, const char* utf8_languages);
MB_EXPORT void mbViewConfigSetFontFamilies(mbViewConfig*, const char* standard,
                                           const char* fixed, const char* serif,
                                           const char* sans_serif);
// Create a view with `config` applied (NULL config == plain mbCreateView).
MB_EXPORT mbView* mbCreateViewWithConfig(int width, int height,
                                         const mbViewConfig*);

// Compositing (experimental). When enabled, the NEXT mbCreateView attaches its widget
// COMPOSITING — blink drives cc into an in-process software viz::Display — instead of the
// default non-compositing software-paint path. Process-global; default OFF (screenshots
// are unchanged). LEGACY LATCH: prefer mbViewConfigSetCompositing +
// mbCreateViewWithConfig, which makes the choice per-view at the call site
// instead of via process state. mbViewFrameSinkRequested returns how many frame sinks
// blink's compositor has pulled (>0 once the view is shown + the message loop pumped),
// or -1 if the view is not compositing.
MB_EXPORT void    mbSetCompositingEnabled(int on);
MB_EXPORT int     mbViewFrameSinkRequested(mbView*);
// Drive one synchronous compositor frame (compositing views only; no-op otherwise) — a headless
// compositing widget has no browser begin-frame source, so frames are pulled explicitly.
MB_EXPORT void    mbViewComposite(mbView*);
// The SkColor (0xAARRGGBB) at (x,y) in the compositor's last captured frame, or 0 if not
// compositing / nothing composited. For verifying the cc -> viz -> bitmap path.
MB_EXPORT unsigned int mbViewCompositorPixel(mbView*, int x, int y);
// Shared-texture output (macOS, compositing views): the IOSurfaceRef the
// in-process display renders composited frames into. Bind it as a CALayer's
// `contents` and the host displays the frame with ZERO CPU readback — the
// composited replacement for the mbRepaintToBitmap copy path:
//
//   mbViewComposite(v);
//   layer.contents = nil;                       // poke CA to re-sample
//   layer.contents = (__bridge id)mbViewGetIOSurface(v);
//
// Returns NULL when the view is not compositing, nothing composited yet, not
// macOS, or IOSurface allocation fell back to heap memory (then use
// mbRepaintToBitmap). The surface stays valid until the next mbResize or
// mbDestroyView — CFRetain it to hold on longer. The engine writes the surface
// during mbViewComposite only (frames are host-pulled), so a host that
// composites-then-assigns on its own tick never races the writer.
//
// (The NON-composited software path already achieves zero extra copies without
// this: mbRepaintToBitmap paints straight into caller-owned memory — point it
// at your own IOSurface's base address and blit only mbViewGetDirtyRect.)
MB_EXPORT void* mbViewGetIOSurface(mbView*);

// Content entry points.
//   mbLoadHTML  — render an in-memory document (no network). First render proof.
//   mbLoadURL   — fetch via libcurl (mb_url_loader) then render (SYNCHRONOUS: blocks the
//                 calling thread until the fetch completes; simplest for automation).
//   mbNavigate  — ASYNCHRONOUS navigation: returns a navigation id
//                 immediately and fetches off the main thread, so an interactive host stays
//                 responsive and can repaint / cancel during a slow load. Prefer it for
//                 interactive UIs. Mock lookup is posted too; file IO and data decoding
//                 run on the background pool, so local responses neither materialize
//                 inside mbNavigate nor freeze the next engine update. The
//                 simple main-frame lifecycle is: started (mbOnNavigationStarted)
//                 -> begin (mbOnBeginLoading, on commit) -> finish
//                 (mbOnLoadFinish) / fail (mbOnFailLoading). Exactly one finish fires per
//                 navigation, including on failure, cancellation, supersession, and a
//                 download-diverted load. Starting a new navigation (mbNavigate OR
//                 mbLoadURL/mbPostURL) supersedes an in-flight one.
MB_EXPORT void mbLoadHTML(mbView*, const char* utf8_html, const char* base_url);
MB_EXPORT void mbLoadURL(mbView*, const char* utf8_url);

typedef uint64_t mbNavigationId;
MB_EXPORT mbNavigationId mbNavigate(mbView*, const char* utf8_url);

// Async navigation with an explicit method/body (POST etc.) — the non-blocking counterpart
// of mbPostURL. Set struct_size = sizeof(mbNavigationOptions); the engine reads only
// fields wholly contained in that prefix, so future appended fields remain compatible.
// `method` NULL means GET (or POST when a body is present); `body`/`body_len` is the
// request body (may contain NULs); `content_type` is its Content-Type. `opts` NULL
// behaves like mbNavigate (GET).
typedef struct mbNavigationOptions {
  int struct_size;
  const char* method;       // NULL -> GET, or POST if a body is present
  const void* body;
  size_t body_len;
  const char* content_type;
} mbNavigationOptions;
MB_EXPORT mbNavigationId mbNavigateEx(mbView*, const char* utf8_url,
                                      const mbNavigationOptions* opts);
// Cancel an in-flight navigation by the id mbNavigate returned: aborts its network I/O
// promptly and guarantees it never commits (its finish fires with a "cancelled" fail).
// Returns 1 if `id` was the active navigation and was cancelled, 0 otherwise (already
// finished / superseded / unknown id).
MB_EXPORT int mbCancelNavigation(mbView*, mbNavigationId);

// Structured main-frame navigation lifecycle: the correlation-safe
// counterpart to the simple mbOnNavigationStarted /
// mbOnBeginLoading / mbOnLoadFinish / mbOnFailLoading callbacks. `navigation_id`
// is the exact non-zero id returned by mbNavigate/mbNavigateEx; navigations begun
// by another API or by the page report 0. For mbNavigate*, the id is reserved
// BEFORE the STARTED callback, so callback code can record or cancel that exact
// navigation even though mbNavigate has not returned yet.
//
// When the same callback registration is present at STARTED and retained while
// the view remains alive, that registration receives one STARTED event, zero or
// one COMMITTED event, and exactly one TERMINAL event. Registering or replacing
// the callback mid-navigation may therefore observe only the remaining phases.
// A pre-commit failure/cancellation/supersession or download diversion has no
// COMMITTED event. `outcome` is NONE until TERMINAL.
// `requested_url` is the original target; `url` is the best-known current URL
// (the committed/final URL after a redirect). All strings and the event struct
// itself are borrowed and valid only for the duration of the callback.
typedef int mbNavigationPhase;
enum {
  MB_NAVIGATION_PHASE_STARTED = 0,
  MB_NAVIGATION_PHASE_COMMITTED = 1,
  MB_NAVIGATION_PHASE_TERMINAL = 2
};

typedef int mbNavigationOutcome;
enum {
  MB_NAVIGATION_OUTCOME_NONE = 0,
  MB_NAVIGATION_OUTCOME_SUCCESS = 1,
  MB_NAVIGATION_OUTCOME_FAILURE = 2,
  MB_NAVIGATION_OUTCOME_CANCELLED = 3,
  MB_NAVIGATION_OUTCOME_SUPERSEDED = 4,
  MB_NAVIGATION_OUTCOME_DOWNLOAD = 5
};

typedef struct mbNavigationEvent {
  int struct_size;  // sizeof(mbNavigationEvent) in this engine
  mbNavigationId navigation_id;
  mbNavigationPhase phase;
  mbNavigationOutcome outcome;
  const char* requested_url;
  const char* url;
  int http_status;           // 0 when no HTTP response/status is available
  const char* error_domain;  // set only on failed/cancelled/superseded terminals
  int error_code;            // CURLcode for domain "curl"; otherwise 0
  const char* description;   // human-readable terminal error; may be empty
} mbNavigationEvent;
typedef void (*mbNavigationEventCallback)(mbView*, void* userdata,
                                          const mbNavigationEvent* event);
MB_EXPORT void mbOnNavigationEvent(mbView*, mbNavigationEventCallback,
                                   void* userdata);

// mbLoadHTML with explicit history control. add_to_history=1 behaves exactly
// like mbLoadHTML (the load appends a back/forward entry). add_to_history=0
// REPLACES the current entry instead (location.replace semantics) — a host
// cycling in-memory documents (e.g. dictionary entries per hover) doesn't
// pollute the back/forward list with every variant it shows.
MB_EXPORT void mbLoadHTMLEx(mbView*, const char* utf8_html,
                            const char* base_url, int add_to_history);

// Uncorrelated completion notification. For a document that commits, this
// is the real Blink DidFinishLoad signal (the main document's `load` event, all
// subresources done), not a poll or fixed timer. A navigation that terminates
// WITHOUT a document (failure, cancellation, supersession, or download diversion)
// also gets one synthesized callback so uncorrelated waiters cannot hang. Therefore this
// callback means "the current attempt ended", NOT "the page loaded successfully".
// Use mbOnNavigationEvent when outcome or mbNavigationId correlation matters.
// Register with mbOnLoadFinish (NULL clears); synchronous loads may invoke it before
// their load call returns. mbIsLoadFinished queries the same state (1 after a
// terminal event, 0 after a new load starts).
typedef void (*mbLoadFinishCallback)(mbView*, void* userdata);
MB_EXPORT void mbOnLoadFinish(mbView*, mbLoadFinishCallback, void* userdata);

// Fires when a top-level navigation is KICKED OFF — at the very start of
// mbLoadURL / mbPostURL, BEFORE the (possibly slow) main-resource fetch. This is
// the "show a spinner now" signal: mbOnBeginLoading only fires once the new
// document COMMITS (after the fetch returns), which for a slow server is too late
// to start loading UI. `url` is the REQUESTED URL (pre-redirect), valid only for
// the duration of the call. Per view. This legacy callback has no mbNavigationId;
// use mbOnNavigationEvent for correlation. Pair with mbOnBeginLoading (commit)
// and mbOnLoadFinish (end): started -> begin(commit) -> finish/fail.
typedef void (*mbNavigationStartedCallback)(mbView*, void* userdata, const char* url);
MB_EXPORT void mbOnNavigationStarted(mbView*, mbNavigationStartedCallback,
                                     void* userdata);

// Fires when a MAIN-FRAME navigation commits: the previous document is gone
// and the new one starts loading subresources. Completes the lifecycle set
// (begin -> DOMContentLoaded -> finish, or begin -> fail). `url` is the
// committed document URL, valid only for the duration of the call.
typedef void (*mbBeginLoadingCallback)(mbView*, void* userdata, const char* url);
MB_EXPORT void mbOnBeginLoading(mbView*, mbBeginLoadingCallback, void* userdata);

// Fires when a TOP-LEVEL load ends without producing a document (file unreadable,
// fetch error, explicit cancellation, or supersession). `url` is page-visible:
// transparent mbRewriteUrl/mbRequestSetUrl transport targets are never exposed.
// `error` is a short description (may be empty). The load-finish callback still
// fires right after — mbIsLoadFinished stays the single completion signal; this
// adds the reason.
typedef void (*mbFailLoadingCallback)(mbView*, void* userdata, const char* url,
                                      const char* error);
MB_EXPORT void mbOnFailLoading(mbView*, mbFailLoadingCallback, void* userdata);
MB_EXPORT int  mbIsLoadFinished(mbView*);

// Structured fail-loading variant: like mbOnFailLoading but machine-checkable —
// `error_domain` is "curl" (transport failure; `error_code` is the CURLcode,
// e.g. 6 couldn't-resolve-host, 7 couldn't-connect, 28 timeout), "file"
// (file:// unreadable), "network" (no response at all), "blocked" (the
// mbSetRequestHook callback vetoed the load), or "cancelled" (explicit cancel
// OR superseded by a newer navigation); `error_code` is 0 outside the curl
// domain. mbOnNavigationEvent distinguishes CANCELLED from SUPERSEDED.
// `description` is the prose mbOnFailLoading carries.
// Branch on domain+code (retry timeouts, report DNS) instead of matching
// English. ONE SLOT with mbOnFailLoading: setting either replaces the other.
// NULL clears. HTTP 4xx/5xx are NOT failures (they commit; see mbGetHttpStatus).
typedef void (*mbFailLoadingCallbackEx)(mbView*, void* userdata, const char* url,
                                        const char* error_domain, int error_code,
                                        const char* description);
MB_EXPORT void mbOnFailLoadingEx(mbView*, mbFailLoadingCallbackEx, void* userdata);

// Fires when the main document's DOMContentLoaded event dispatches — the DOM is parsed and
// deferred scripts have run, but subresources/images may still be loading. This is the
// "page interactive" signal and is EARLIER than mbOnLoadFinish (which waits for `load` /
// all subresources), matching Puppeteer/Playwright's 'domcontentloaded' wait. NULL clears.
typedef void (*mbDOMContentLoadedCallback)(mbView*, void* userdata);
MB_EXPORT void mbOnDOMContentLoaded(mbView*, mbDOMContentLoadedCallback, void* userdata);

// Fires for each new MAIN-FRAME document at the earliest scriptable moment: the
// window object exists, the init script (mbSetInitScript) has run, and NO page
// script has executed yet — earlier than mbOnDOMContentLoaded (DOM parsed) and
// the sanctioned point for host-COMPUTED per-document setup (fresh tokens,
// state not known until now): mbRunJS/mbEvalJS from inside the callback run
// before the page sees the world. For static setup prefer mbSetInitScript.
// Keep it cheap; it fires from inside document initialization. NULL clears.
typedef void (*mbWindowObjectReadyCallback)(mbView*, void* userdata);
MB_EXPORT void mbOnWindowObjectReady(mbView*, mbWindowObjectReadyCallback,
                                     void* userdata);

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

// Streaming download lifecycle — the large-body alternative to mbOnDownload's
// complete-buffer callback. One download runs begin -> data... -> finish:
//   begin(id, url, mime, filename, expected_bytes)   response started;
//       expected_bytes is the body total from Content-Length, -1 if unknown
//       (chunked). filename: the page's suggested name, else the server's
//       Content-Disposition, else the URL's last segment.
//   data(id, bytes, len, received, expected_bytes)   one chunk, in order;
//       `received` accumulates. NOT NUL-safe — use len. The engine buffers
//       nothing: write chunks to disk yourself.
//   finish(id, success)                              exactly once; success=0
//       on HTTP >= 400, transport failure, or mbCancelDownload.
// `id` identifies one download (unique per view). Callbacks arrive as engine
// tasks — an mbUpdate tick delivers them (never from inside mbOnDownloadStream
// / mbDownloadURLStream themselves).
//
// While registered, this sink TAKES OVER all page-initiated downloads from
// mbOnDownload (which stops firing): navigation downloads and blob:/data:
// bodies (already materialized in memory) deliver as begin + one data +
// finish; an http(s) <a download> link genuinely streams. Passing all-NULL
// callbacks clears the sink and restores mbOnDownload routing.
typedef void (*mbDownloadBeginCallback)(mbView*, void* userdata,
                                        unsigned int id, const char* url,
                                        const char* mime, const char* filename,
                                        long long expected_bytes);
typedef void (*mbDownloadDataCallback)(mbView*, void* userdata,
                                       unsigned int id, const char* bytes,
                                       int len, long long received,
                                       long long expected_bytes);
typedef void (*mbDownloadFinishCallback)(mbView*, void* userdata,
                                         unsigned int id, int success);
MB_EXPORT void mbOnDownloadStream(mbView*, mbDownloadBeginCallback,
                                  mbDownloadDataCallback,
                                  mbDownloadFinishCallback, void* userdata);

// Host-initiated streaming download of `url` through the engine network stack
// (interception layer, session cookies, UA, extra headers, proxy). Returns the
// download id (> 0) whose lifecycle arrives on the mbOnDownloadStream sink, or
// 0 when refused (no sink registered, blocked/vetoed URL, unfetchable scheme).
// Unlike mbDownloadURL the engine never buffers the whole body — suitable for
// files larger than memory.
MB_EXPORT unsigned int mbDownloadURLStream(mbView*, const char* url);

// Abort a running download promptly (mid-transfer, or from inside its own
// begin/data callbacks); its finish callback then reports success=0. Unknown
// ids are ignored.
MB_EXPORT void mbCancelDownload(mbView*, unsigned int id);

// New-window notification: the callback fires when the page requests a new window
// (window.open() or a target=_blank activation), with the requested `url` and window
// `name`. It is a notification only — the popup is not auto-created (window.open returns
// null), so an embedder can decide what to do (e.g. load `url` in this or a new view).
// NULL clears it.
typedef void (*mbNewWindowCallback)(mbView*, void* userdata, const char* url,
                                    const char* name);
MB_EXPORT void mbOnNewWindow(mbView*, mbNewWindowCallback, void* userdata);

// Child views: make window.open() actually WORK (vs mbOnNewWindow, which only
// observes). When registered, a page-initiated window request creates a real
// child view wired as the opener's child — window.open returns a live window
// object, the opener/postMessage relationship works (OAuth popups that post
// back, pages that drive their popup) — and the callback delivers it:
//   `child`      a fully functional mbView the HOST now owns. Return 1 to
//                ADOPT it (size it, show it, and mbDestroyView it when done;
//                it inherits the parent's session). Return 0 to DECLINE —
//                the engine destroys it and the page sees window.open == null.
//   `url`/`name` the requested URL (loads into the child right after the
//                callback returns) and window name.
//   `is_popup`   1 for a popup/new-window disposition, 0 for a tab-like one.
//   `x,y,width,height` the window.open features geometry; 0 when unspecified
//                (the child defaults to the parent's size — mbResize it).
// The callback fires from INSIDE the page's JS (window.open is synchronous):
// keep it cheap, don't pump, defer heavy host work (mbDefer). The OPENER's
// URL is mbGetURL(parent) — the parent is the view whose page called
// window.open, so no separate opener_url parameter is needed. Registering
// this takes precedence over mbOnNewWindow for the views it adopts; declined
// requests still fire mbOnNewWindow. NULL clears (window.open returns null
// again). LIFETIME: the child shares the opener's JS agent cluster — destroy
// the child BEFORE its parent view.
typedef int (*mbCreateChildViewCallback)(mbView* parent, void* userdata,
                                         mbView* child, const char* url,
                                         const char* name, int is_popup,
                                         int x, int y, int width, int height);
MB_EXPORT void mbOnCreateChildView(mbView*, mbCreateChildViewCallback,
                                   void* userdata);

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

// Register an in-memory font (TTF/OTF bytes) with the platform font system at
// PROCESS scope — the third piece of the font story: mbSetFontFamilies names
// families, the fallback callback answers per character, and this SERVES the
// bytes, so a host can bundle a font (a guaranteed CJK face for a dictionary)
// instead of depending on the user's installed library. On success returns 1
// and writes the face's family name (UTF-8, NUL-terminated, truncated to
// family_cap; out_family may be NULL) — use that exact name in
// mbSetFontFamilies / the fallback callback / CSS. The bytes are COPIED; free
// yours after the call. Register before loading content (a committed document
// re-resolves on the next style recalc, but do not rely on it). Registering
// the same face twice is a success no-op. PLATFORMS: macOS today; on Windows
// this returns 0 (blink's DirectWrite stack cannot see memory-registered GDI
// fonts; a private DirectWrite collection is planned).
MB_EXPORT int mbAddFontData(const void* data, int len, char* out_family,
                            int family_cap);

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

// In-engine DevTools endpoint — the one-call alternative to bridging the raw
// mbDevToolsAttach pipe to a socket yourself (item 41). Opens a LOOPBACK-ONLY
// (127.0.0.1) HTTP+WebSocket CDP server on `port`: serves /json/version and
// /json/list (one target per live view) and a WebSocket per view bridging to
// the SAME per-view session mbDevToolsAttach uses (so one client per view — a
// view already attached via mbDevToolsAttach or another socket refuses the new
// one). Point Chrome at the printed devtoolsFrontendUrl, or
// devtools://devtools/bundled/inspector.html?ws=127.0.0.1:<port>/devtools/page/<id>.
// Call AFTER mbInitialize, on the engine thread; the host must keep pumping
// mbUpdate for CDP messages to flow (the bridge rides the task queue). OFF
// unless started; the embedder WebSocket bridge remains fully supported.
// Returns 1 on success (or already running on `port`), 0 on bind/listen
// failure. Cross-platform (BSD sockets on macOS/Linux, Winsock on Windows).
// mbDevToolsStopServer closes all clients + the listener; mbDevToolsServerPort
// returns the live port, 0 when stopped.
MB_EXPORT int  mbDevToolsStartServer(int port);
MB_EXPORT void mbDevToolsStopServer(void);
MB_EXPORT int  mbDevToolsServerPort(void);

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
//
// ABI evolution is PREFIX-STABLE: new fields are only ever APPENDED, and the
// engine accepts any struct at least as large as the prefix it needs, treating
// fields your (older, smaller) struct doesn't carry as their zero default. So a
// program built against an older header keeps working against a newer engine —
// its smaller struct_size is honored, not rejected. (Conversely a newer caller
// on an older engine is fine too: the engine simply ignores trailing fields it
// doesn't know.) Always set struct_size = sizeof(the struct) and never reorder
// or resize existing fields.

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

// Typed keyboard event — the lossless path for forwarding REAL host key events
// (vs the mbSendKey* name-based shorthands, which can't express auto-repeat,
// keypad keys, or down-without-up). Field mapping from a macOS NSEvent:
//   type            keyDown -> MB_KEY_RAW_DOWN then (if it produces text and no
//                   cmd/ctrl is held) MB_KEY_CHAR; keyUp -> MB_KEY_UP
//   modifiers       from modifierFlags: control->1, shift->2, option->4,
//                   command->8
//   windows_key_code the Windows VK code for the key (map from keyCode; e.g.
//                   kVK_ANSI_A -> 'A', kVK_Return -> 0x0D) — drives keyCode and
//                   default actions
//   native_key_code the platform scan code (NSEvent.keyCode), informational
//   text            NSEvent.characters (UTF-8); unmodified_text
//                   NSEvent.charactersIgnoringModifiers — used for shortcut
//                   resolution; both may be NULL for non-text keys
//   is_keypad       modifierFlags contains numericPad
//   is_auto_repeat  NSEvent.isARepeat
#define MB_KEY_RAW_DOWN 0  /* key press, no char yet (WebKit RawKeyDown) */
#define MB_KEY_DOWN     1  /* press + implicit char (single-event style).
                              WARNING: prefer RAW_DOWN + CHAR when forwarding
                              real host key events — DOWN exists for callers
                              synthesizing a whole press in one call and can't
                              represent a key that produces no text correctly
                              alongside one that does. */
#define MB_KEY_UP       2
#define MB_KEY_CHAR     3  /* the produced text (send after RAW_DOWN)     */
typedef struct mbKeyEvent {
  int struct_size;             // = sizeof(mbKeyEvent)
  int type;                    // MB_KEY_*
  int modifiers;               // bitmask: 1 ctrl, 2 shift, 4 alt, 8 meta
  int windows_key_code;        // Windows VK code (keyCode)
  int native_key_code;         // platform scan code (informational)
  const char* text;            // UTF-8 text the key produces (may be NULL)
  const char* unmodified_text; // text without modifiers (may be NULL)
  int is_keypad;
  int is_auto_repeat;
  int is_system_key;           // e.g. Alt+key menu accelerators
} mbKeyEvent;
MB_EXPORT void mbSendKeyEvent(mbView*, const mbKeyEvent*);

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

// Enable (1, default) or disable (0) JavaScript for this view. Script-off is
// both hardening (untrusted static content can't run code) and a perf switch
// (no parse/compile/execute) for content that doesn't need it. Call before
// mbLoadURL/mbLoadHTML — applies to documents committed after the call.
// NOTE: while disabled, blink refuses ALL script in the view — the page's own
// AND host-driven (mbEvalJS*, mbRunJS return empty). Re-enable to script the
// document again (the page's inert <script> tags do not retroactively run).
MB_EXPORT void mbSetEnableJavascript(mbView*, int enabled);

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
// Synchronous — JS receives the return value inline. RETURN-STRING LIFETIME:
// the engine copies the returned string before the call returns, so a static
// buffer, a per-binding buffer overwritten on the next call, or a std::string's
// c_str() held by userdata are all fine. NULL is safe: undefined for
// string/json returns, 0/false for number/boolean. The binding is
// installed into each new document's main world (call before navigating).
// `userdata` is passed back to every invocation.
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

// Write this view's browsing-profile cookie jar (every host, including both
// session cookies and persistent cookies) into `out` as a Netscape cookie file
// (NUL-terminated, up to out_cap; size first with out=NULL/out_cap=0). Returns
// the full length. This exports custom sessions too; the view-less mbSaveCookies writes
// only the implicit default session.
MB_EXPORT int mbGetAllCookies(mbView*, char* out, int out_cap);

// Inject a cookie ("name=value[; Path=/; Domain=...; Secure]") into the HTTP jar
// for `url`'s origin — the inverse of mbGetCookies, for restoring a saved login
// session before navigating. No-op for non-http(s) URLs.
MB_EXPORT void mbSetCookie(mbView*, const char* url, const char* cookie);

// Erase the entire HTTP cookie jar for this view's browsing session/profile.
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
// `headers` already includes view/global request headers plus every matching static
// mbSetRequestHeader* registration, in registration order.
// Return nonzero to BLOCK, zero to allow. Setting either request callback replaces the
// other (one slot). NULL clears it.
typedef int (*mbRequestCallbackEx)(const char* url, const char* method,
                                   const char* headers, const char* body,
                                   int body_len, void* userdata);
MB_EXPORT void mbSetRequestCallbackEx(mbRequestCallbackEx cb, void* userdata);

// MUTABLE request hook — the request-side twin of the mbResponse* handle: the
// callback receives an opaque mbRequest it can inspect AND change, so per-
// request decisions that the static tables can't express become possible —
// compute an Authorization header for one host only, redirect a fetch to a
// local server, veto by full request context. Fires on the main thread for
// every request (subresources AND top-level loads), after the static
// block/rewrite/header tables. The handle is valid only during the callback.
//   mbRequestURL/Method/Headers   what's about to be fetched. Headers are the
//                                 composed caller/request headers plus matching
//                                 static registrations, "\n"-joined.
//   mbRequestBody                 request body bytes (POST/PUT; *out_len gets
//                                 the length, NOT NUL-safe); "" for GET.
//   mbRequestSetUrl               TRANSPARENTLY redirect the fetch (the page
//                                 still sees the original URL, like
//                                 mbRewriteUrl). Invalid URLs are ignored.
//   mbRequestSetHeader            add an outgoing header; an existing
//                                 same-name header is REPLACED.
//   mbRequestBlock                fail the request (ERR_BLOCKED_BY_CLIENT); a
//                                 blocked TOP-LEVEL load reports error_domain
//                                 "blocked" via mbOnFailLoadingEx.
// ONE SLOT with mbSetRequestCallback/Ex: setting any of the three replaces the
// others. NULL clears. Process-wide.
typedef struct mbRequest mbRequest;
MB_EXPORT const char* mbRequestURL(mbRequest*);
MB_EXPORT const char* mbRequestMethod(mbRequest*);
MB_EXPORT const char* mbRequestHeaders(mbRequest*);
MB_EXPORT const char* mbRequestBody(mbRequest*, int* out_len);
MB_EXPORT void mbRequestSetUrl(mbRequest*, const char* url);
MB_EXPORT void mbRequestSetHeader(mbRequest*, const char* name, const char* value);
MB_EXPORT void mbRequestBlock(mbRequest*);
// TLS public-key pinning for THIS request (the secure counterpart of
// mbSetIgnoreCertErrors): the fetch fails with a curl-domain error
// (CURLE_SSL_PINNEDPUBKEYNOTMATCH, code 90) unless the server's leaf public
// key matches. `pins` is curl CURLOPT_PINNEDPUBLICKEY syntax, passed through
// verbatim: "sha256//BASE64" hashes, several separated by ";". Applies to the
// whole redirect chain of the fetch. NULL/"" = no pin (the default).
MB_EXPORT void mbRequestPinPublicKey(mbRequest*, const char* pins);
typedef void (*mbRequestHookCallback)(mbRequest*, void* userdata);
MB_EXPORT void mbSetRequestHook(mbRequestHookCallback cb, void* userdata);

// Response hook: a process-wide callback invoked after a successful load, BEFORE the body
// reaches the page — so you can inspect the headers / status or REPLACE the response bytes
// (inject a script, strip content, rewrite a JSON payload). Covers every load kind: the
// MAIN document of a top-level navigation (mbLoadURL / mbPostURL) and a page-initiated
// navigation (link click / location= / form submit), as well as subresources, fetch/XHR,
// worker scripts, mocks, downloads, and file:/data: loads. The handle is valid only for the
// duration of the callback; query it with mbResponseURL/Status/Headers/Body and replace the
// body with mbResponseSetBody (which updates the delivered length). Runs on the main thread
// inside the load. NULL clears it.
//
// mbResponseURL is the final PAGE-VISIBLE URL after server redirects (the requested URL
// for mock/file/data). Transparent mbRewriteUrl/mbRequestSetUrl transport targets never
// leak through it, including when a rewritten backend returns a relative redirect
// or an absolute/network-path redirect back to its exact transport origin.
// Fires exactly once per load.
//
// Covers BUFFERED loads only — where the whole body is in hand before delivery, so it can be
// inspected/replaced. STREAMING transports (EventSource/SSE, and streaming downloads via
// mbOnDownloadStream) do NOT invoke it: their bytes are delivered incrementally with no
// buffered body to rewrite. A host that must rewrite a body uses the buffered path.
//
typedef struct mbResponse mbResponse;
typedef void (*mbResponseCallback)(mbResponse*, void* userdata);
MB_EXPORT void mbSetResponseCallback(mbResponseCallback cb, void* userdata);
// Extended form: identical to mbSetResponseCallback but the callback also
// receives the ORIGINATING view so a multi-view host can tell which tab a response belongs
// to (and reach its session via mbViewGetSession). `view` may be NULL for a load with no
// owning view (a view-less/background fetch). A shared worker is attributed to its
// starting connection while that connection remains live, then to another connected
// live view in the same session; all connected views receive its network-idle activity.
// Added alongside — NOT replacing —
// mbSetResponseCallback so already-compiled clients keep their (response, userdata) ABI.
// Setting either callback replaces the other (one hook slot).
typedef void (*mbResponseCallbackEx)(mbResponse*, mbView* view, void* userdata);
MB_EXPORT void mbSetResponseCallbackEx(mbResponseCallbackEx cb, void* userdata);
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
// is invisible to the host.
//
// SCOPE (be explicit — this is process-wide by design): the Notification permission is
// auto-GRANTED for every origin (a headless engine has no permission UI), so onshow always
// fires and this hook always runs. The callback carries the notification's own fields only,
// NOT the originating view or origin — the engine's NotificationService is a single
// process-wide binder, not per-view, so a multi-view host cannot attribute a notification
// to a specific tab here. If you need per-origin policy, gate inside the page (or don't
// grant it upstream). NULL clears it. `tag`/`icon` may be "" when unset.
typedef void (*mbNotificationCallback)(void* userdata, const char* title,
                                       const char* body, const char* tag,
                                       const char* icon);
MB_EXPORT void mbOnNotificationShown(mbNotificationCallback cb, void* userdata);

// Response mocking — the signature interception feature. Any request whose URL
// CONTAINS `url_substring` is served `body` WITHOUT a real network fetch: run a
// page fully offline, or substitute an API/XHR/fetch response in tests/automation.
// `content_type` defaults to "text/html" when NULL/empty; `status` defaults to 200
// when <= 0. Register several (last matching wins on overlap); mbClearMocks removes
// all. Process-wide, applied after blocking policy, so a blocked URL remains blocked
// even when it also matches a mock.
// Set before navigating to affect that page's requests.
MB_EXPORT void mbMockResponse(const char* url_substring, const char* body,
                              const char* content_type, int status);
MB_EXPORT void mbClearMocks(void);

// Host image sources — embed live host-generated images in pages (the
// ImageSource pattern: charts, camera frames, app-rendered icons) without
// data: URLs or a local HTTP server. Registers (or replaces) image `id`
// backed by width x height premultiplied BGRA pixels (stride bytes per row;
// 0 = width*4; the pixels are copied and PNG-encoded once). Pages display it
// like any image:
//
//   <img src="https://mb-image.internal/myimage">
//
// The reserved mb-image.internal host is answered in-process by the engine
// (200, image/png) ahead of the network and the mock layer; any ?query is
// ignored. Re-registering an id swaps the backing image and dispatches the
// document CustomEvent 'mbimagesourceupdate' (detail = id) in every live
// view, so a page showing live frames re-fetches with a cache-buster:
//
//   document.addEventListener('mbimagesourceupdate', e => {
//     if (e.detail === 'cam') img.src =
//         'https://mb-image.internal/cam?v=' + performance.now();
//   });
//
// `id` is [A-Za-z0-9._-] (anything else is ignored). Process-wide, like
// mbMockResponse. One PNG encode per registration: right for icons, charts
// and moderate-rate frames — not a 60 fps video path.
MB_EXPORT void mbRegisterImageSource(const char* id, const void* bgra,
                                     int width, int height, int stride);
MB_EXPORT void mbUnregisterImageSource(const char* id);

// ZERO-COPY image source: like mbRegisterImageSource but the engine BORROWS
// the caller's pixels — no copy, no eager PNG encode; the PNG is produced
// lazily on the FIRST fetch and cached. For high-rate/large sources where the
// copying variant's per-registration encode is the bottleneck. The buffer
// must stay valid and UNCHANGED until `release` fires: when the id is
// replaced (either register variant) or unregistered. Returns 1 on success;
// 0 when refused (bad id/args) — then the engine did NOT take the buffer and
// `release` will never fire. Fires the same mbimagesourceupdate event.
typedef void (*mbImageSourceReleaseCallback)(void* userdata, const void* bgra);
MB_EXPORT int mbRegisterImageSourceBuffer(const char* id, const void* bgra,
                                          int width, int height, int stride,
                                          mbImageSourceReleaseCallback release,
                                          void* userdata);

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
// document's view (including nested-worker scripts); a shared worker uses its
// starting connection while that connection remains live, then another connected
// live view in the same session. Consulted after the static mock table and before
// the process-wide callback. A worker with no remaining live owning/connected view
// falls through to the process-wide hook.
// Pass NULL to remove; removed automatically when the view is destroyed.
MB_EXPORT void mbOnRequestMock(mbView*, mbRequestMockCallback, void* userdata);

// Request URL rewriting — the request-side counterpart to mocking. Before any
// fetch, the first occurrence of `from` in a request URL is replaced with `to`
// (host swap, http->https, point a CDN/API at a local mock). The rewrite is
// transparent: the page still sees its ORIGINAL URL as the response URL (so
// fetch()/XHR behave correctly). Across a server redirect, relative Location values
// resolve separately against the visible URL and rewritten transport URL. An absolute
// Location back to the exact current backend origin, or a network-path reference such
// as `//host/path` that resolves to that exact transport host and effective port, is
// projected onto the public origin too. A different host or effective port remains a
// visible real redirect. Register several (applied in order);
// mbClearUrlRewrites removes all. Process-wide, applied at the loader.
MB_EXPORT void mbRewriteUrl(const char* from, const char* to);
MB_EXPORT void mbClearUrlRewrites(void);

// Per-URL request header injection: add/override the outgoing http(s) header `name: value`
// for any request whose URL CONTAINS `url_substring` — e.g. a per-domain User-Agent, or a
// header keyed off a path/query marker. Conditional on the URL, unlike the global
// mbSetExtraHeaders. NOTE: this is a raw SUBSTRING match over the whole URL, so it is a
// footgun for CREDENTIALS — "api.example.com" also matches
// "https://evil.test/?next=api.example.com". For an Authorization / API-key header bound to
// a host, prefer mbSetRequestHeaderForHost. Register several; mbClearRequestHeaders removes
// all three kinds. Process-wide, applied at the loader.
MB_EXPORT void mbSetRequestHeader(const char* url_substring, const char* name,
                                  const char* value);

// Host-scoped request header injection. The header rides a request when the
// request URL's parsed HOST matches `host_filter`, applied PER-HOP on every buffered and
// streaming HTTP transport (top-level navigation, fetch/XHR/subresource, SSE/download).
// `host_filter`:
//   "api.example.com"       exact host only
//   ".example.com"          that host or any subdomain (leading dot opts in)
//   "api.example.com/v2/"   exact host AND request path starts with "/v2/"
// NOTE: host-only — it does NOT check scheme or port, so http://host and https://host (and
// any port) all match. It is tighter than the substring form but is NOT a full-origin
// match; for a CREDENTIAL bound to one origin, prefer mbSetRequestHeaderForOrigin.
// Non-http(s) URLs (data:/file:/blob:) have no host and never match. Register several;
// mbClearRequestHeaders removes all. Process-wide.
MB_EXPORT void mbSetRequestHeaderForHost(const char* host_filter, const char* name,
                                         const char* value);

// Origin-scoped request header injection — the strict, cross-origin-safe form
// for credentials (Authorization / X-Api-Key). The header rides a request only when the
// request URL's full ORIGIN matches: scheme AND host AND effective port. `origin` is
// "scheme://host[:port][/path/prefix]" — a default port is implied by the scheme, so
// "https://api.example.com" == "https://api.example.com:443" but != "http://..." and
// != ":8443". An optional trailing path further narrows it (path-prefix match). Like the
// host form it is applied PER-HOP, so a credential binds to exactly its origin and never
// rides a cross-origin (or cross-scheme/port)
// redirect. Register several; mbClearRequestHeaders removes all (all three kinds).
//
// Override semantics (all three injection APIs): matching registrations are applied in
// true process-wide call order across substring/host/origin categories. A later same-name
// registration, or one colliding with mbSetExtraHeaders, OVERRIDES case-insensitively.
// The request callback then inspects the composed result; mbRequestSetHeader runs last and
// may override it again.
MB_EXPORT void mbSetRequestHeaderForOrigin(const char* origin, const char* name,
                                           const char* value);
MB_EXPORT void mbClearRequestHeaders(void);

// In-process text clipboard shared with the page (navigator.clipboard / execCommand).
// mbSetClipboard makes the page's next navigator.clipboard.readText() / paste see `text`;
// mbGetClipboard writes the current clipboard text into `out` (NUL-terminated; size first
// with out=NULL) and returns its length — read what a page copied via writeText/copy.
// Process-wide. clipboard-read/write permission is granted so navigator.clipboard works.
MB_EXPORT void mbSetClipboard(const char* utf8_text);
MB_EXPORT int  mbGetClipboard(char* out, int out_cap);

// OS-clipboard bridge: route the PAGE's clipboard through the HOST so paste
// sees the real pasteboard and copy lands on it — without this, an interactive
// host must sync mbSet/GetClipboard around every copy/paste by hand.
//   read_cb   consulted on page READS (paste, navigator.clipboard.readText):
//             write the current host-clipboard text (UTF-8, NUL-terminated,
//             truncated to out_cap) into `out` and return the FULL length in
//             bytes (the engine retries with a bigger buffer if it exceeds
//             out_cap-1). While installed it is authoritative — page reads
//             bypass the in-process jar (and mbSetClipboard).
//   write_cb  fired on page WRITES (copy/cut, writeText) with the text. The
//             in-process jar still keeps a copy, so mbGetClipboard works.
// Either may be NULL — that direction keeps the in-process jar. Process-wide.
// THREADING: both callbacks fire on an engine SERVICE thread (not the main
// thread, like mbOnLogMessage) — keep them thread-safe and cheap; marshal to
// your UI thread yourself if your pasteboard API needs it. NULL/NULL restores
// the pure in-process clipboard.
typedef int (*mbClipboardReadCallback)(void* userdata, char* out, int out_cap);
typedef void (*mbClipboardWriteCallback)(void* userdata, const char* utf8_text);
MB_EXPORT void mbSetClipboardHandler(mbClipboardReadCallback read_cb,
                                     mbClipboardWriteCallback write_cb,
                                     void* userdata);

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
// Jump `offset` entries in one step (negative = back, positive = forward);
// mbGoToOffset(v, -1) == mbGoBack. Returns 1 if it navigated, 0 when the
// target is out of range (or offset is 0).
MB_EXPORT int mbGoToOffset(mbView*, int offset);

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

// STRUCTURED console push — the terminal shape (a struct_size-versioned struct,
// so new fields never need another Ex). Adds over mbOnConsoleMessageEx:
//   column     the 1-based column within `line` (0 when unknown)
//   category   blink's message-source category: which subsystem emitted it —
//              "javascript" (exceptions), "console-api" (console.*),
//              "network", "security", "rendering", "storage", "deprecation",
//              "violation", "intervention", "worker", "xml", "other"
// String pointers are valid only during the callback. ONE SLOT with
// mbOnConsoleMessage/Ex (setting any of the three replaces the others).
// NULL clears.
typedef struct mbConsoleMessageInfo {
  int struct_size;      // = sizeof(mbConsoleMessageInfo), ABI versioning
  const char* level;    // "verbose" / "log" / "warn" / "error"
  const char* message;  // UTF-8 message text
  const char* source;   // source URL ("" when unknown)
  int line;             // 1-based line (0 unknown)
  int column;           // 1-based column (0 unknown)
  const char* category; // source category, see above
  const char* stack;    // JS stack ("" when unavailable)
} mbConsoleMessageInfo;
typedef void (*mbConsoleCallback2)(mbView*, void* userdata,
                                   const mbConsoleMessageInfo* info);
MB_EXPORT void mbOnConsoleMessage2(mbView*, mbConsoleCallback2 cb, void* userdata);

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

// ---- Per-frame load lifecycle + stable frame IDs ------------------------------
// The frame-aware sibling of the main-frame load callbacks: fires for EVERY
// local frame (main document AND iframes, nested included) at each phase, with
// the frame's STABLE id — constant for the frame's whole life, never reused
// within a view — so iframe automation doesn't race the index-based APIs when
// the page mutates its frame set. `url` is the frame's document URL, valid
// only during the call. The main-frame-only callbacks (mbOnBeginLoading /
// mbOnDOMContentLoaded / mbOnLoadFinish / mbOnFailLoading*) are unchanged and
// remain the simple path. MB_FRAME_LOAD_FAIL currently reports top-level
// failures only (is_main_frame=1). NULL clears.
#define MB_FRAME_LOAD_BEGIN     0  /* navigation committed in the frame      */
#define MB_FRAME_LOAD_DOM_READY 1  /* the frame's DOMContentLoaded dispatched */
#define MB_FRAME_LOAD_FINISH    2  /* the frame's load event fired            */
#define MB_FRAME_LOAD_FAIL      3  /* top-level load failed (main frame only) */
typedef void (*mbFrameLoadCallback)(mbView*, void* userdata, uint64_t frame_id,
                                    int is_main_frame, int phase,
                                    const char* url);
MB_EXPORT void mbOnFrameLoadEvent(mbView*, mbFrameLoadCallback, void* userdata);

// Write the stable ids of all live local frames into `out` (up to `cap`):
// main frame first, then depth-first document order — NESTED frames included,
// unlike mbGetFrameCount's direct-children contract. Returns the total number
// of live frames (may exceed cap; size first with out=NULL/cap=0).
MB_EXPORT int mbGetFrameIds(mbView*, uint64_t* out, int cap);

// mbEvalJSInFrame keyed by STABLE frame id (from mbOnFrameLoadEvent /
// mbGetFrameIds) instead of a racy document-order index; reaches nested
// frames too. Same out-buffer contract as mbEvalJS; "" for an unknown or
// dead id.
MB_EXPORT int mbEvalJSInFrameById(mbView*, uint64_t frame_id,
                                  const char* utf8_script, char* out,
                                  int out_cap);

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
// PIXEL FORMAT (all paint exports): BGRA byte order, PREMULTIPLIED alpha, sRGB.
// With mbSetTransparentBackground, composite as premultiplied — converting to
// straight alpha (or assuming it) fringes antialiased edges.
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

// Force the next paint: mark the view dirty even though blink thinks the last
// frame is current. For when the HOST lost its copy of the pixels (a purged
// CALayer on hide/show, a buffer dropped on resize) — without this a
// damage-gated blit loop would skip forever.
MB_EXPORT void mbViewSetDirty(mbView*);

// The damaged region of the frame delivered by the LAST successful
// mbRepaintToBitmap / mbPaintToBitmap: the rect (logical px, clamped to the
// view; multiply by the device scale factor for buffer coordinates) that can
// differ from the previous painted frame. Everything outside it is
// bit-identical, so after a successful repaint a damage-gated host uploads
// only this subrect; w==h==0 means the frame was identical — skip the blit
// entirely. Reports the full view when fine-grained damage is unknown (first
// paint, resize, mbViewSetDirty, mbSetForceRepaint, composited views).
// Single-buffer hosts only as-is: with several buffers in flight, union the
// rects since each buffer's last paint yourself. Returns 1 (0 = bad view).
MB_EXPORT int mbViewGetDirtyRect(mbView*, int* x, int* y, int* w, int* h);

// Diagnostic: while enabled (1), mbViewIsDirty always reports 1 for this view,
// so a damage-gated host repaints every tick. The escape hatch for "the dirty
// flag is lying" bugs — bypass the gating to bisect whether stale content
// comes from damage tracking or from the paint itself. 0 (default) restores
// normal dirty gating.
MB_EXPORT void mbSetForceRepaint(mbView*, int enabled);

// ---- Pixel-format utilities ----------------------------------------------------
// The conversions the paint contract implies (BGRA, PREMULTIPLIED, sRGB — see
// the header top), so hosts stop hand-rolling them. All operate in place on a
// width x height buffer with `stride` bytes per row (0 = width*4); no view or
// engine state needed, callable before mbInitialize, any thread.
// Premultiplied -> straight alpha (for consumers that need unassociated
// alpha; note PNG exports already do this internally).
MB_EXPORT void mbConvertToStraightAlpha(void* pixels, int width, int height,
                                        int stride);
// Straight -> premultiplied alpha (before handing host-generated pixels to
// mbRegisterImageSource*, which expect premultiplied).
MB_EXPORT void mbConvertToPremultipliedAlpha(void* pixels, int width,
                                             int height, int stride);
// Swap the R and B channels in place: BGRA <-> RGBA, for RGBA-only consumers
// (OpenGL textures, image libraries).
MB_EXPORT void mbSwapRedBlueChannels(void* pixels, int width, int height,
                                     int stride);
#if defined(__cplusplus)
}  // extern "C"
#endif

#endif  // MINIBLINK2_WEBVIEW_H_
