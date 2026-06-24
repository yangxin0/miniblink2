// wke.cc — implementation of the wke compatibility slice over mb_capi.
//
// Each wke function is a thin wrapper over the modern mb_capi host. The wkeWebView
// handle wraps an mbView* plus the small per-view state wke exposes synchronously
// (size, the last load's scheme for success reporting, and string caches for the
// const utf8* getters).

#include "wke/wke.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "miniblink_host/capi/mb_capi.h"

// Opaque string handle passed to callbacks (backs wkeGetString).
struct _tagWkeString {
  std::string s;
};

struct _tagWkeWebView {
  mbView* view = nullptr;
  int width = 0;
  int height = 0;
  bool last_was_http = false;  // success reporting: http uses the status code
  std::string url_cache;       // backs wkeGetURL's const utf8* return
  std::string title_cache;     // backs wkeGetTitle's const utf8* return
  // Async callbacks (the wke event model).
  wkeLoadingFinishCallback on_loading_finish = nullptr;
  void* loading_param = nullptr;
  wkeTitleChangedCallback on_title_changed = nullptr;
  void* title_param = nullptr;
  std::string last_title;  // so a title callback only fires on a real change
  wkeConsoleCallback on_console = nullptr;
  void* console_param = nullptr;
  wkeDocumentReadyCallback on_document_ready = nullptr;
  void* document_ready_param = nullptr;
  wkeJsBridgeCallback on_js_bridge = nullptr;  // page->host window.mbBridge channel
  void* js_bridge_param = nullptr;
  std::string user_init_script;  // wkeSetInitScript; combined with the bridge bootstrap
  std::string bridge_channel_cache;  // backs the callback's channel arg
  std::string bridge_message_cache;  // backs the callback's message arg
  std::string source_cache;  // backs wkeGetSource's const utf8* return
  std::string cookie_cache;  // backs wkeGetCookie's const utf8* return
  // Pure wke view-state (not backed by the engine): an app-name, a transparent
  // flag mirror, and an app-owned key/value store threaded through callbacks.
  std::string name;
  bool transparent = false;
  std::map<std::string, void*> user_kv;
  double zoom_factor = 1.0;  // wkeSetZoomFactor; re-applied after each load
  bool editable = false;     // wkeSetEditable; re-applied after each load
  std::string selector_text_cache;  // backs wkeGetTextForSelector's return
  std::string selector_attr_cache;  // backs wkeGetAttribute's return
  std::string computed_style_cache;  // backs wkeGetComputedStyle's return
  std::string text_cache;            // backs wkeGetText's return
  std::string response_headers_cache;  // backs wkeGetResponseHeaders's return
};

// The last live webView, so the view-less wkePerformCookieCommand has a handle
// to drive the (process-wide) cookie jar. Set on create, cleared on destroy.
namespace {
wkeWebView g_last_webview = nullptr;
// The cookie jar file path (process-wide, matching mbSave/LoadCookies). Set via
// wkeSetCookieJarPath; the Flush/Reload cookie commands persist to/from it.
// Leaked (never destroyed) to avoid an exit-time destructor.
std::string& CookieJarPath() {
  static auto* p = new std::string();
  return *p;
}
}  // namespace

namespace {
bool IsHttpUrl(const utf8* url) {
  return url && (std::strncmp(url, "http://", 7) == 0 ||
                 std::strncmp(url, "https://", 8) == 0);
}
// Drain captured page console output and deliver it to the wkeOnConsole callback,
// one message per line ("level: text"). Only drains when a callback is set, so the
// buffer is preserved otherwise. Called after a load and after wkeRunJS.
void DrainConsoleToCallback(wkeWebView wv) {
  if (!wv || !wv->view || !wv->on_console)
    return;
  std::vector<char> buf(1 << 16, 0);
  mbDrainConsole(wv->view, buf.data(), static_cast<int>(buf.size()));
  const std::string all(buf.data());
  std::string::size_type start = 0;
  for (std::string::size_type i = 0; i <= all.size(); ++i) {
    if (i != all.size() && all[i] != '\n')
      continue;
    const std::string line = all.substr(start, i - start);
    start = i + 1;
    if (line.empty())
      continue;
    wkeConsoleLevel level = wkeLevelLog;
    std::string msg = line;
    const std::string::size_type colon = line.find(": ");
    if (colon != std::string::npos) {
      const std::string lv = line.substr(0, colon);
      msg = line.substr(colon + 2);
      if (lv == "warn")
        level = wkeLevelWarning;
      else if (lv == "error")
        level = wkeLevelError;
      else if (lv == "verbose")
        level = wkeLevelDebug;
    }
    _tagWkeString m{msg};
    _tagWkeString empty{std::string()};
    wv->on_console(wv, wv->console_param, level, &m, &empty, 0, &empty);
  }
}

// The page->host bridge bootstrap: defines window.mbBridge(channel, message),
// which queues "channelmessage" for the host to drain. Installed via the
// init script (so it exists before page scripts) only when a bridge callback is
// set. Combined with any user wkeSetInitScript.
const char kBridgeBootstrap[] =
    "window.mbBridge=function(c,m){(window.__mbb=window.__mbb||[])"
    ".push(String(c)+'\\u0001'+String(m));};";

void ApplyInitScript(wkeWebView wv) {
  if (!wv || !wv->view)
    return;
  std::string combined =
      wv->on_js_bridge ? std::string(kBridgeBootstrap) : std::string();
  combined += wv->user_init_script;
  mbSetInitScript(wv->view, combined.c_str());
}

// Drain queued window.mbBridge messages and deliver each to the host callback.
// Entries are joined by , each "channelmessage". Called after a load
// and after wkeRunJS (the points where page JS may have run).
void DrainBridgeToCallback(wkeWebView wv) {
  if (!wv || !wv->view || !wv->on_js_bridge)
    return;
  std::vector<char> buf(1 << 16, 0);
  mbEvalJS(wv->view,
           "(function(){try{var q=(window.__mbb||[]).join('\\u0002');"
           "window.__mbb=[];return q}catch(e){return ''}})()",
           buf.data(), static_cast<int>(buf.size()));
  const std::string all(buf.data());
  std::string::size_type start = 0;
  for (std::string::size_type i = 0; i <= all.size(); ++i) {
    if (i != all.size() && all[i] != '\x02')
      continue;
    const std::string entry = all.substr(start, i - start);
    start = i + 1;
    if (entry.empty())
      continue;
    const std::string::size_type sep = entry.find('\x01');
    wv->bridge_channel_cache =
        sep == std::string::npos ? std::string() : entry.substr(0, sep);
    wv->bridge_message_cache =
        sep == std::string::npos ? entry : entry.substr(sep + 1);
    wv->on_js_bridge(wv, wv->js_bridge_param, wv->bridge_channel_cache.c_str(),
                     wv->bridge_message_cache.c_str());
  }
}

// Re-apply the view's zoom to the current document. This port models wke's page
// zoom as CSS zoom on the document element, which scales layout (and the
// coordinates getBoundingClientRect reports). A factor of 1.0 is a no-op.
void ApplyZoom(wkeWebView wv) {
  if (!wv || !wv->view || wv->zoom_factor == 1.0)
    return;
  const std::string js = "try{document.documentElement.style.zoom='" +
                         std::to_string(wv->zoom_factor) + "'}catch(e){}";
  char buf[8] = {0};
  mbEvalJS(wv->view, js.c_str(), buf, sizeof(buf));
}

// Re-apply the view's editable flag to the current document (wke's whole-page
// editability is modeled as document.designMode). Fresh documents default to
// non-editable, so this only needs to act when the flag is set.
void ApplyEditable(wkeWebView wv) {
  if (!wv || !wv->view || !wv->editable)
    return;
  char buf[8] = {0};
  mbEvalJS(wv->view, "try{document.designMode='on'}catch(e){}", buf, sizeof(buf));
}

// Fire the title-changed then loading-finish callbacks after a load completes.
// (The load is synchronous, so this is the faithful "loading finished" moment.)
void FireLoadCallbacks(wkeWebView wv) {
  if (!wv || !wv->view)
    return;
  ApplyZoom(wv);      // a non-default zoom persists across navigations
  ApplyEditable(wv);  // a set editable flag persists across navigations
  DrainConsoleToCallback(wv);
  DrainBridgeToCallback(wv);  // deliver any window.mbBridge calls from page load
  if (wv->on_title_changed) {
    char tb[2048] = {0};
    mbGetTitle(wv->view, tb, sizeof(tb));
    if (tb[0] && wv->last_title != tb) {
      wv->last_title = tb;
      _tagWkeString title{std::string(tb)};
      wv->on_title_changed(wv, wv->title_param, &title);
    }
  }
  if (wv->on_document_ready)
    wv->on_document_ready(wv, wv->document_ready_param);
  if (wv->on_loading_finish) {
    char ub[4096] = {0};
    mbGetURL(wv->view, ub, sizeof(ub));
    _tagWkeString url{std::string(ub)};
    _tagWkeString reason{std::string()};
    const wkeLoadingResult result =
        wkeIsLoadingSucceeded(wv) ? WKE_LOADING_SUCCEEDED : WKE_LOADING_FAILED;
    wv->on_loading_finish(wv, wv->loading_param, &url, result, &reason);
  }
}
}  // namespace

