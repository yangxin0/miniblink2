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

// Opaque string handle passed to callbacks; read it with wkeGetString.
typedef struct _tagWkeString* wkeString;

// Scripting handle types (upstream: jsExecState = void*, jsValue = __int64). In
// this slice a jsValue is an opaque handle to the string result of a wkeRunJS
// call; the jsToXxx readers below coerce it. The full V8-backed jsValue object
// model (jsObject/jsArray/jsCall, constructing values to pass into JS) is not yet
// implemented.
typedef void* jsExecState;
typedef long long jsValue;

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
// POST `postData` (Content-Type defaults to form-urlencoded) to an http(s) URL and
// commit the response. `postLen` is the body length; the body is treated as text
// (a trailing NUL is assumed, as for form/JSON data).
WKE_API void wkePostURL(wkeWebView webView, const utf8* url, const char* postData,
                        int postLen);

// --- Navigation history --------------------------------------------------------
// Back/forward over the view's navigation history. The Can* queries report
// whether a step is available; wkeGoBack/wkeGoForward perform it (re-navigating to
// that entry's URL) and return true on success.
WKE_API bool wkeCanGoBack(wkeWebView webView);
WKE_API bool wkeGoBack(wkeWebView webView);
WKE_API bool wkeCanGoForward(wkeWebView webView);
WKE_API bool wkeGoForward(wkeWebView webView);

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
// The full scrollable document size (>= the view), e.g. to size a full-page
// capture. 0 before the first load.
WKE_API int wkeGetContentWidth(wkeWebView webView);
WKE_API int wkeGetContentHeight(wkeWebView webView);

// --- Accessors / config --------------------------------------------------------
// The returned const utf8* is owned by the view and valid until the next call to
// the same getter on that view (the classic wke contract).
WKE_API const utf8* wkeGetURL(wkeWebView webView);
WKE_API const utf8* wkeGetTitle(wkeWebView webView);
WKE_API void wkeSetUserAgent(wkeWebView webView, const utf8* userAgent);
// Extra HTTP request headers added to the navigation and its subresources:
// newline-separated "Name: Value" lines (NULL/"" clears them). Set before
// loading. Port extension — classic wke injects per-request via the net hook.
WKE_API void wkeSetExtraHeaders(wkeWebView webView, const utf8* headers);
// Emulate prefers-color-scheme: dark (true) or light (false) so a page renders
// its dark theme. Persists across loads; set before navigating. Port extension.
WKE_API void wkeSetDarkMode(wkeWebView webView, bool dark);
// Emulate i18n environment for deterministic localized rendering (port
// extensions). wkeSetLocale drives navigator.language(s) (comma-separated, e.g.
// "fr-FR,fr,en"); wkeSetTimezone overrides the Date/Intl timezone (an IANA id,
// e.g. "America/New_York" — process-global). Set before navigating.
WKE_API void wkeSetLocale(wkeWebView webView, const utf8* languages);
WKE_API void wkeSetTimezone(wkeWebView webView, const utf8* ianaTimezone);
// Run a script in each new document BEFORE the page's own scripts (like
// Puppeteer's evaluateOnNewDocument): set globals, stub/override APIs, or
// install a harness the page observes. NULL/"" clears. Port extension.
WKE_API void wkeSetInitScript(wkeWebView webView, const utf8* script);
// Print the current document to a multi-page US-Letter PDF at `path`; returns
// whether the file was written. Port extension (no classic wke print API).
WKE_API bool wkeSavePdf(wkeWebView webView, const utf8* path);
// Capture with a transparent background (areas the page does not paint keep
// alpha 0) instead of opaque white. Call before loading the page.
WKE_API void wkeSetTransparent(wkeWebView webView, bool transparent);
// Whether wkeSetTransparent(true) was last set on this view.
WKE_API bool wkeIsTransparent(wkeWebView webView);
// Pure wke view-state. wkeSetName/wkeGetName label the view (default ""). The
// user key/value store lets an app attach its own context to a view (e.g. to
// recover it inside a callback); wkeGetUserKeyValue returns NULL for an unset
// key. Values are owned by the app — wke neither copies nor frees them.
WKE_API const char* wkeGetName(wkeWebView webView);
WKE_API void wkeSetName(wkeWebView webView, const char* name);
WKE_API void wkeSetUserKeyValue(wkeWebView webView, const char* key, void* value);
WKE_API void* wkeGetUserKeyValue(wkeWebView webView, const char* key);
// Page zoom factor (1.0 = 100%). This port models it as CSS zoom on the document
// element, scaling layout (and the rects getBoundingClientRect reports); it is
// re-applied after each navigation. Non-positive factors are ignored.
WKE_API void wkeSetZoomFactor(wkeWebView webView, float factor);
WKE_API float wkeGetZoomFactor(wkeWebView webView);
// Make the whole document editable (modeled as document.designMode). The flag
// is re-applied after each navigation, matching wke's persistent editability.
WKE_API void wkeSetEditable(wkeWebView webView, bool editable);
// The current document's HTML (the rendered, post-JS DOM serialized to HTML).
// Owned by the view, valid until the next wkeGetSource on it.
WKE_API const utf8* wkeGetSource(wkeWebView webView);