void wkeInitialize(void) {
  mbInitialize();
}

void wkeFinalize(void) {
  mbShutdown();
}

wkeWebView wkeCreateWebView(void) {
  auto* wv = new _tagWkeWebView();
  wv->width = 800;  // wke's default view size; the app can wkeResize.
  wv->height = 600;
  wv->view = mbCreateView(wv->width, wv->height);
  if (!wv->view) {
    delete wv;
    return nullptr;
  }
  g_last_webview = wv;
  return wv;
}

void wkeDestroyWebView(wkeWebView webView) {
  if (!webView)
    return;
  if (g_last_webview == webView)
    g_last_webview = nullptr;
  if (webView->view)
    mbDestroyView(webView->view);
  delete webView;
}

void wkeLoadURL(wkeWebView webView, const utf8* url) {
  if (!webView || !webView->view)
    return;
  webView->last_was_http = IsHttpUrl(url);
  mbLoadURL(webView->view, url);
  mbWait(webView->view, 60);  // let async parsing/subresources settle (sync model)
  FireLoadCallbacks(webView);
}

void wkeLoadHTML(wkeWebView webView, const utf8* html) {
  if (!webView || !webView->view)
    return;
  webView->last_was_http = false;
  mbLoadHTML(webView->view, html, "about:blank");
  mbWait(webView->view, 30);
  FireLoadCallbacks(webView);
}

void wkeReload(wkeWebView webView) {
  if (webView && webView->view)
    mbReload(webView->view);
}

void wkePostURL(wkeWebView webView, const utf8* url, const char* postData,
                int /*postLen*/) {
  if (!webView || !webView->view)
    return;
  webView->last_was_http = true;
  mbPostURL(webView->view, url, postData, /*content_type=*/nullptr);
  mbWait(webView->view, 60);  // settle the synchronous load (as wkeLoadURL does)
  FireLoadCallbacks(webView);
}

bool wkeCanGoBack(wkeWebView webView) {
  return webView && webView->view && mbCanGoBack(webView->view);
}

bool wkeGoBack(wkeWebView webView) {
  if (!webView || !webView->view || !mbGoBack(webView->view))
    return false;
  mbWait(webView->view, 30);  // settle the re-navigation (synchronous load model)
  return true;
}

bool wkeCanGoForward(wkeWebView webView) {
  return webView && webView->view && mbCanGoForward(webView->view);
}

bool wkeGoForward(wkeWebView webView) {
  if (!webView || !webView->view || !mbGoForward(webView->view))
    return false;
  mbWait(webView->view, 30);
  return true;
}

bool wkeIsLoading(wkeWebView /*webView*/) {
  // The load is synchronous here: by the time wkeLoadURL/wkeLoadHTML returns the
  // document is committed, so nothing is ever still loading.
  return false;
}

bool wkeIsLoadingCompleted(wkeWebView /*webView*/) {
  return true;
}

bool wkeIsDocumentReady(wkeWebView /*webView*/) {
  return true;
}

bool wkeIsLoadingSucceeded(wkeWebView webView) {
  if (!webView || !webView->view)
    return false;
  if (!webView->last_was_http)
    return true;  // file/data/in-memory loads always commit
  const int status = mbGetHttpStatus(webView->view);
  return status >= 200 && status < 400;
}

bool wkeIsLoadingFailed(wkeWebView webView) {
  return !wkeIsLoadingSucceeded(webView);
}

void wkeResize(wkeWebView webView, int w, int h) {
  if (!webView || !webView->view || w <= 0 || h <= 0)
    return;
  webView->width = w;
  webView->height = h;
  mbResize(webView->view, w, h);
}

int wkeGetWidth(wkeWebView webView) {
  return webView ? webView->width : 0;
}

int wkeGetHeight(wkeWebView webView) {
  return webView ? webView->height : 0;
}

int wkeWidth(wkeWebView webView) {
  return wkeGetWidth(webView);
}

int wkeHeight(wkeWebView webView) {
  return wkeGetHeight(webView);
}

int wkeGetContentWidth(wkeWebView webView) {
  if (!webView || !webView->view)
    return 0;
  int w = 0, h = 0;
  mbGetContentSize(webView->view, &w, &h);
  return w;
}

int wkeGetContentHeight(wkeWebView webView) {
  if (!webView || !webView->view)
    return 0;
  int w = 0, h = 0;
  mbGetContentSize(webView->view, &w, &h);
  return h;
}

const utf8* wkeGetURL(wkeWebView webView) {
  if (!webView || !webView->view)
    return "";
  char buf[4096] = {0};
  mbGetURL(webView->view, buf, sizeof(buf));
  webView->url_cache.assign(buf);
  return webView->url_cache.c_str();
}

const utf8* wkeGetTitle(wkeWebView webView) {
  if (!webView || !webView->view)
    return "";
  char buf[2048] = {0};
  mbGetTitle(webView->view, buf, sizeof(buf));
  webView->title_cache.assign(buf);
  return webView->title_cache.c_str();
}

void wkeSetUserAgent(wkeWebView webView, const utf8* userAgent) {
  if (webView && webView->view)
    mbSetUserAgent(webView->view, userAgent);
}

void wkeSetExtraHeaders(wkeWebView webView, const utf8* headers) {
  // Newline-separated "Name: Value" lines applied to the navigation and its
  // subresources. NULL/"" clears them. (Port extension — classic wke does
  // per-request headers via the net hook.)
  if (webView && webView->view)
    mbSetExtraHeaders(webView->view, headers ? headers : "");
}

void wkeSetFocus(wkeWebView webView) {
  // Give the view window-focus (document.hasFocus() true, :focus-within active).
  if (webView && webView->view)
    mbSetFocus(webView->view, 1);
}

void wkeKillFocus(wkeWebView webView) {
  // Drop window-focus (document.hasFocus() false; blurs the active element).
  if (webView && webView->view)
    mbSetFocus(webView->view, 0);
}

void wkeSetLoadImages(wkeWebView webView, bool enable) {
  // Enable (default) or disable automatic image loading — disabling speeds up
  // text/HTML scraping (no image fetch/decode). Inline data: images are
  // unaffected. Set before navigating. (Port extension.)
  if (webView && webView->view)
    mbSetLoadImages(webView->view, enable ? 1 : 0);
}

void wkeSetDarkMode(wkeWebView webView, bool dark) {
  // Drive the prefers-color-scheme media feature so a page renders its dark (or
  // light) theme. The setting persists across loads; set it before navigating
  // for it to apply to that document. (Port extension — modern, not classic wke.)
  if (webView && webView->view)
    mbSetDarkMode(webView->view, dark ? 1 : 0);
}

void wkeSetLocale(wkeWebView webView, const utf8* languages) {
  // navigator.language / navigator.languages (comma-separated, e.g. "fr-FR,fr").
  // Set before navigating. (Port extension.)
  if (webView && webView->view && languages)
    mbSetLocale(webView->view, languages);
}

void wkeSetTimezone(wkeWebView webView, const utf8* ianaTimezone) {
  // Override the Date/Intl timezone (an IANA id, e.g. "America/New_York"), so
  // time-dependent UIs render deterministically. Process-global. (Port extension.)
  if (webView && webView->view && ianaTimezone)
    mbSetTimezone(webView->view, ianaTimezone);
}

void wkeSetInitScript(wkeWebView webView, const utf8* script) {
  // Run `script` in each new document BEFORE the page's own scripts (like
  // Puppeteer's evaluateOnNewDocument) — set globals, stub/override APIs, or
  // install a harness the page then observes. NULL/"" clears. (Port extension.)
  if (!webView || !webView->view)
    return;
  webView->user_init_script = script ? script : "";
  ApplyInitScript(webView);  // re-combine with the bridge bootstrap (if any)
}

void wkeOnJsBridge(wkeWebView webView, wkeJsBridgeCallback callback,
                   void* param) {
  // Register a host callback for page->host messages sent via
  // window.mbBridge(channel, message). One-way (no return value). Set before
  // navigating so the bootstrap is installed in the next document. (Port ext.)
  if (!webView || !webView->view)
    return;
  webView->on_js_bridge = callback;
  webView->js_bridge_param = param;
  ApplyInitScript(webView);  // (de)install the window.mbBridge bootstrap
}

bool wkeSavePdf(wkeWebView webView, const utf8* path) {
  // Print the current document to a multi-page US-Letter PDF at `path`.
  // Returns whether it was written. (Port extension — no classic wke print API.)
  return webView && webView->view && path &&
         mbSavePdf(webView->view, path) != 0;
}

bool wkeSavePng(wkeWebView webView, const utf8* path, int width, int height) {
  // Render the current frame at width x height and encode to `path`; the format
  // follows the extension (.jpg/.jpeg -> JPEG, else PNG). Returns whether it was
  // written. (Port extension — classic wke captures via wkePaint then app-encodes.)
  return webView && webView->view && path && width > 0 && height > 0 &&
         mbSavePng(webView->view, path, width, height) != 0;
}

bool wkeSavePngRect(wkeWebView webView, const utf8* path, int x, int y, int w,
                    int h) {
  // Render just the logical rect (x,y,w,h) of the page to a PNG (e.g. an element
  // screenshot); the output is (w*dsf x h*dsf) px. (Port extension.)
  return webView && webView->view && path && w > 0 && h > 0 &&
         mbSavePngRect(webView->view, path, x, y, w, h) != 0;
}

void wkeSetDeviceScaleFactor(wkeWebView webView, float scale) {
  // HiDPI/retina: window.devicePixelRatio reports `scale` and paint/PNG output
  // is rasterized at `scale`x (layout stays in CSS px). Size capture buffers at
  // logical_width*scale x logical_height*scale. (Port extension — modern.)
  if (webView && webView->view && scale > 0.0f)
    mbSetDeviceScaleFactor(webView->view, scale);
}

void wkeSetFollowRedirects(bool follow) {
  // Follow HTTP 3xx redirects (default) or stop at the redirect response, so a
  // caller can inspect a 30x's status/Location (e.g. resolve a URL shortener)
  // without following it. Process-wide; set before navigating. (Port extension.)
  mbSetFollowRedirects(follow ? 1 : 0);
}

void wkeSetIgnoreCertErrors(bool ignore) {
  // Accept invalid TLS certificates (self-signed/expired) — the equivalent of
  // curl -k / Puppeteer ignoreHTTPSErrors; false restores the secure default.
  // Process-wide; set before navigating. (Port extension.)
  mbSetIgnoreCertErrors(ignore ? 1 : 0);
}

void wkeScrollTo(wkeWebView webView, int x, int y) {
  // Absolute scroll of the layout viewport to (x,y) in CSS px (window.scrollTo).
  // The real viewport moves, so fixed/sticky elements render correctly — pair
  // with wkeSavePng/wkePaint for a viewport shot of a long page. (Port extension.)
  if (webView && webView->view)
    mbScrollTo(webView->view, x, y);
}

int wkeEncodePng(wkeWebView webView, int width, int height,
                 const unsigned char** outData) {
  // Render the current frame to a width x height PNG held in memory (no temp
  // file) — for embedders that serve the bytes. Returns the length and sets
  // *outData; 0 on failure. The bytes are owned by the view and valid only until
  // the next wkeEncodePng on it or wkeDestroyWebView — copy them out. (Port ext.)
  if (!webView || !webView->view || width <= 0 || height <= 0 || !outData)
    return 0;
  return mbEncodePng(webView->view, width, height, outData);
}

// --- DOM query helpers (scrape without writing JS) -----------------------------
int wkeCountSelector(wkeWebView webView, const char* selector) {
  // querySelectorAll length (>=0; 0 is valid), or -1 for a null/invalid selector.
  if (!webView || !webView->view || !selector)
    return -1;
  return mbCountSelector(webView->view, selector);
}