// Cookies, backed by the (process-wide) libcurl cookie jar.
// wkeGetCookie returns the jar's cookies for the CURRENT document's URL as
// "name=value; name2=value2" (owned by the view, valid until the next call).
// wkeSetCookie injects a single "name=value[; Path=/; Domain=...]" set-cookie
// string for `url`'s origin — useful for restoring a saved session.
WKE_API const utf8* wkeGetCookie(wkeWebView webView);
WKE_API void wkeSetCookie(wkeWebView webView, const utf8* url,
                          const utf8* cookie);
// Set the cookie jar file path (Flush/Reload persist to/from it). This port
// takes a utf8 path rather than the Windows wke WCHAR. The path is process-wide
// (the jar is shared); the webView arg is accepted for signature compatibility.
WKE_API void wkeSetCookieJarPath(wkeWebView webView, const utf8* path);
// wkePerformCookieCommand drives the whole jar (no view: it acts on the last
// live webView). The clear commands reset the jar; FlushCookiesToFile /
// ReloadCookiesFromFile persist to/from the wkeSetCookieJarPath file (a no-op
// until a path is set), in curl's Netscape jar format.
typedef enum _wkeCookieCommand {
  wkeCookieCommandClearAllCookies,
  wkeCookieCommandClearSessionCookies,
  wkeCookieCommandFlushCookiesToFile,
  wkeCookieCommandReloadCookiesFromFile,
} wkeCookieCommand;
WKE_API void wkePerformCookieCommand(wkeCookieCommand command);

// Process-wide HTTP/SOCKS proxy for all subsequent network fetches. The struct
// matches classic wke; the type selects the curl proxy scheme, and a non-empty
// username enables proxy auth. A NULL proxy or WKE_PROXY_NONE forces a direct
// connection (overriding *_proxy env vars). Affects http(s) only.
typedef enum {
  WKE_PROXY_NONE,
  WKE_PROXY_HTTP,
  WKE_PROXY_SOCKS4,
  WKE_PROXY_SOCKS4A,
  WKE_PROXY_SOCKS5,
  WKE_PROXY_SOCKS5HOSTNAME,
} wkeProxyType;
typedef struct {
  wkeProxyType type;
  char hostname[100];
  unsigned short port;
  char username[50];
  char password[50];
} wkeProxy;
WKE_API void wkeSetProxy(const wkeProxy* proxy);

// --- Input ---------------------------------------------------------------------
// Deliver a mouse event at (x, y). `message` is one of the WKE_MSG_* codes;
// `flags` carries WKE_LBUTTON/etc. (currently advisory). Supported in this slice:
// MOUSEMOVE (move), LBUTTONUP (a left click — the press is implicit, as with a
// real button-up), and LBUTTONDBLCLK (double click). Returns true if handled.
WKE_API bool wkeFireMouseEvent(wkeWebView webView, unsigned int message, int x,
                               int y, unsigned int flags);

// Deliver a mouse-wheel event at (x, y). `delta` follows the Win32 convention:
// positive scrolls up / away from the user, negative scrolls down (one notch is
// ~120). Returns true if handled.
WKE_API bool wkeFireMouseWheelEvent(wkeWebView webView, int x, int y, int delta,
                                    unsigned int flags);