const utf8* wkeGetTextForSelector(wkeWebView webView, const char* selector) {
  // innerText of the FIRST element matching `selector` ("" if none). Owned by
  // the view, valid until the next wkeGetTextForSelector on it. (Port extension.)
  if (!webView || !webView->view || !selector) {
    if (webView)
      webView->selector_text_cache.clear();
    return webView ? webView->selector_text_cache.c_str() : "";
  }
  const int len = mbGetTextForSelector(webView->view, selector, nullptr, 0);
  if (len <= 0) {
    webView->selector_text_cache.clear();
    return webView->selector_text_cache.c_str();
  }
  std::vector<char> buf(static_cast<size_t>(len) + 1, 0);
  mbGetTextForSelector(webView->view, selector, buf.data(), len + 1);
  webView->selector_text_cache.assign(buf.data());
  return webView->selector_text_cache.c_str();
}

const utf8* wkeGetAttribute(wkeWebView webView, const char* selector,
                            const char* attr) {
  // Value of `attr` on the FIRST element matching `selector` ("" if no match or
  // the attribute is absent). Owned by the view, valid until the next
  // wkeGetAttribute on it. Property reads (.value/.checked) come via wkeRunJS.
  if (!webView || !webView->view || !selector || !attr) {
    if (webView)
      webView->selector_attr_cache.clear();
    return webView ? webView->selector_attr_cache.c_str() : "";
  }
  const int len = mbGetAttribute(webView->view, selector, attr, nullptr, 0);
  if (len <= 0) {  // -1 absent / no match, or 0 empty value
    webView->selector_attr_cache.clear();
    return webView->selector_attr_cache.c_str();
  }
  std::vector<char> buf(static_cast<size_t>(len) + 1, 0);
  mbGetAttribute(webView->view, selector, attr, buf.data(), len + 1);
  webView->selector_attr_cache.assign(buf.data());
  return webView->selector_attr_cache.c_str();
}

const utf8* wkeGetComputedStyle(wkeWebView webView, const char* selector,
                                const char* property) {
  // Resolved computed value of CSS `property` for the first match (color ->
  // "rgb(r, g, b)", display:none -> "none"); "" if no element matches. Owned by
  // the view, valid until the next wkeGetComputedStyle on it. For visibility /
  // style assertions without writing JS. (Port extension.)
  if (!webView || !webView->view || !selector || !property) {
    if (webView)
      webView->computed_style_cache.clear();
    return webView ? webView->computed_style_cache.c_str() : "";
  }
  const int len =
      mbGetComputedStyle(webView->view, selector, property, nullptr, 0);
  if (len <= 0) {  // -1 no match, or 0 empty value
    webView->computed_style_cache.clear();
    return webView->computed_style_cache.c_str();
  }
  std::vector<char> buf(static_cast<size_t>(len) + 1, 0);
  mbGetComputedStyle(webView->view, selector, property, buf.data(), len + 1);
  webView->computed_style_cache.assign(buf.data());
  return webView->computed_style_cache.c_str();
}

bool wkeGetElementRect(wkeWebView webView, const char* selector, int* x, int* y,
                       int* w, int* h) {
  // Viewport-relative bounding box (logical px) of the first match into *x/*y/
  // *w/*h (any may be NULL). False if nothing matches. Compose with
  // wkeSavePngRect for an element shot or wkeFireMouseEvent for a precise click.
  return webView && webView->view && selector &&
         mbGetElementRect(webView->view, selector, x, y, w, h) != 0;
}

// --- DOM actions (drive the page without writing JS) ---------------------------
bool wkeClickSelector(wkeWebView webView, const char* selector) {
  // Click the first element matching `selector` (resolves its box and dispatches
  // a real click). False if nothing matches or it has no box. (Port extension.)
  return webView && webView->view && selector &&
         mbClickSelector(webView->view, selector) != 0;
}

bool wkeFillSelector(wkeWebView webView, const char* selector,
                     const utf8* text) {
  // Set the value of the first matching input/textarea and fire input+change
  // (so frameworks like React observe it). False if nothing matches. (Port ext.)
  return webView && webView->view && selector && text &&
         mbFillSelector(webView->view, selector, text) != 0;
}

bool wkeSelectOption(wkeWebView webView, const char* selector,
                     const utf8* value) {
  // Choose a <select> option whose value (or visible text) equals `value`,
  // firing input+change. False if no <select> or no matching option. (Port ext.)
  return webView && webView->view && selector && value &&
         mbSelectOption(webView->view, selector, value) != 0;
}

bool wkeScrollIntoView(wkeWebView webView, const char* selector) {
  // Scroll the first matching element into the viewport (to trigger lazy
  // loading or frame it before a screenshot). False if nothing matches. The
  // click/fill selector ops already do this internally. (Port extension.)
  return webView && webView->view && selector &&
         mbScrollIntoView(webView->view, selector) != 0;
}

// Additional pointer/focus actions on the first selector match (each false if
// nothing matches). Hover fires mouseover/enter; double/right-click fire dblclick
// /contextmenu; focus/blur move the active element. (Port extensions.)
bool wkeHoverSelector(wkeWebView webView, const char* selector) {
  return webView && webView->view && selector &&
         mbHoverSelector(webView->view, selector) != 0;
}
bool wkeDoubleClickSelector(wkeWebView webView, const char* selector) {
  return webView && webView->view && selector &&
         mbDoubleClickSelector(webView->view, selector) != 0;
}
bool wkeRightClickSelector(wkeWebView webView, const char* selector) {
  return webView && webView->view && selector &&
         mbRightClickSelector(webView->view, selector) != 0;
}
bool wkeFocusSelector(wkeWebView webView, const char* selector) {
  return webView && webView->view && selector &&
         mbFocusSelector(webView->view, selector) != 0;
}
bool wkeBlurSelector(wkeWebView webView, const char* selector) {
  return webView && webView->view && selector &&
         mbBlurSelector(webView->view, selector) != 0;
}

// --- Waits (pump the loop until a condition or timeout) ------------------------
bool wkeWaitForSelector(wkeWebView webView, const char* selector,
                        int timeoutMs) {
  // Pump until the first element matching `selector` exists, or timeoutMs
  // elapses. True if it appeared (Puppeteer-style waitForSelector). (Port ext.)
  return webView && webView->view && selector &&
         mbWaitForSelector(webView->view, selector, timeoutMs) != 0;
}

bool wkeWaitForFunction(wkeWebView webView, const utf8* jsExpr, int timeoutMs) {
  // Pump until `jsExpr` evaluates truthy (exceptions count as false), or
  // timeoutMs elapses. True if it became truthy. Generalizes the selector wait.
  return webView && webView->view && jsExpr &&
         mbWaitForFunction(webView->view, jsExpr, timeoutMs) != 0;
}

void wkeSetTransparent(wkeWebView webView, bool transparent) {
  if (!webView || !webView->view)
    return;
  webView->transparent = transparent;  // mirror for wkeIsTransparent
  mbSetTransparentBackground(webView->view, transparent ? 1 : 0);
}

bool wkeIsTransparent(wkeWebView webView) {
  return webView && webView->transparent;
}

// --- Pure wke view-state (app name + app-owned key/value store) -----------------
void wkeSetName(wkeWebView webView, const char* name) {
  if (webView)
    webView->name = name ? name : "";
}

const char* wkeGetName(wkeWebView webView) {
  return webView ? webView->name.c_str() : "";
}

void wkeSetUserKeyValue(wkeWebView webView, const char* key, void* value) {
  if (webView && key)
    webView->user_kv[key] = value;
}

void* wkeGetUserKeyValue(wkeWebView webView, const char* key) {
  if (!webView || !key)
    return nullptr;
  auto it = webView->user_kv.find(key);
  return it == webView->user_kv.end() ? nullptr : it->second;
}

void wkeSetZoomFactor(wkeWebView webView, float factor) {
  if (!webView)
    return;
  webView->zoom_factor = factor > 0.0f ? factor : 1.0;  // ignore non-positive
  ApplyZoom(webView);  // apply to the current document now (and again on reload)
}

float wkeGetZoomFactor(wkeWebView webView) {
  return webView ? static_cast<float>(webView->zoom_factor) : 1.0f;
}

void wkeSetEditable(wkeWebView webView, bool editable) {
  if (!webView || !webView->view)
    return;
  webView->editable = editable;  // persisted across loads via ApplyEditable
  char buf[8] = {0};
  mbEvalJS(webView->view,
           editable ? "try{document.designMode='on'}catch(e){}"
                    : "try{document.designMode='off'}catch(e){}",
           buf, sizeof(buf));
}

const utf8* wkeGetSource(wkeWebView webView) {
  if (!webView || !webView->view)
    return "";
  const int len = mbGetHTML(webView->view, nullptr, 0);  // size first (pages vary)
  std::vector<char> buf(static_cast<size_t>(len) + 1, 0);
  mbGetHTML(webView->view, buf.data(), len + 1);
  webView->source_cache.assign(buf.data());
  return webView->source_cache.c_str();
}

int wkeGetHttpStatusCode(wkeWebView webView) {
  // The main document's final HTTP status (e.g. 200/404/30x), or 0 for a
  // non-http load (loadHTML/file/data) or a failed fetch. (Port extension.)
  return (webView && webView->view) ? mbGetHttpStatus(webView->view) : 0;
}

const utf8* wkeGetResponseHeaders(wkeWebView webView) {
  // The main document's raw HTTP response headers ("" for a non-http/failed
  // load). Owned by the view, valid until the next call on it. (Port extension.)
  if (!webView || !webView->view)
    return "";
  const int len = mbGetResponseHeaders(webView->view, nullptr, 0);  // size first
  if (len <= 0) {
    webView->response_headers_cache.clear();
    return webView->response_headers_cache.c_str();
  }
  std::vector<char> buf(static_cast<size_t>(len) + 1, 0);
  mbGetResponseHeaders(webView->view, buf.data(), len + 1);
  webView->response_headers_cache.assign(buf.data());
  return webView->response_headers_cache.c_str();
}

const utf8* wkeGetText(wkeWebView webView) {
  // The page's visible text (document.body.innerText). Owned by the view, valid
  // until the next wkeGetText on it. (Companion to wkeGetSource's HTML.)
  if (!webView || !webView->view)
    return "";
  const int len = mbGetText(webView->view, nullptr, 0);  // size first
  if (len <= 0) {
    webView->text_cache.clear();
    return webView->text_cache.c_str();
  }
  std::vector<char> buf(static_cast<size_t>(len) + 1, 0);
  mbGetText(webView->view, buf.data(), len + 1);
  webView->text_cache.assign(buf.data());
  return webView->text_cache.c_str();
}

// --- Cookies (wrap the libcurl jar via mb_capi) --------------------------------
const utf8* wkeGetCookie(wkeWebView webView) {
  if (!webView || !webView->view)
    return "";
  char url[4096] = {0};
  mbGetURL(webView->view, url, sizeof(url));  // cookies for the current document
  std::vector<char> buf(1 << 14, 0);          // 16 KiB; cookie strings are small
  mbGetCookies(webView->view, url, buf.data(), static_cast<int>(buf.size()));
  webView->cookie_cache.assign(buf.data());
  return webView->cookie_cache.c_str();
}

void wkeSetCookie(wkeWebView webView, const utf8* url, const utf8* cookie) {
  // `cookie` is a single "name=value[; Path=/; Domain=...]" set-cookie string.
  if (webView && webView->view && url && cookie)
    mbSetCookie(webView->view, url, cookie);
}

void wkeSetCookieJarPath(wkeWebView /*webView*/, const utf8* path) {
  // Process-wide jar path (this port uses a utf8 path, not the Win wke WCHAR).
  // Drives the Flush/Reload cookie commands below.
  CookieJarPath() = path ? path : "";
}

void wkePerformCookieCommand(wkeCookieCommand command) {
  // The jar is process-wide. The two clear commands map to a full jar reset
  // (driven through the last live webView; we don't distinguish session vs
  // persistent cookies). Flush/Reload persist to/from the path set by
  // wkeSetCookieJarPath (no-op until one is configured).
  switch (command) {
    case wkeCookieCommandClearAllCookies:
    case wkeCookieCommandClearSessionCookies:
      if (g_last_webview && g_last_webview->view)
        mbClearCookies(g_last_webview->view);
      return;
    case wkeCookieCommandFlushCookiesToFile:
      if (!CookieJarPath().empty())
        mbSaveCookies(CookieJarPath().c_str());
      return;
    case wkeCookieCommandReloadCookiesFromFile:
      if (!CookieJarPath().empty())
        mbLoadCookies(CookieJarPath().c_str());
      return;
  }
}

void wkeSetProxy(const wkeProxy* proxy) {
  // Build a curl proxy URL "scheme://[user[:pass]@]host:port" from the struct;
  // mbSetProxy hands it straight to CURLOPT_PROXY (which parses the scheme).
  if (!proxy || proxy->type == WKE_PROXY_NONE || proxy->hostname[0] == '\0') {
    mbSetProxy("");  // direct connection
    return;
  }
  const char* scheme = "http://";
  switch (proxy->type) {
    case WKE_PROXY_HTTP: scheme = "http://"; break;
    case WKE_PROXY_SOCKS4: scheme = "socks4://"; break;
    case WKE_PROXY_SOCKS4A: scheme = "socks4a://"; break;
    case WKE_PROXY_SOCKS5: scheme = "socks5://"; break;
    case WKE_PROXY_SOCKS5HOSTNAME: scheme = "socks5h://"; break;
    case WKE_PROXY_NONE: break;  // handled above
  }
  // The struct's char buffers may not be NUL-terminated if filled to capacity;
  // copy into a +1 buffer and terminate before treating them as C strings.
  char host[101], user[51], pass[51];
  std::memcpy(host, proxy->hostname, 100); host[100] = '\0';
  std::memcpy(user, proxy->username, 50); user[50] = '\0';
  std::memcpy(pass, proxy->password, 50); pass[50] = '\0';
  std::string url = scheme;
  if (user[0]) {
    url += user;
    if (pass[0]) {
      url += ":";
      url += pass;
    }
    url += "@";
  }
  url += host;
  url += ":";
  url += std::to_string(proxy->port);
  mbSetProxy(url.c_str());
}