// Keyboard events to the focused element. wkeFireKeyPressEvent inserts the
// character `charCode` (the text-producing event). wkeFireKeyDownEvent maps the
// common special virtual-key codes (VK_RETURN=0x0D, VK_TAB=0x09, VK_BACK=0x08,
// VK_DELETE=0x2E, VK_ESCAPE=0x1B, the arrows, Home/End/PageUp/PageDown) to their
// default actions; a plain character key is a no-op there (the matching
// wkeFireKeyPressEvent does the insertion, as with a real keyboard). KeyUp is a
// no-op. `flags`/`systemKey` are advisory in this slice. Return true if handled.
WKE_API bool wkeFireKeyDownEvent(wkeWebView webView, unsigned int virtualKeyCode,
                                 unsigned int flags, bool systemKey);
WKE_API bool wkeFireKeyUpEvent(wkeWebView webView, unsigned int virtualKeyCode,
                               unsigned int flags, bool systemKey);
WKE_API bool wkeFireKeyPressEvent(wkeWebView webView, unsigned int charCode,
                                  unsigned int flags, bool systemKey);

// --- Scripting -----------------------------------------------------------------
// Run `script` in the page's main frame and return a handle to its result. Read
// the result with the jsToXxx coercions below (string/int/double/boolean). The
// script must not open a modal dialog (alert/confirm/prompt) — that path is not
// serviced here. NOTE: scripts that themselves show dialogs are unsupported.
WKE_API jsValue wkeRunJS(wkeWebView webView, const utf8* script);
// The view's global execution state. Accepted by the jsToXxx readers (in this
// slice the result is carried by the jsValue handle, so the state is a token).
WKE_API jsExecState wkeGlobalExec(wkeWebView webView);

// Coerce a wkeRunJS result. jsToTempString returns a pointer owned by the library,
// valid until the next jsToTempString call (the classic wke "temp" contract).
WKE_API int jsToInt(jsExecState es, jsValue v);
WKE_API double jsToDouble(jsExecState es, jsValue v);
WKE_API bool jsToBoolean(jsExecState es, jsValue v);
WKE_API const utf8* jsToTempString(jsExecState es, jsValue v);

// The JS type of a wkeRunJS result (captured during the eval). The object kinds
// (object/array/function) are reported, but reading into them (jsGet/jsGetAt/
// jsCall) is not yet implemented — read structured data via wkeRunJS for now.
typedef enum _jsType {
  JSTYPE_NUMBER,
  JSTYPE_STRING,
  JSTYPE_BOOLEAN,
  JSTYPE_OBJECT,
  JSTYPE_FUNCTION,
  JSTYPE_UNDEFINED,
  JSTYPE_ARRAY,
  JSTYPE_NULL,
} jsType;
WKE_API jsType jsTypeOf(jsValue v);

// Object-model reads over a wkeRunJS result (array/object). jsGetLength returns
// an array's .length (0 otherwise); jsGetAt returns element `index` as a new,
// further-navigable jsValue (read it with the jsToXxx/jsTypeOf above; undefined if
// out of range). Valid until the next navigation. (jsGet by property name and
// jsCall remain future work.)
WKE_API int jsGetLength(jsExecState es, jsValue object);
WKE_API jsValue jsGetAt(jsExecState es, jsValue object, int index);
// Read object property `prop` (or a window global) as a new, further-navigable
// jsValue (undefined if absent). jsCall remains future work.
WKE_API jsValue jsGet(jsExecState es, jsValue object, const char* prop);
WKE_API jsValue jsGetGlobal(jsExecState es, const char* prop);

// Enumerate an object's own-enumerable property names (Object.keys order).
// The returned jsKeys* and its strings are owned by wke and stay valid only
// until the next jsGetKeys call on the same thread (empty list if not an
// object). Mirrors the classic wke jsGetKeys contract.
struct _jsKeys {
  unsigned int length;
  const char** keys;
};
typedef struct _jsKeys jsKeys;
WKE_API jsKeys* jsGetKeys(jsExecState es, jsValue object);