bool wkeFireMouseEvent(wkeWebView webView, unsigned int message, int x, int y,
                       unsigned int /*flags*/) {
  if (!webView || !webView->view)
    return false;
  switch (message) {
    case WKE_MSG_MOUSEMOVE:
      mbSendMouseMove(webView->view, x, y);
      return true;
    case WKE_MSG_LBUTTONDOWN:
      // The click is delivered on the matching LBUTTONUP (mbSendMouseClick does
      // its own press+release), so the press alone is a no-op.
      return true;
    case WKE_MSG_LBUTTONUP:
      mbSendMouseClick(webView->view, x, y);
      mbWait(webView->view, 20);  // let the click's handlers/layout settle
      return true;
    case WKE_MSG_LBUTTONDBLCLK:
      mbSendMouseClick(webView->view, x, y);
      mbSendMouseClick(webView->view, x, y);  // approximate a double click
      mbWait(webView->view, 20);
      return true;
    default:
      return false;  // other buttons / wheel not in this slice
  }
}

namespace {
// Encode a Unicode code point as UTF-8 (mbSendText takes UTF-8).
std::string CodePointToUtf8(unsigned int cp) {
  std::string s;
  if (cp < 0x80) {
    s += static_cast<char>(cp);
  } else if (cp < 0x800) {
    s += static_cast<char>(0xC0 | (cp >> 6));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    s += static_cast<char>(0xE0 | (cp >> 12));
    s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    s += static_cast<char>(0xF0 | (cp >> 18));
    s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  }
  return s;
}
// Map a Win32 virtual-key code to the key name mbSendKey understands, or null for
// a plain character key (whose insertion comes via wkeFireKeyPressEvent instead).
const char* VkToKeyName(unsigned int vk) {
  switch (vk) {
    case 0x0D: return "Enter";
    case 0x09: return "Tab";
    case 0x1B: return "Escape";
    case 0x08: return "Backspace";
    case 0x2E: return "Delete";
    case 0x25: return "ArrowLeft";
    case 0x26: return "ArrowUp";
    case 0x27: return "ArrowRight";
    case 0x28: return "ArrowDown";
    case 0x24: return "Home";
    case 0x23: return "End";
    case 0x21: return "PageUp";
    case 0x22: return "PageDown";
    default: return nullptr;
  }
}
}  // namespace

bool wkeFireMouseWheelEvent(wkeWebView webView, int x, int y, int delta,
                            unsigned int /*flags*/) {
  if (!webView || !webView->view)
    return false;
  // Win32 wheel delta is positive-up; mbSendScroll's dy is positive-down.
  mbSendScroll(webView->view, x, y, 0, -delta);
  mbWait(webView->view, 20);  // let the scroll apply before the next read/paint
  return true;
}

bool wkeFireKeyDownEvent(wkeWebView webView, unsigned int virtualKeyCode,
                         unsigned int /*flags*/, bool /*systemKey*/) {
  if (!webView || !webView->view)
    return false;
  if (const char* name = VkToKeyName(virtualKeyCode)) {
    mbSendKey(webView->view, name);  // a special key's default action
    mbWait(webView->view, 20);
  }
  // A plain character key is delivered by the matching wkeFireKeyPressEvent.
  return true;
}

bool wkeFireKeyUpEvent(wkeWebView webView, unsigned int /*virtualKeyCode*/,
                       unsigned int /*flags*/, bool /*systemKey*/) {
  return webView && webView->view;  // no-op (the down/press did the work)
}

bool wkeFireKeyPressEvent(wkeWebView webView, unsigned int charCode,
                          unsigned int /*flags*/, bool /*systemKey*/) {
  if (!webView || !webView->view || charCode == 0)
    return false;
  mbSendText(webView->view, CodePointToUtf8(charCode).c_str());
  return true;
}

void wkePaint(wkeWebView webView, void* bits, int pitch) {
  if (!webView || !webView->view || !bits)
    return;
  const int stride = pitch > 0 ? pitch : webView->width * 4;
  mbPaintToBitmap(webView->view, bits, webView->width, webView->height, stride);
}

// --- Scripting (string-backed jsValue) -----------------------------------------
namespace {
// jsValue handle -> the coerced-to-string result + its JS type. Heap-owned with
// no destructor (Blink builds with -Wexit-time-destructors).
struct JsRecord {
  std::string value;
  std::string type;     // "number"/"string"/.../"array"/"null" from mbEvalJSEx
  std::string literal;  // a JS expression reproducing this value: a slot ref
                        // "window.__mbslots[h]" or a primitive literal ("5"/"x").
};
std::map<jsValue, JsRecord>& JsRegistry() {
  static auto* r = new std::map<jsValue, JsRecord>();
  return *r;
}
std::string& JsTempBuf() {
  static auto* s = new std::string();
  return *s;
}
std::string& JsStringBuf() {  // separate temp for jsToString (JSON view)
  static auto* s = new std::string();
  return *s;
}
// Start above wke's small reserved constants (jsUndefined/jsNull/jsTrue/jsFalse).
jsValue g_next_js_value = 0x10000;

const JsRecord* JsLookup(jsValue v) {
  auto& reg = JsRegistry();
  auto it = reg.find(v);
  return it == reg.end() ? nullptr : &it->second;
}

// A JS expression that reproduces jsValue `v` (for embedding it in a call). For a
// slot-backed value it's the slot ref; for a primitive constructor it's the
// stored literal; unknown handles read as undefined.
std::string LiteralOf(jsValue v) {
  const JsRecord* r = JsLookup(v);
  return (r && !r->literal.empty()) ? r->literal : std::string("undefined");
}

// Register a primitive jsValue (jsInt/jsString/...) with no eval — value, type,
// and the JS literal that reproduces it (so it can be inlined into a jsCall).
jsValue MakeLiteral(const std::string& value, const std::string& type,
                    const std::string& literal) {
  auto& reg = JsRegistry();
  if (reg.size() >= 4096)
    reg.clear();
  const jsValue h = g_next_js_value++;
  reg[h] = JsRecord{value, type, literal};
  return h;
}

// Quote `s` as a JS string literal (for embedding a property name safely).
std::string JsStringLiteral(const char* s) {
  std::string out = "\"";
  for (const char* p = s; p && *p; ++p) {
    switch (*p) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      default: out += *p;
    }
  }
  out += "\"";
  return out;
}

// Eval `script` and record its coerced value + type under a fresh handle, ALSO
// parking the live result in window.__mbslots[handle] so a later eval can index
// it (backs jsGetAt). The slot store is done IN JS (a wrapper assignment) — never
// from C++, which is what crashed a prior attempt. The wrapper only parses if
// `script` is an EXPRESSION; for a statement it's a parse error (nothing runs,
// empty type) and we fall back to a plain eval (no slot). The empty-type check
// avoids re-running a valid expression that merely yields undefined.
jsValue StoreEval(wkeWebView wv, const std::string& script) {
  auto& reg = JsRegistry();
  if (reg.size() >= 4096)
    reg.clear();  // bound the C++ registry; old handles' value/type expire to ""
  const jsValue handle = g_next_js_value++;  // handle id == its __mbslots slot
  const std::string wrapped =
      "window.__mbslots=window.__mbslots||{};window.__mbslots[" +
      std::to_string(handle) + "]=(" + script + ")";
  std::vector<char> value(1 << 16, 0);  // 64 KiB result cap
  char type[16] = {0};
  mbEvalJSEx(wv->view, wrapped.c_str(), value.data(),
             static_cast<int>(value.size()), type, sizeof(type));
  bool slotted = true;
  if (type[0] == '\0') {  // wrapper didn't parse/run (a statement) -> plain eval
    slotted = false;
    std::fill(value.begin(), value.end(), 0);
    mbEvalJSEx(wv->view, script.c_str(), value.data(),
               static_cast<int>(value.size()), type, sizeof(type));
  }
  // A slotted result is navigable/callable via its slot ref; a fallback isn't.
  std::string literal =
      slotted ? ("window.__mbslots[" + std::to_string(handle) + "]")
              : std::string();
  reg[handle] =
      JsRecord{std::string(value.data()), std::string(type), std::move(literal)};
  return handle;
}
}  // namespace

jsValue wkeRunJS(wkeWebView webView, const utf8* script) {
  if (!webView || !webView->view || !script)
    return 0;
  const jsValue handle = StoreEval(webView, std::string(script));
  DrainConsoleToCallback(webView);  // deliver any console output the script logged
  DrainBridgeToCallback(webView);   // deliver any window.mbBridge calls it made
  return handle;
}

jsExecState wkeGlobalExec(wkeWebView webView) {
  // No real exec state is needed — the jsValue handle carries the result. Return
  // a non-null token so callers' null checks pass.
  return reinterpret_cast<jsExecState>(webView);
}

int jsToInt(jsExecState /*es*/, jsValue v) {
  const JsRecord* r = JsLookup(v);
  return r ? std::atoi(r->value.c_str()) : 0;
}

double jsToDouble(jsExecState /*es*/, jsValue v) {
  const JsRecord* r = JsLookup(v);
  return r ? std::atof(r->value.c_str()) : 0.0;
}

float jsToFloat(jsExecState /*es*/, jsValue v) {
  const JsRecord* r = JsLookup(v);
  return r ? static_cast<float>(std::atof(r->value.c_str())) : 0.0f;
}

bool jsToBoolean(jsExecState /*es*/, jsValue v) {
  const JsRecord* r = JsLookup(v);
  if (!r)
    return false;
  const std::string& s = r->value;
  // JS truthiness over the coerced string: "true" / any non-empty value that
  // isn't "false" or "0".
  return s == "true" || (!s.empty() && s != "false" && s != "0");
}

const utf8* jsToTempString(jsExecState /*es*/, jsValue v) {
  const JsRecord* r = JsLookup(v);
  JsTempBuf() = r ? r->value : std::string();
  return JsTempBuf().c_str();
}

const utf8* jsToString(jsExecState es, jsValue v) {
  // Like jsToTempString, but object/array values are JSON-serialized (via the
  // slot) for a useful representation instead of "[object Object]". Owned by the
  // library, valid until the next jsToString call.
  const JsRecord* r = JsLookup(v);
  if (!r) {
    JsStringBuf().clear();
    return JsStringBuf().c_str();
  }
  wkeWebView wv = reinterpret_cast<wkeWebView>(es);
  if (wv && wv->view && !r->literal.empty() &&
      (r->type == "object" || r->type == "array")) {
    std::vector<char> buf(1 << 16, 0);
    const std::string js = "(function(){try{return JSON.stringify(" +
                           r->literal + ")}catch(e){return ''}})()";
    mbEvalJS(wv->view, js.c_str(), buf.data(), static_cast<int>(buf.size()));
    JsStringBuf().assign(buf.data());
    return JsStringBuf().c_str();
  }
  JsStringBuf() = r->value;  // primitives: the coerced value
  return JsStringBuf().c_str();
}

jsType jsTypeOf(jsValue v) {
  const JsRecord* r = JsLookup(v);
  if (!r)
    return JSTYPE_UNDEFINED;
  const std::string& t = r->type;
  if (t == "number")
    return JSTYPE_NUMBER;
  if (t == "string")
    return JSTYPE_STRING;
  if (t == "boolean")
    return JSTYPE_BOOLEAN;
  if (t == "array")
    return JSTYPE_ARRAY;
  if (t == "function")
    return JSTYPE_FUNCTION;
  if (t == "null")
    return JSTYPE_NULL;
  if (t == "object")
    return JSTYPE_OBJECT;
  return JSTYPE_UNDEFINED;
}

// Type predicates over a jsValue (built on jsTypeOf). Arrays report jsIsArray
// (not jsIsObject) in this port; jsIsTrue/jsIsFalse test a boolean's value.
bool jsIsNumber(jsValue v) { return jsTypeOf(v) == JSTYPE_NUMBER; }
bool jsIsString(jsValue v) { return jsTypeOf(v) == JSTYPE_STRING; }
bool jsIsBoolean(jsValue v) { return jsTypeOf(v) == JSTYPE_BOOLEAN; }
bool jsIsObject(jsValue v) { return jsTypeOf(v) == JSTYPE_OBJECT; }
bool jsIsFunction(jsValue v) { return jsTypeOf(v) == JSTYPE_FUNCTION; }
bool jsIsUndefined(jsValue v) { return jsTypeOf(v) == JSTYPE_UNDEFINED; }
bool jsIsNull(jsValue v) { return jsTypeOf(v) == JSTYPE_NULL; }
bool jsIsArray(jsValue v) { return jsTypeOf(v) == JSTYPE_ARRAY; }
bool jsIsTrue(jsValue v) {
  return jsTypeOf(v) == JSTYPE_BOOLEAN && jsToBoolean(nullptr, v);
}
bool jsIsFalse(jsValue v) {
  return jsTypeOf(v) == JSTYPE_BOOLEAN && !jsToBoolean(nullptr, v);
}

int jsGetLength(jsExecState es, jsValue object) {
  wkeWebView wv = reinterpret_cast<wkeWebView>(es);
  if (!wv || !wv->view)
    return 0;
  // Read window.__mbslots[object].length safely (0 if not array / not present).
  const std::string script = "(function(){try{return window.__mbslots[" +
                             std::to_string(object) +
                             "].length}catch(e){return 0}})()";
  char buf[32] = {0};
  mbEvalJS(wv->view, script.c_str(), buf, sizeof(buf));
  return std::atoi(buf);
}

jsValue jsGetAt(jsExecState es, jsValue object, int index) {
  wkeWebView wv = reinterpret_cast<wkeWebView>(es);
  if (!wv || !wv->view)
    return 0;
  // Index into the parked value, returning a fresh navigable jsValue (undefined
  // if out of range / not indexable). The IIFE never throws, so StoreEval's
  // wrapper always parses.
  const std::string expr = "(function(){try{return window.__mbslots[" +
                           std::to_string(object) + "][" +
                           std::to_string(index) +
                           "]}catch(e){return undefined}})()";
  return StoreEval(wv, expr);
}

jsValue jsGet(jsExecState es, jsValue object, const char* prop) {
  wkeWebView wv = reinterpret_cast<wkeWebView>(es);
  if (!wv || !wv->view || !prop)
    return 0;
  // Read window.__mbslots[object][prop] into a fresh navigable handle.
  const std::string expr = "(function(){try{return window.__mbslots[" +
                           std::to_string(object) + "][" + JsStringLiteral(prop) +
                           "]}catch(e){return undefined}})()";
  return StoreEval(wv, expr);
}