// Construct jsValues to pass as arguments INTO JS (e.g. to jsCall).
WKE_API jsValue jsInt(int n);
WKE_API jsValue jsDouble(double d);
WKE_API jsValue jsBoolean(bool b);
WKE_API jsValue jsString(jsExecState es, const utf8* str);
WKE_API jsValue jsUndefined(void);
WKE_API jsValue jsNull(void);

// Build structured values to populate and pass INTO JS. jsEmptyObject/
// jsEmptyArray return a fresh, navigable {}/[] handle; jsSet/jsSetAt mutate it
// in place by property name / index; jsSetGlobal assigns a window global. The
// assigned `value` may be any jsValue (a constructor result or another handle).
WKE_API jsValue jsEmptyObject(jsExecState es);
WKE_API jsValue jsEmptyArray(jsExecState es);
WKE_API void jsSet(jsExecState es, jsValue object, const char* prop,
                   jsValue value);
WKE_API void jsSetAt(jsExecState es, jsValue object, int index, jsValue value);
WKE_API void jsSetGlobal(jsExecState es, const char* prop, jsValue value);

// Call a JS function value with the given args, returning its result as a new
// jsValue. jsCall binds `thisObject`; jsCallGlobal uses the global `this`.
WKE_API jsValue jsCall(jsExecState es, jsValue func, jsValue thisObject,
                       jsValue* args, int argCount);
WKE_API jsValue jsCallGlobal(jsExecState es, jsValue func, jsValue* args,
                             int argCount);

// --- Paint (pull model): render the view into a caller BGRA buffer -------------
// `bits` must hold width*height*4 bytes; `pitch` is the row stride in bytes
// (pass width*4 for a tightly-packed buffer).
WKE_API void wkePaint(wkeWebView webView, void* bits, int pitch);

// --- Callbacks (the async event model) -----------------------------------------
// Read the text of a wkeString passed to a callback (owned by the library, valid
// for the duration of the callback).
WKE_API const utf8* wkeGetString(const wkeString string);

typedef enum _wkeLoadingResult {
  WKE_LOADING_SUCCEEDED,
  WKE_LOADING_FAILED,
  WKE_LOADING_CANCELED
} wkeLoadingResult;

// Fired when the document's title changes (after a load, in this slice).
typedef void (*wkeTitleChangedCallback)(wkeWebView webView, void* param,
                                        const wkeString title);
// Fired when a load finishes (synchronously at the end of wkeLoadURL/LoadHTML
// here, since the load is synchronous). `result` is SUCCEEDED/FAILED; `url` is
// the committed URL; `failedReason` is empty on success.
typedef void (*wkeLoadingFinishCallback)(wkeWebView webView, void* param,
                                         const wkeString url,
                                         wkeLoadingResult result,
                                         const wkeString failedReason);

WKE_API void wkeOnTitleChanged(wkeWebView webView,
                               wkeTitleChangedCallback callback,
                               void* callbackParam);
WKE_API void wkeOnLoadingFinish(wkeWebView webView,
                                wkeLoadingFinishCallback callback, void* param);

typedef enum _wkeConsoleLevel {
  wkeLevelLog = 1,
  wkeLevelWarning = 2,
  wkeLevelError = 3,
  wkeLevelDebug = 4,
  wkeLevelInfo = 5,
  wkeLevelRevokedError = 6,
} wkeConsoleLevel;

// Fired for each page console message (console.log/warn/error). In this slice
// `message` and `level` are populated; sourceName/sourceLine/stackTrace are empty
// (mb_capi's console capture is message+level only). Messages are delivered after
// a load settles and after wkeRunJS.
typedef void (*wkeConsoleCallback)(wkeWebView webView, void* param,
                                   wkeConsoleLevel level, const wkeString message,
                                   const wkeString sourceName,
                                   unsigned sourceLine,
                                   const wkeString stackTrace);
WKE_API void wkeOnConsole(wkeWebView webView, wkeConsoleCallback callback,
                          void* param);

// Fired when the document is ready (after a load settles, in this slice).
typedef void (*wkeDocumentReadyCallback)(wkeWebView webView, void* param);
WKE_API void wkeOnDocumentReady(wkeWebView webView,
                                wkeDocumentReadyCallback callback, void* param);

#endif  // MINIBLINK_WKE_WKE_H_