jsValue jsGetGlobal(jsExecState es, const char* prop) {
  wkeWebView wv = reinterpret_cast<wkeWebView>(es);
  if (!wv || !wv->view || !prop)
    return 0;
  const std::string expr = "(function(){try{return window[" +
                           JsStringLiteral(prop) +
                           "]}catch(e){return undefined}})()";
  return StoreEval(wv, expr);
}

jsKeys* jsGetKeys(jsExecState es, jsValue object) {
  // Thread-local backing store: holds the key strings (and the const char*
  // array pointing into them) so the returned jsKeys* stays valid until the
  // next call. Rebuilt each invocation.
  struct KeysHolder {
    std::vector<std::string> storage;
    std::vector<const char*> ptrs;
    jsKeys keys;
  };
  static thread_local KeysHolder* holder = new KeysHolder();  // leaked: no dtor
  holder->storage.clear();
  holder->ptrs.clear();
  holder->keys.length = 0;
  holder->keys.keys = nullptr;

  wkeWebView wv = reinterpret_cast<wkeWebView>(es);
  if (!wv || !wv->view)
    return &holder->keys;

  // Park Object.keys(obj) in a slot, then read each name back as a string.
  const jsValue arr =
      StoreEval(wv, "(function(){try{return Object.keys(window.__mbslots[" +
                        std::to_string(object) + "])}catch(e){return []}})()");
  const int n = jsGetLength(es, arr);
  holder->storage.reserve(n);
  for (int i = 0; i < n; ++i) {
    const JsRecord* r = JsLookup(jsGetAt(es, arr, i));
    holder->storage.push_back(r ? r->value : std::string());
  }
  // Build the pointer array only after storage is final (push_back can realloc).
  holder->ptrs.reserve(holder->storage.size());
  for (const std::string& s : holder->storage)
    holder->ptrs.push_back(s.c_str());
  holder->keys.length = static_cast<unsigned int>(holder->ptrs.size());
  holder->keys.keys = holder->ptrs.empty() ? nullptr : holder->ptrs.data();
  return &holder->keys;
}

// --- jsValue constructors (build args to pass INTO JS) -------------------------
jsValue jsInt(int n) {
  return MakeLiteral(std::to_string(n), "number", std::to_string(n));
}
jsValue jsDouble(double d) {
  return MakeLiteral(std::to_string(d), "number", std::to_string(d));
}
jsValue jsBoolean(bool b) {
  return MakeLiteral(b ? "true" : "false", "boolean", b ? "true" : "false");
}
jsValue jsString(jsExecState /*es*/, const utf8* str) {
  const std::string s = str ? str : "";
  return MakeLiteral(s, "string", JsStringLiteral(str ? str : ""));
}
jsValue jsUndefined() {
  return MakeLiteral("undefined", "undefined", "undefined");
}
jsValue jsNull() {
  return MakeLiteral("null", "null", "null");
}

// --- Building structured values (object/array) to pass INTO JS -----------------
jsValue jsEmptyObject(jsExecState es) {
  wkeWebView wv = reinterpret_cast<wkeWebView>(es);
  if (!wv || !wv->view)
    return 0;
  return StoreEval(wv, "({})");  // a fresh, navigable object handle
}

jsValue jsEmptyArray(jsExecState es) {
  wkeWebView wv = reinterpret_cast<wkeWebView>(es);
  if (!wv || !wv->view)
    return 0;
  return StoreEval(wv, "([])");  // a fresh, navigable array handle
}

namespace {
// Mutate the live slot object in place (no new slot). The IIFE never throws, so
// a bad target/value is a silent no-op rather than a script error.
void EvalVoid(wkeWebView wv, const std::string& script) {
  char buf[8] = {0};
  mbEvalJS(wv->view, script.c_str(), buf, sizeof(buf));
}
}  // namespace

void jsSet(jsExecState es, jsValue object, const char* prop, jsValue value) {
  wkeWebView wv = reinterpret_cast<wkeWebView>(es);
  if (!wv || !wv->view || !prop)
    return;
  EvalVoid(wv, "(function(){try{window.__mbslots[" + std::to_string(object) +
                   "][" + JsStringLiteral(prop) + "]=(" + LiteralOf(value) +
                   ")}catch(e){}})()");
}

void jsSetAt(jsExecState es, jsValue object, int index, jsValue value) {
  wkeWebView wv = reinterpret_cast<wkeWebView>(es);
  if (!wv || !wv->view)
    return;
  EvalVoid(wv, "(function(){try{window.__mbslots[" + std::to_string(object) +
                   "][" + std::to_string(index) + "]=(" + LiteralOf(value) +
                   ")}catch(e){}})()");
}

void jsSetGlobal(jsExecState es, const char* prop, jsValue value) {
  wkeWebView wv = reinterpret_cast<wkeWebView>(es);
  if (!wv || !wv->view || !prop)
    return;
  EvalVoid(wv, "(function(){try{window[" + JsStringLiteral(prop) + "]=(" +
                   LiteralOf(value) + ")}catch(e){}})()");
}

// --- Calling JS functions ------------------------------------------------------
namespace {
std::string ArgList(jsValue* args, int argCount) {
  std::string a;
  for (int i = 0; i < argCount; ++i) {
    if (i)
      a += ",";
    a += LiteralOf(args[i]);
  }
  return a;
}
}  // namespace

jsValue jsCallGlobal(jsExecState es, jsValue func, jsValue* args, int argCount) {
  wkeWebView wv = reinterpret_cast<wkeWebView>(es);
  if (!wv || !wv->view)
    return 0;
  const std::string expr = "(function(){try{return (" + LiteralOf(func) + ")(" +
                           ArgList(args, argCount) + ")}catch(e){return undefined}})()";
  return StoreEval(wv, expr);
}

jsValue jsCall(jsExecState es, jsValue func, jsValue thisObject, jsValue* args,
               int argCount) {
  wkeWebView wv = reinterpret_cast<wkeWebView>(es);
  if (!wv || !wv->view)
    return 0;
  const std::string expr = "(function(){try{return (" + LiteralOf(func) +
                           ").apply(" + LiteralOf(thisObject) + ",[" +
                           ArgList(args, argCount) +
                           "])}catch(e){return undefined}})()";
  return StoreEval(wv, expr);
}

// --- Callbacks -----------------------------------------------------------------
const utf8* wkeGetString(const wkeString string) {
  return string ? string->s.c_str() : "";
}

void wkeOnTitleChanged(wkeWebView webView, wkeTitleChangedCallback callback,
                       void* callbackParam) {
  if (!webView)
    return;
  webView->on_title_changed = callback;
  webView->title_param = callbackParam;
}

void wkeOnLoadingFinish(wkeWebView webView, wkeLoadingFinishCallback callback,
                        void* param) {
  if (!webView)
    return;
  webView->on_loading_finish = callback;
  webView->loading_param = param;
}

void wkeOnConsole(wkeWebView webView, wkeConsoleCallback callback, void* param) {
  if (!webView)
    return;
  webView->on_console = callback;
  webView->console_param = param;
}

void wkeOnDocumentReady(wkeWebView webView, wkeDocumentReadyCallback callback,
                        void* param) {
  if (!webView)
    return;
  webView->on_document_ready = callback;
  webView->document_ready_param = param;
}
