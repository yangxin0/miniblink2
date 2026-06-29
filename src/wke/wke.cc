// wke.cc — implementation of the wke compatibility slice over mb_capi.
//
// Each wke function is a thin wrapper over the modern mb_capi host. The wkeWebView
// handle wraps an mbView* plus the small per-view state wke exposes synchronously
// (size, the last load's scheme for success reporting, and string caches for the
// const utf8* getters).

#include "wke/wke.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdio>
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
  int width = 0;            // LOGICAL (CSS px) viewport size — what Resize sets
  int height = 0;
  float device_scale = 1.0f;  // HiDPI/retina device pixel ratio (wkeSetDeviceScaleFactor);
                              // wkePaint rasters into a width*scale x height*scale buffer
  bool last_was_http = false;  // success reporting: http uses the status code
  bool did_load = false;       // a navigation has completed at least once (sync model)
  std::string url_cache;       // backs wkeGetURL's const utf8* return
  std::string title_cache;     // backs wkeGetTitle's const utf8* return
  std::string ua_cache;        // backs wkeGetUserAgent's const utf8* return
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
  wkeAlertBoxCallback on_alert = nullptr;
  void* alert_param = nullptr;
  wkeConfirmBoxCallback on_confirm = nullptr;
  void* confirm_param = nullptr;
  wkePromptBoxCallback on_prompt = nullptr;
  void* prompt_param = nullptr;
  wkeNavigationCallback on_navigation = nullptr;
  void* navigation_param = nullptr;
  wkeURLChangedCallback on_url_changed = nullptr;
  void* url_changed_param = nullptr;
  wkeDownloadCallback on_download = nullptr;
  void* download_param = nullptr;
  std::string user_init_script;  // wkeSetInitScript; combined with the bridge bootstrap
  std::string bridge_channel_cache;  // backs the callback's channel arg
  std::string bridge_message_cache;  // backs the callback's message arg
  std::string source_cache;  // backs wkeGetSource's const utf8* return
  std::string cookie_cache;  // backs wkeGetCookie's const utf8* return
  std::string all_cookie_cache;  // backs wkeGetAllCookie's const utf8* return
  // Pure wke view-state (not backed by the engine): an app-name, a transparent
  // flag mirror, and an app-owned key/value store threaded through callbacks.
  std::string name;
  bool transparent = false;
  std::map<std::string, void*> user_kv;
  double zoom_factor = 1.0;  // wkeSetZoomFactor; re-applied after each load
  bool editable = false;     // wkeSetEditable; re-applied after each load
  std::string selector_text_cache;  // backs wkeGetTextForSelector's return
  std::string selector_alltext_cache;  // backs wkeGetAllTextForSelector's return
  std::string selector_html_cache;  // backs wkeGetHtmlForSelector's return
  std::string selector_allattr_cache;  // backs wkeGetAllAttributeForSelector's return
  std::string selector_allvalue_cache;  // backs wkeGetAllValueForSelector's return
  std::string local_storage_cache;  // backs wkeGetLocalStorage's return
  std::string session_storage_cache;  // backs wkeGetSessionStorage's return
  std::string request_log_cache;  // backs wkeGetRequestLog's return
  std::string selector_attr_cache;  // backs wkeGetAttribute's return
  std::string selector_value_cache; // backs wkeGetValueForSelector's return
  std::string computed_style_cache;  // backs wkeGetComputedStyle's return
  std::string isolated_cache;  // backs wkeRunJsInIsolatedWorld's return
  std::string text_cache;            // backs wkeGetText's return
  std::string response_headers_cache;  // backs wkeGetResponseHeaders's return
};

// Live webViews (newest last), so the view-less wkePerformCookieCommand always
// has a handle to drive the (process-wide) cookie jar even after some views are
// destroyed. Push on create, erase on destroy.
namespace {
std::vector<wkeWebView>& LiveViews() {
  static auto* v = new std::vector<wkeWebView>();  // leaked: no exit-time dtor
  return *v;
}
// Any live view (the most recent), or null if none remain.
wkeWebView AnyLiveView() {
  return LiveViews().empty() ? nullptr : LiveViews().back();
}
void ForgetBindingsForView(wkeWebView wv);  // defined with the binding storage
void ClearNetHooksForView(wkeWebView wv);  // defined with the net-hook storage
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
  // 1 MiB per-drain cap. mbDrainConsole is destructive (it clears the host buffer),
  // so a single burst larger than this loses the overflow — generous enough that a
  // real page's console output between drains fits. (Embedded NULs would also cut
  // the string early, but console text doesn't contain them.)
  std::vector<char> buf(1 << 20, 0);
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
  std::vector<char> buf(1 << 20, 0);  // 1 MiB per-drain cap (see DrainConsoleToCallback)
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

// Deliver any console output / window.mbBridge messages that a just-run page
// event handler (a selector click/fill/etc.) produced — the same drains the
// load and wkeRunJS paths do, so page->host signals from handlers aren't
// stranded until the next eval.
void DrainPageEvents(wkeWebView wv) {
  DrainConsoleToCallback(wv);
  DrainBridgeToCallback(wv);
}

// Re-apply the view's zoom to the current document. This port models wke's page
// zoom as CSS zoom on the document element, which scales layout (and the
// coordinates getBoundingClientRect reports). A factor of 1.0 is a no-op.
void ApplyZoom(wkeWebView wv) {
  if (!wv || !wv->view || wv->zoom_factor == 1.0)
    return;
  // Build the zoom literal with '.'-decimal regardless of LC_NUMERIC (std::to_string
  // honors the locale, so a comma-decimal locale would emit an unparseable "1,5").
  char zoom[32];
  std::snprintf(zoom, sizeof(zoom), "%.17g", wv->zoom_factor);
  const std::string js = "try{document.documentElement.style.zoom='" +
                         std::string(zoom) + "'}catch(e){}";
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
  wv->did_load = true;  // a navigation has now completed (every load path lands here)
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
  LiveViews().push_back(wv);
  return wv;
}

void wkeDestroyWebView(wkeWebView webView) {
  if (!webView)
    return;
  auto& live = LiveViews();
  live.erase(std::remove(live.begin(), live.end(), webView), live.end());
  ForgetBindingsForView(webView);  // free this view's native bindings
  ClearNetHooksForView(webView);   // drop any process-wide net hooks this view owned
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

void wkeLoadHtmlWithBaseUrl(wkeWebView webView, const utf8* html,
                            const utf8* baseUrl) {
  // Like wkeLoadHTML but commits the document at `baseUrl` (the document URL +
  // origin) instead of about:blank — so relative URLs resolve against it and an
  // https:// base makes the page a secure context (Web Crypto subtle, etc.).
  if (!webView || !webView->view)
    return;
  webView->last_was_http = false;
  mbLoadHTML(webView->view, html,
             (baseUrl && baseUrl[0]) ? baseUrl : "about:blank");
  mbWait(webView->view, 30);
  FireLoadCallbacks(webView);
}

void wkeReload(wkeWebView webView) {
  if (webView && webView->view)
    mbReload(webView->view);
}

void wkePostURL(wkeWebView webView, const utf8* url, const char* postData,
                int postLen) {
  if (!webView || !webView->view)
    return;
  webView->last_was_http = true;
  // Honor postLen — binary bodies (multipart/protobuf) may contain NUL bytes, so
  // the length-aware path posts them whole instead of truncating at the first NUL.
  mbPostURLData(webView->view, url, postData, postLen, /*content_type=*/nullptr);
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
  // The load is synchronous here: wkeLoadURL/wkeLoadHTML don't return until the
  // document is committed, so a caller can never observe a load still in flight.
  return false;
}

bool wkeIsLoadingCompleted(wkeWebView webView) {
  // True once a navigation has finished. False on a fresh view before any load
  // (was hardcoded true) — distinguishes "never navigated" from "loaded".
  return webView && webView->did_load;
}

bool wkeIsDocumentReady(wkeWebView webView) {
  // Real document.readyState (was hardcoded true): 'interactive' (DOM parsed) or
  // 'complete' (subresources done) — false if no frame yet or still parsing.
  if (!webView || !webView->view)
    return false;
  char buf[8] = {0};
  mbEvalJS(webView->view,
           "(document.readyState==='complete'||document.readyState==="
           "'interactive')?'1':'0'",
           buf, sizeof(buf));
  return buf[0] == '1';
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

const utf8* wkeGetUserAgent(wkeWebView webView) {
  if (!webView || !webView->view)
    return "";
  char buf[1024] = {0};
  mbGetUserAgent(webView->view, buf, sizeof(buf));
  webView->ua_cache.assign(buf);
  return webView->ua_cache.c_str();
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

// Editor clipboard/selection/history commands (miniblink49 parity). Each maps to the
// matching blink editing command via mbExecuteEditCommand. SelectAll/Copy work on any
// page's selection; Cut/Paste/Delete/Undo/Redo act on the focused editable.
void wkeEditorSelectAll(wkeWebView webView) {
  if (webView && webView->view)
    mbExecuteEditCommand(webView->view, "SelectAll");
}
void wkeEditorUnSelect(wkeWebView webView) {
  if (webView && webView->view)
    mbExecuteEditCommand(webView->view, "Unselect");
}
void wkeEditorCopy(wkeWebView webView) {
  if (webView && webView->view)
    mbExecuteEditCommand(webView->view, "Copy");
}
void wkeEditorCut(wkeWebView webView) {
  if (webView && webView->view)
    mbExecuteEditCommand(webView->view, "Cut");
}
void wkeEditorPaste(wkeWebView webView) {
  if (webView && webView->view)
    mbExecuteEditCommand(webView->view, "Paste");
}
void wkeEditorDelete(wkeWebView webView) {
  if (webView && webView->view)
    mbExecuteEditCommand(webView->view, "Delete");
}
void wkeEditorUndo(wkeWebView webView) {
  if (webView && webView->view)
    mbExecuteEditCommand(webView->view, "Undo");
}
void wkeEditorRedo(wkeWebView webView) {
  if (webView && webView->view)
    mbExecuteEditCommand(webView->view, "Redo");
}

// Classic miniblink49 names for the editor commands above (real apps call these
// unprefixed forms). wkeSelectAll selects the whole document — handy for
// "select-all then copy/read" text scraping; the rest act on the focused
// editable. Thin aliases over the same editor command path.
void wkeSelectAll(wkeWebView webView) {
  if (webView && webView->view)
    mbExecuteEditCommand(webView->view, "SelectAll");
}
void wkeCopy(wkeWebView webView) {
  if (webView && webView->view)
    mbExecuteEditCommand(webView->view, "Copy");
}
void wkeCut(wkeWebView webView) {
  if (webView && webView->view)
    mbExecuteEditCommand(webView->view, "Cut");
}
void wkePaste(wkeWebView webView) {
  if (webView && webView->view)
    mbExecuteEditCommand(webView->view, "Paste");
}
void wkeDelete(wkeWebView webView) {
  if (webView && webView->view)
    mbExecuteEditCommand(webView->view, "Delete");
}

// Abort the current load (classic miniblink49 API). No-op when already idle.
void wkeStopLoading(wkeWebView webView) {
  if (webView && webView->view)
    mbStopLoading(webView->view);
}

// Classic alias for wkeIsLoadingCompleted (the load finished, success or fail).
bool wkeIsLoadComplete(wkeWebView webView) {
  return wkeIsLoadingCompleted(webView);
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

bool wkeInsertCSS(wkeWebView webView, const utf8* css) {
  // Append a <style> with `css` to the document head; true on success. (Port ext.)
  return webView && webView->view && css &&
         mbInsertCSS(webView->view, css) != 0;
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

void wkeSetString(wkeString string, const utf8* str, size_t len) {
  if (!string)
    return;
  if (!str)
    string->s.clear();
  else if (len == static_cast<size_t>(-1))
    string->s.assign(str);
  else
    string->s.assign(str, len);
}

namespace {
// Routes the host JS-dialog callback (mbSetJsDialogCallback) to the per-view wke
// alert/confirm/prompt callbacks. userdata is the wkeWebView. type: 0 alert, 1 confirm,
// 2 prompt; returns accept(1)/dismiss(0); for an accepted prompt writes the text to `out`.
int WkeDialogRouter(int type, const char* message, const char* default_value,
                    char* out, int out_cap, void* userdata) {
  auto* wv = static_cast<wkeWebView>(userdata);
  if (!wv)
    return type == 0 ? 1 : 0;
  _tagWkeString msg{message ? std::string(message) : std::string()};
  if (type == 0) {  // alert
    if (wv->on_alert)
      wv->on_alert(wv, wv->alert_param, &msg);
    return 1;
  }
  if (type == 1) {  // confirm
    return (wv->on_confirm && wv->on_confirm(wv, wv->confirm_param, &msg)) ? 1 : 0;
  }
  // prompt
  if (!wv->on_prompt)
    return 0;
  _tagWkeString def{default_value ? std::string(default_value) : std::string()};
  _tagWkeString result{std::string()};
  const bool accept =
      wv->on_prompt(wv, wv->prompt_param, &msg, &def, &result);
  if (accept && out && out_cap > 0)
    std::snprintf(out, static_cast<size_t>(out_cap), "%s", result.s.c_str());
  return accept ? 1 : 0;
}
// Install the host router on the view (idempotent) whenever any wke dialog cb is set.
void EnsureDialogRouter(wkeWebView wv) {
  if (wv && wv->view)
    mbSetJsDialogCallback(wv->view, &WkeDialogRouter, wv);
}
}  // namespace

void wkeOnAlertBox(wkeWebView webView, wkeAlertBoxCallback callback, void* param) {
  if (!webView)
    return;
  webView->on_alert = callback;
  webView->alert_param = param;
  EnsureDialogRouter(webView);
}

void wkeOnConfirmBox(wkeWebView webView, wkeConfirmBoxCallback callback,
                     void* param) {
  if (!webView)
    return;
  webView->on_confirm = callback;
  webView->confirm_param = param;
  EnsureDialogRouter(webView);
}

void wkeOnPromptBox(wkeWebView webView, wkePromptBoxCallback callback,
                    void* param) {
  if (!webView)
    return;
  webView->on_prompt = callback;
  webView->prompt_param = param;
  EnsureDialogRouter(webView);
}

namespace {
// Routes the host navigation policy callback (mbOnNavigation) to the per-view wke
// callback. Returns allow(1)/cancel(0). userdata is the wkeWebView.
int WkeNavRouter(mbView* /*view*/, void* userdata, const char* url) {
  auto* wv = static_cast<wkeWebView>(userdata);
  if (!wv || !wv->on_navigation)
    return 1;  // allow
  _tagWkeString u{url ? std::string(url) : std::string()};
  return wv->on_navigation(wv, wv->navigation_param,
                           WKE_NAVIGATION_TYPE_OTHER, &u)
             ? 1
             : 0;
}
}  // namespace

void wkeOnNavigation(wkeWebView webView, wkeNavigationCallback callback,
                     void* param) {
  if (!webView || !webView->view)
    return;
  webView->on_navigation = callback;
  webView->navigation_param = param;
  mbOnNavigation(webView->view, callback ? &WkeNavRouter : nullptr, webView);
}

namespace {
// Routes the host URL-changed notification (mbOnUrlChanged) to the per-view wke callback.
void WkeUrlChangedRouter(mbView* /*view*/, void* userdata, const char* url) {
  auto* wv = static_cast<wkeWebView>(userdata);
  if (!wv || !wv->on_url_changed)
    return;
  _tagWkeString u{url ? std::string(url) : std::string()};
  wv->on_url_changed(wv, wv->url_changed_param, &u);
}
}  // namespace

void wkeOnURLChanged(wkeWebView webView, wkeURLChangedCallback callback,
                     void* param) {
  if (!webView || !webView->view)
    return;
  webView->on_url_changed = callback;
  webView->url_changed_param = param;
  mbOnUrlChanged(webView->view, callback ? &WkeUrlChangedRouter : nullptr, webView);
}

namespace {
// Routes the host download diversion (mbOnDownload) to the per-view wke callback, which
// (per the miniblink signature) gets the URL only — the bytes are available via the
// richer mb_capi mbOnDownload for embedders that need them. The wke callback's return is
// advisory; this host has already diverted the response (not rendered it).
void WkeDownloadRouter(mbView* /*view*/, void* userdata, const char* url,
                       const char* /*mime*/, const char* /*filename*/,
                       const char* /*data*/, int /*len*/) {
  auto* wv = static_cast<wkeWebView>(userdata);
  if (wv && wv->on_download)
    wv->on_download(wv, wv->download_param, url ? url : "");
}
}  // namespace

void wkeOnDownload(wkeWebView webView, wkeDownloadCallback callback, void* param) {
  if (!webView || !webView->view)
    return;
  webView->on_download = callback;
  webView->download_param = param;
  mbOnDownload(webView->view, callback ? &WkeDownloadRouter : nullptr, webView);
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

bool wkeSaveElementPng(wkeWebView webView, const char* selector,
                       const utf8* path) {
  // Screenshot just the first match (scroll into view + clip its box) to `path`.
  // True on success. (Port extension — Puppeteer elementHandle.screenshot.)
  return webView && webView->view && selector && path &&
         mbSaveElementPng(webView->view, selector, path) != 0;
}

void wkeSetDeviceScaleFactor(wkeWebView webView, float scale) {
  // HiDPI/retina: window.devicePixelRatio reports `scale` and paint/PNG output
  // is rasterized at `scale`x (layout stays in CSS px). Size capture buffers at
  // logical_width*scale x logical_height*scale. (Port extension — modern.)
  if (webView && webView->view && scale > 0.0f) {
    webView->device_scale = scale;  // wkePaint rasters into width*scale buffers
    mbSetDeviceScaleFactor(webView->view, scale);
  }
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

int wkeScrollToBottom(wkeWebView webView, int maxSteps) {
  // Auto-scroll to the bottom, settling each step so lazy/IntersectionObserver
  // content loads, until the page stops growing or maxSteps. Returns the number
  // of steps that grew the page. (Port extension.)
  if (!webView || !webView->view)
    return 0;
  return mbScrollToBottom(webView->view, maxSteps);
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

int wkeGetCheckedForSelector(wkeWebView webView, const char* selector) {
  // .checked of the first match: 1 checked, 0 unchecked, -1 no match / not checkable.
  if (!webView || !webView->view || !selector)
    return -1;
  return mbGetCheckedForSelector(webView->view, selector);
}

int wkeIsVisibleForSelector(wkeWebView webView, const char* selector) {
  // Visibility of the first match: 1 visible, 0 hidden, -1 no match.
  if (!webView || !webView->view || !selector)
    return -1;
  return mbIsVisibleForSelector(webView->view, selector);
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

bool wkeSetHtmlForSelector(wkeWebView webView, const char* selector,
                           const char* html) {
  // Set the first match's innerHTML; true if matched. (Port extension.)
  return webView && webView->view && selector &&
         mbSetHtmlForSelector(webView->view, selector, html) != 0;
}

const utf8* wkeGetHtmlForSelector(wkeWebView webView, const char* selector) {
  // outerHTML of the FIRST match ("" if none). Owned by the view until the next
  // call. (Port extension.)
  if (!webView || !webView->view || !selector) {
    if (webView)
      webView->selector_html_cache.clear();
    return webView ? webView->selector_html_cache.c_str() : "";
  }
  const int len = mbGetHtmlForSelector(webView->view, selector, nullptr, 0);
  if (len <= 0) {  // -1 no match
    webView->selector_html_cache.clear();
    return webView->selector_html_cache.c_str();
  }
  std::vector<char> buf(static_cast<size_t>(len) + 1, 0);
  mbGetHtmlForSelector(webView->view, selector, buf.data(), len + 1);
  webView->selector_html_cache.assign(buf.data());
  return webView->selector_html_cache.c_str();
}

const utf8* wkeGetAllTextForSelector(wkeWebView webView, const char* selector) {
  // innerText of EVERY match as a JSON array string ("[]" for none, "" for an
  // invalid selector). Owned by the view until the next call. (Port extension.)
  if (!webView || !webView->view || !selector) {
    if (webView)
      webView->selector_alltext_cache.clear();
    return webView ? webView->selector_alltext_cache.c_str() : "";
  }
  const int len = mbGetAllTextForSelector(webView->view, selector, nullptr, 0);
  if (len <= 0) {  // -1 invalid selector
    webView->selector_alltext_cache.clear();
    return webView->selector_alltext_cache.c_str();
  }
  std::vector<char> buf(static_cast<size_t>(len) + 1, 0);
  mbGetAllTextForSelector(webView->view, selector, buf.data(), len + 1);
  webView->selector_alltext_cache.assign(buf.data());
  return webView->selector_alltext_cache.c_str();
}

const utf8* wkeGetAllValueForSelector(wkeWebView webView, const char* selector) {
  // Live .value of EVERY match as a JSON array string ("[]" none, "" invalid).
  // Owned by the view until the next call. (Port extension.)
  if (!webView || !webView->view || !selector) {
    if (webView)
      webView->selector_allvalue_cache.clear();
    return webView ? webView->selector_allvalue_cache.c_str() : "";
  }
  const int len = mbGetAllValueForSelector(webView->view, selector, nullptr, 0);
  if (len <= 0) {  // -1 invalid selector
    webView->selector_allvalue_cache.clear();
    return webView->selector_allvalue_cache.c_str();
  }
  std::vector<char> buf(static_cast<size_t>(len) + 1, 0);
  mbGetAllValueForSelector(webView->view, selector, buf.data(), len + 1);
  webView->selector_allvalue_cache.assign(buf.data());
  return webView->selector_allvalue_cache.c_str();
}

const utf8* wkeGetAllAttributeForSelector(wkeWebView webView,
                                          const char* selector,
                                          const char* attr) {
  // getAttribute(attr) of EVERY match as a JSON array string (absent -> null;
  // "[]" for none, "" for an invalid selector). Owned by the view. (Port ext.)
  if (!webView || !webView->view || !selector || !attr) {
    if (webView)
      webView->selector_allattr_cache.clear();
    return webView ? webView->selector_allattr_cache.c_str() : "";
  }
  const int len =
      mbGetAllAttributeForSelector(webView->view, selector, attr, nullptr, 0);
  if (len <= 0) {  // -1 invalid selector
    webView->selector_allattr_cache.clear();
    return webView->selector_allattr_cache.c_str();
  }
  std::vector<char> buf(static_cast<size_t>(len) + 1, 0);
  mbGetAllAttributeForSelector(webView->view, selector, attr, buf.data(), len + 1);
  webView->selector_allattr_cache.assign(buf.data());
  return webView->selector_allattr_cache.c_str();
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

bool wkeSetAttribute(wkeWebView webView, const char* selector, const char* attr,
                     const char* value) {
  // setAttribute on the first match; true if an element matched. (Port ext.)
  return webView && webView->view && selector && attr &&
         mbSetAttribute(webView->view, selector, attr, value) != 0;
}

const utf8* wkeGetValueForSelector(wkeWebView webView, const char* selector) {
  // Live .value of the FIRST element matching `selector` ("" if no match or the
  // element has no value property — same contract as wkeGetAttribute). Owned by
  // the view, valid until the next wkeGetValueForSelector on it.
  if (!webView || !webView->view || !selector) {
    if (webView)
      webView->selector_value_cache.clear();
    return webView ? webView->selector_value_cache.c_str() : "";
  }
  const int len = mbGetValueForSelector(webView->view, selector, nullptr, 0);
  if (len <= 0) {  // -1 no match / no value property, or 0 empty value
    webView->selector_value_cache.clear();
    return webView->selector_value_cache.c_str();
  }
  std::vector<char> buf(static_cast<size_t>(len) + 1, 0);
  mbGetValueForSelector(webView->view, selector, buf.data(), len + 1);
  webView->selector_value_cache.assign(buf.data());
  return webView->selector_value_cache.c_str();
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
// Each selector action runs page JS (event handlers), so after acting it drains
// console + window.mbBridge output via DrainPageEvents — so a page->host message
// sent from, e.g., an onclick handler reaches the host immediately.
bool wkeClickSelector(wkeWebView webView, const char* selector) {
  // Click the first element matching `selector` (resolves its box and dispatches
  // a real click). False if nothing matches or it has no box. (Port extension.)
  if (!webView || !webView->view || !selector)
    return false;
  const bool ok = mbClickSelector(webView->view, selector) != 0;
  DrainPageEvents(webView);
  return ok;
}

bool wkeDispatchEvent(wkeWebView webView, const char* selector, const char* type) {
  // Dispatch a synthetic DOM event of `type` on the first match; true if matched.
  // (Port extension.)
  if (!webView || !webView->view || !selector || !type)
    return false;
  const bool ok = mbDispatchEvent(webView->view, selector, type) != 0;
  DrainPageEvents(webView);
  return ok;
}

bool wkeDragSelector(wkeWebView webView, const char* fromSelector,
                     const char* toSelector) {
  // Mouse-drag `from` -> `to` by selector centers; true if both matched. (Port
  // extension — Puppeteer dragAndDrop; mouse-based, not HTML5 native DnD.)
  if (!webView || !webView->view || !fromSelector || !toSelector)
    return false;
  const bool ok = mbDragSelector(webView->view, fromSelector, toSelector) != 0;
  DrainPageEvents(webView);
  return ok;
}

bool wkeFillSelector(wkeWebView webView, const char* selector,
                     const utf8* text) {
  // Set the value of the first matching input/textarea and fire input+change
  // (so frameworks like React observe it). False if nothing matches. (Port ext.)
  if (!webView || !webView->view || !selector || !text)
    return false;
  const bool ok = mbFillSelector(webView->view, selector, text) != 0;
  DrainPageEvents(webView);
  return ok;
}

bool wkeSelectOption(wkeWebView webView, const char* selector,
                     const utf8* value) {
  // Choose a <select> option whose value (or visible text) equals `value`,
  // firing input+change. False if no <select> or no matching option. (Port ext.)
  if (!webView || !webView->view || !selector || !value)
    return false;
  const bool ok = mbSelectOption(webView->view, selector, value) != 0;
  DrainPageEvents(webView);
  return ok;
}

bool wkeScrollIntoView(wkeWebView webView, const char* selector) {
  // Scroll the first matching element into the viewport (to trigger lazy
  // loading or frame it before a screenshot). False if nothing matches. The
  // click/fill selector ops already do this internally. (Port extension.)
  if (!webView || !webView->view || !selector)
    return false;
  const bool ok = mbScrollIntoView(webView->view, selector) != 0;
  DrainPageEvents(webView);
  return ok;
}

// Additional pointer/focus actions on the first selector match (each false if
// nothing matches). Hover fires mouseover/enter; double/right-click fire dblclick
// /contextmenu; focus/blur move the active element. (Port extensions.)
bool wkeHoverSelector(wkeWebView webView, const char* selector) {
  if (!webView || !webView->view || !selector)
    return false;
  const bool ok = mbHoverSelector(webView->view, selector) != 0;
  DrainPageEvents(webView);
  return ok;
}
bool wkeDoubleClickSelector(wkeWebView webView, const char* selector) {
  if (!webView || !webView->view || !selector)
    return false;
  const bool ok = mbDoubleClickSelector(webView->view, selector) != 0;
  DrainPageEvents(webView);
  return ok;
}
bool wkeRightClickSelector(wkeWebView webView, const char* selector) {
  if (!webView || !webView->view || !selector)
    return false;
  const bool ok = mbRightClickSelector(webView->view, selector) != 0;
  DrainPageEvents(webView);
  return ok;
}
bool wkeFocusSelector(wkeWebView webView, const char* selector) {
  if (!webView || !webView->view || !selector)
    return false;
  const bool ok = mbFocusSelector(webView->view, selector) != 0;
  DrainPageEvents(webView);
  return ok;
}
bool wkeBlurSelector(wkeWebView webView, const char* selector) {
  if (!webView || !webView->view || !selector)
    return false;
  const bool ok = mbBlurSelector(webView->view, selector) != 0;
  DrainPageEvents(webView);
  return ok;
}

// --- Waits (pump the loop until a condition or timeout) ------------------------
bool wkeWaitForSelector(wkeWebView webView, const char* selector,
                        int timeoutMs) {
  // Pump until the first element matching `selector` exists, or timeoutMs
  // elapses. True if it appeared (Puppeteer-style waitForSelector). (Port ext.)
  return webView && webView->view && selector &&
         mbWaitForSelector(webView->view, selector, timeoutMs) != 0;
}

bool wkeWaitForVisibleSelector(wkeWebView webView, const char* selector,
                               int timeoutMs) {
  // Pump until the first match is actually visible (not just present), or
  // timeoutMs elapses. True once it's shown. (Port ext.)
  return webView && webView->view && selector &&
         mbWaitForVisibleSelector(webView->view, selector, timeoutMs) != 0;
}

bool wkeWaitForSelectorHidden(wkeWebView webView, const char* selector,
                              int timeoutMs) {
  // Pump until the first match is gone or hidden, or timeoutMs elapses. True
  // once it's no longer visible (the spinner-gone signal). (Port ext.)
  return webView && webView->view && selector &&
         mbWaitForSelectorHidden(webView->view, selector, timeoutMs) != 0;
}

bool wkeWaitForNetworkIdle(wkeWebView webView, int idleMs, int timeoutMs) {
  // Pump until no new request for idleMs (networkidle), or timeoutMs. True once
  // quiet. Clear the log before navigating to scope it. (Port ext.)
  return webView && webView->view &&
         mbWaitForNetworkIdle(webView->view, idleMs, timeoutMs) != 0;
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
  if (len <= 0) {  // empty/failed: guard against assign(nullptr) from a 0-size buf
    webView->source_cache.clear();
    return webView->source_cache.c_str();
  }
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
  // Size-first (mbGetCookies is a non-destructive read): a fixed 16 KiB buffer
  // silently truncated a large jar. Query the length, then fit the buffer — the
  // same pattern the sibling wkeGetCookieValue / wkeGetAllCookie already use.
  const int len = mbGetCookies(webView->view, url, nullptr, 0);
  std::vector<char> buf(static_cast<size_t>(len > 0 ? len : 0) + 1, 0);
  mbGetCookies(webView->view, url, buf.data(), static_cast<int>(buf.size()));
  webView->cookie_cache.assign(buf.data());
  return webView->cookie_cache.c_str();
}

const utf8* wkeGetCookieValue(wkeWebView webView, const utf8* name) {
  // The value of a single cookie `name` for the current document ("" if absent) —
  // e.g. read the session/auth cookie. Owned by the view. (Port extension.)
  if (!webView || !webView->view || !name) {
    if (webView)
      webView->cookie_cache.clear();
    return webView ? webView->cookie_cache.c_str() : "";
  }
  char url[4096] = {0};
  mbGetURL(webView->view, url, sizeof(url));
  const int len = mbGetCookie(webView->view, url, name, nullptr, 0);
  if (len < 0) {  // not set
    webView->cookie_cache.clear();
    return webView->cookie_cache.c_str();
  }
  std::vector<char> buf(static_cast<size_t>(len) + 1, 0);
  mbGetCookie(webView->view, url, name, buf.data(), len + 1);
  webView->cookie_cache.assign(buf.data(), len);
  return webView->cookie_cache.c_str();
}

const utf8* wkeGetAllCookie(wkeWebView webView) {
  // The WHOLE jar (every host, session + persistent) as a Netscape cookie file,
  // in memory — for full session export. Owned by the view, valid until the next
  // wkeGetAllCookie on it. (Port extension; size-first via mbGetAllCookies.)
  if (!webView || !webView->view)
    return "";
  const int len = mbGetAllCookies(webView->view, nullptr, 0);  // size first
  if (len <= 0) {
    webView->all_cookie_cache.clear();
    return webView->all_cookie_cache.c_str();
  }
  std::vector<char> buf(static_cast<size_t>(len) + 1, 0);
  mbGetAllCookies(webView->view, buf.data(), len + 1);
  webView->all_cookie_cache.assign(buf.data());
  return webView->all_cookie_cache.c_str();
}

const utf8* wkeGetLocalStorage(wkeWebView webView, const utf8* key) {
  // localStorage.getItem(key) for the document's origin ("" if absent or storage
  // is unavailable). Owned by the view until the next call. (Port extension.)
  if (!webView || !webView->view || !key) {
    if (webView)
      webView->local_storage_cache.clear();
    return webView ? webView->local_storage_cache.c_str() : "";
  }
  const int len = mbGetLocalStorage(webView->view, key, nullptr, 0);
  if (len < 0) {  // -1 absent / unavailable
    webView->local_storage_cache.clear();
    return webView->local_storage_cache.c_str();
  }
  std::vector<char> buf(static_cast<size_t>(len) + 1, 0);
  mbGetLocalStorage(webView->view, key, buf.data(), len + 1);
  webView->local_storage_cache.assign(buf.data(), len);
  return webView->local_storage_cache.c_str();
}

bool wkeSetLocalStorage(wkeWebView webView, const utf8* key, const utf8* value) {
  // localStorage.setItem(key, value) for the document's origin; true on success.
  return webView && webView->view && key &&
         mbSetLocalStorage(webView->view, key, value) != 0;
}

const utf8* wkeGetSessionStorage(wkeWebView webView, const utf8* key) {
  // sessionStorage.getItem(key) ("" if absent / unavailable). Owned by the view
  // until the next call. (Port extension.)
  if (!webView || !webView->view || !key) {
    if (webView)
      webView->session_storage_cache.clear();
    return webView ? webView->session_storage_cache.c_str() : "";
  }
  const int len = mbGetSessionStorage(webView->view, key, nullptr, 0);
  if (len < 0) {
    webView->session_storage_cache.clear();
    return webView->session_storage_cache.c_str();
  }
  std::vector<char> buf(static_cast<size_t>(len) + 1, 0);
  mbGetSessionStorage(webView->view, key, buf.data(), len + 1);
  webView->session_storage_cache.assign(buf.data(), len);
  return webView->session_storage_cache.c_str();
}

bool wkeSetSessionStorage(wkeWebView webView, const utf8* key, const utf8* value) {
  // sessionStorage.setItem(key, value); true on success. (Port extension.)
  return webView && webView->view && key &&
         mbSetSessionStorage(webView->view, key, value) != 0;
}

void wkeClearStorage(wkeWebView webView) {
  // Empty localStorage + sessionStorage for the origin (reset/logout). (Port ext.)
  if (webView && webView->view)
    mbClearStorage(webView->view);
}

const utf8* wkeGetRequestLog(wkeWebView webView) {
  // Process-wide subresource request log (newline-separated). Owned by the view
  // until the next call. (Port extension; the log itself is process-wide.)
  if (!webView)
    return "";
  const int len = mbGetRequestLog(nullptr, 0);  // size first
  if (len <= 0) {
    webView->request_log_cache.clear();
    return webView->request_log_cache.c_str();
  }
  std::vector<char> buf(static_cast<size_t>(len) + 1, 0);
  mbGetRequestLog(buf.data(), len + 1);
  webView->request_log_cache.assign(buf.data());
  return webView->request_log_cache.c_str();
}

void wkeClearRequestLog() {
  mbClearRequestLog();  // process-wide
}

void wkeBlockUrl(const utf8* substring) {
  if (substring)
    mbBlockUrl(substring);  // process-wide
}

void wkeClearUrlBlocks() {
  mbClearUrlBlocks();  // process-wide
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
    case wkeCookieCommandClearSessionCookies: {
      wkeWebView v = AnyLiveView();  // any live view drives the shared jar
      if (v && v->view)
        mbClearCookies(v->view);
      return;
    }
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
      // Real button press: a DOWN -> MOUSEMOVE(s) -> UP sequence performs a drag
      // (the moves carry the held button); a DOWN+UP at one point is a click.
      mbSendMouseDown(webView->view, x, y);
      return true;
    case WKE_MSG_LBUTTONUP:
      mbSendMouseUp(webView->view, x, y);
      mbWait(webView->view, 20);  // let the click/drag handlers + layout settle
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

bool wkeFireTouchTap(wkeWebView webView, int x, int y) {
  // Single-finger touch tap at (x,y) — fires touch-only handlers. (Port ext.)
  if (!webView || !webView->view)
    return false;
  mbSendTouchTap(webView->view, x, y);
  mbWait(webView->view, 20);  // let the tap's handlers/layout settle
  return true;
}

bool wkeFireTouchSwipe(wkeWebView webView, int x1, int y1, int x2, int y2) {
  // One-finger swipe (x1,y1)->(x2,y2) — touch scroll/swipe gestures. (Port ext.)
  if (!webView || !webView->view)
    return false;
  mbSendTouchSwipe(webView->view, x1, y1, x2, y2);
  mbWait(webView->view, 20);
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

bool wkeFireKeyUpEvent(wkeWebView webView, unsigned int virtualKeyCode,
                       unsigned int /*flags*/, bool /*systemKey*/) {
  if (!webView || !webView->view)
    return false;
  // Dispatch a real key RELEASE so page `keyup` handlers fire (key-release
  // detection, games, shortcut bookkeeping) — pre-fix this was a no-op.
  mbSendKeyUp(webView->view, static_cast<int>(virtualKeyCode));
  mbWait(webView->view, 20);  // let keyup handlers run before the next read
  return true;
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
  // HiDPI: the viewport is LOGICAL (webView->width CSS px) but the engine rasters at
  // device_scale, so the pixel buffer is width*scale x height*scale (retina-crisp).
  // A host on a 2x display sizes its buffer that way; pitch defaults to the physical row.
  const int pw = static_cast<int>(webView->width * webView->device_scale);
  const int ph = static_cast<int>(webView->height * webView->device_scale);
  const int stride = pitch > 0 ? pitch : pw * 4;
  // INTERACTIVE blit (a windowed host repaints continuously): use the FAST repaint,
  // not mbPaintToBitmap's one-shot screenshot settle — the latter re-drains the whole
  // task queue every call and makes live pages (YouTube) crawl. For a one-shot capture
  // use wkeSavePng (which keeps the settle).
  mbRepaintToBitmap(webView->view, bits, pw, ph, stride);
}

bool wkePaintRect(wkeWebView webView, void* bits, int x, int y, int w, int h,
                  int pitch) {
  // Composite the logical rect (x,y,w,h) into a caller BGRA8888 buffer (w x h px,
  // `pitch` bytes/row, default w*4) — a partial/dirty-rect capture to memory
  // without encoding. (Port extension; pairs with wkeGetElementRect.)
  if (!webView || !webView->view || !bits || w <= 0 || h <= 0)
    return false;
  const int stride = pitch > 0 ? pitch : w * 4;
  return mbPaintRectToBitmap(webView->view, bits, x, y, w, h, stride) != 0;
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
  // The view whose window.__mbslots holds this handle's live value (null for a
  // primitive with no slot). Slot reads/eviction must target THIS view, not the
  // caller's es-derived one, so a handle works regardless of which es is passed.
  wkeWebView owner = nullptr;
};
std::map<jsValue, JsRecord>& JsRegistry() {
  static auto* r = new std::map<jsValue, JsRecord>();
  return *r;
}
// thread_local so the returned const utf8* isn't clobbered by a concurrent call on
// another thread (matches jsGetKeys's per-thread holder; the contract is "valid until
// the next call on THIS thread").
std::string& JsTempBuf() {
  thread_local std::string* s = new std::string();  // leaked: no exit-time destructor
  return *s;
}
std::string& JsStringBuf() {  // separate temp for jsToString (JSON view)
  thread_local std::string* s = new std::string();  // leaked: no exit-time destructor
  return *s;
}
// Start above wke's small reserved constants (jsUndefined/jsNull/jsTrue/jsFalse).
jsValue g_next_js_value = 0x10000;

const JsRecord* JsLookup(jsValue v) {
  auto& reg = JsRegistry();
  auto it = reg.find(v);
  return it == reg.end() ? nullptr : &it->second;
}

// The view that owns handle `v`'s window.__mbslots slot, if it's still live;
// otherwise `fallback` (the caller's es-derived view). Guarding on liveness keeps
// a stale handle whose owner was destroyed from dereferencing a freed view.
wkeWebView OwnerView(jsValue v, wkeWebView fallback) {
  const JsRecord* r = JsLookup(v);
  if (r && r->owner) {
    auto& live = LiveViews();
    if (std::find(live.begin(), live.end(), r->owner) != live.end())
      return r->owner;
  }
  return fallback;
}

// A JS expression that reproduces jsValue `v` (for embedding it in a call). For a
// slot-backed value it's the slot ref; for a primitive constructor it's the
// stored literal; unknown handles read as undefined.
std::string LiteralOf(jsValue v) {
  const JsRecord* r = JsLookup(v);
  return (r && !r->literal.empty()) ? r->literal : std::string("undefined");
}

// Bound the jsValue registry WITHOUT invalidating recent handles. When it grows
// past a soft cap, drop only the OLDEST records (lowest ids — created long ago, so
// most likely already consumed by the caller) and keep the most recent. The old
// code did reg.clear() at 4096, which dangled EVERY outstanding jsValue at once —
// turning held handles (including a native callback's own jsArgs) silently into
// undefined the moment a loop crossed the cap. Slot-backed evictees also get their
// window.__mbslots entry deleted so the page heap stays bounded between navigations.
constexpr size_t kJsRegistrySoftCap = 16384;
constexpr size_t kJsRegistryRetain = 8192;
void EvictOldJsRecords() {
  auto& reg = JsRegistry();
  if (reg.size() < kJsRegistrySoftCap)
    return;
  const size_t to_drop = reg.size() - kJsRegistryRetain;
  // Prune each evicted slot-backed value from its OWNING view's page heap (not a
  // broadcast to every view, which would delete unrelated slots that happen to share
  // an id in another view).
  std::map<wkeWebView, std::string> del_by_view;
  size_t dropped = 0;
  for (auto it = reg.begin(); dropped < to_drop && it != reg.end(); ++dropped) {
    if (it->second.owner &&
        it->second.literal.rfind("window.__mbslots[", 0) == 0)
      del_by_view[it->second.owner] +=
          "delete window.__mbslots[" + std::to_string(it->first) + "];";
    it = reg.erase(it);
  }
  char tmp[8] = {0};
  auto& live = LiveViews();
  for (auto& kv : del_by_view) {
    wkeWebView lv = kv.first;
    if (lv && lv->view && !kv.second.empty() &&
        std::find(live.begin(), live.end(), lv) != live.end())
      mbEvalJS(lv->view, kv.second.c_str(), tmp, sizeof(tmp));
  }
}

// Register a primitive jsValue (jsInt/jsString/...) with no eval — value, type,
// and the JS literal that reproduces it (so it can be inlined into a jsCall).
jsValue MakeLiteral(const std::string& value, const std::string& type,
                    const std::string& literal) {
  auto& reg = JsRegistry();
  EvictOldJsRecords();
  const jsValue h = g_next_js_value++;
  reg[h] = JsRecord{value, type, literal, /*owner=*/nullptr};  // primitive: no slot
  return h;
}

// Quote `s` as a JS string literal (for embedding a property name / value safely). All
// control chars are escaped — a raw control char in a string literal is a SyntaxError, so
// the old \n/\r-only escaping could produce an unparseable literal. (An embedded NUL still
// truncates: the input is a NUL-terminated C string, so the bytes after it are unreachable.)
std::string JsStringLiteral(const char* s) {
  std::string out = "\"";
  for (const char* p = s; p && *p; ++p) {
    unsigned char c = static_cast<unsigned char>(*p);
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += "\"";
  return out;
}

// Eval `script` and record its coerced value + type under a fresh handle, ALSO
// parking the live result in window.__mbslots[handle] so a later eval can index
// it (backs jsGetAt). The slot store is done IN JS (a wrapper assignment) — never
// from C++, which is what crashed a prior attempt.
//
// The wrapper is an IIFE that CATCHES a runtime exception and returns undefined: so a
// statement (which can't be the assignment's RHS) is a SyntaxError of the whole wrapper
// -> nothing runs -> empty type -> plain-eval fallback; but a side-effecting EXPRESSION
// that throws runs ONCE inside the try and returns a defined (undefined-typed) value ->
// NO fallback. The earlier `slot=(script)` form couldn't tell a throw from a parse error
// (both gave empty type), so it re-ran a throwing expression's side effects a second time.
jsValue StoreEval(wkeWebView wv, const std::string& script) {
  auto& reg = JsRegistry();
  EvictOldJsRecords();  // bound the registry; evicts only OLDEST handles + their slots
  const jsValue handle = g_next_js_value++;  // handle id == its __mbslots slot
  const std::string slot =
      "window.__mbslots[" + std::to_string(handle) + "]";
  const std::string wrapped =
      "window.__mbslots=window.__mbslots||{};(function(){try{return(" + slot +
      "=(" + script + "));}catch(e){" + slot + "=undefined;return undefined;}})()";
  std::vector<char> value(1 << 16, 0);  // 64 KiB result cap
  char type[16] = {0};
  int n = mbEvalJSEx(wv->view, wrapped.c_str(), value.data(),
                     static_cast<int>(value.size()), type, sizeof(type));
  bool slotted = true;
  if (type[0] == '\0') {  // wrapper didn't parse (a statement) -> plain eval
    slotted = false;
    std::fill(value.begin(), value.end(), 0);
    n = mbEvalJSEx(wv->view, script.c_str(), value.data(),
                   static_cast<int>(value.size()), type, sizeof(type));
  }
  // A slotted result is navigable/callable via its slot ref; a fallback isn't.
  std::string literal =
      slotted ? ("window.__mbslots[" + std::to_string(handle) + "]")
              : std::string();
  // Use the returned length so an embedded NUL in a JS string result isn't truncated
  // (clamped to the buffer: the return reports the full length, which may exceed cap).
  const size_t vlen =
      n < 0 ? 0 : std::min(static_cast<size_t>(n), value.size() - 1);
  reg[handle] = JsRecord{std::string(value.data(), vlen), std::string(type),
                         std::move(literal), /*owner=*/wv};
  return handle;
}

// Frame handles are opaque pointers carrying a frame index: main frame == 1
// (mb index -1), child i == i+2 (mb index i). 0/null is invalid. This keeps a
// stable, comparable handle without tracking blink frame objects across loads.
int FrameIndexOf(void* h) {
  intptr_t v = reinterpret_cast<intptr_t>(h);
  if (v == 1)
    return -1;  // main frame
  if (v >= 2)
    return static_cast<int>(v - 2);  // child index
  return -2;                         // invalid
}
void* FrameHandleOf(int frame_index) {  // frame_index -1 => main
  return reinterpret_cast<void*>(
      static_cast<intptr_t>(frame_index < 0 ? 1 : frame_index + 2));
}

// Eval `script` in the frame_index-th frame (-1 = main) and register a string-
// typed jsValue for its result. Unlike StoreEval there is no __mbslots slot — a
// frame's result isn't navigable/callable from the main world — so the handle
// carries only the coerced string value (read via jsToString/jsToInt/...).
jsValue StoreEvalInFrame(wkeWebView wv, int frame_index,
                         const std::string& script) {
  auto& reg = JsRegistry();
  EvictOldJsRecords();  // bound the registry; evicts only OLDEST handles
  const jsValue handle = g_next_js_value++;
  std::vector<char> value(1 << 16, 0);  // 64 KiB result cap
  const int n = mbEvalJSInFrame(wv->view, frame_index, script.c_str(),
                                value.data(), static_cast<int>(value.size()));
  // Use the returned length so an embedded NUL isn't truncated (clamped to the buffer).
  const size_t vlen =
      n < 0 ? 0 : std::min(static_cast<size_t>(n), value.size() - 1);
  reg[handle] = JsRecord{std::string(value.data(), vlen), "string", std::string(),
                         /*owner=*/wv};
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

jsValue jsEval(jsExecState es, const utf8* str) {
  // Evaluate `str` in the exec state and return its result as a jsValue — the
  // es-based sibling of wkeRunJS (es is the webView token from wkeGlobalExec).
  return wkeRunJS(reinterpret_cast<wkeWebView>(es), str);
}

const utf8* wkeRunJsInIsolatedWorld(wkeWebView webView, const utf8* script) {
  // Run `script` in a dedicated ISOLATED world: its own JS globals (separate
  // from the page and from wkeRunJS's main world) but the SAME DOM — the
  // content-script model, for injecting automation that the page can't see or
  // collide with. Returns the result coerced to a string (owned by the view,
  // valid until the next call on it). (Port extension — wraps mbEvalJSIsolated.)
  if (!webView || !webView->view || !script) {
    if (webView)
      webView->isolated_cache.clear();
    return webView ? webView->isolated_cache.c_str() : "";
  }
  const int len = mbEvalJSIsolated(webView->view, script, nullptr, 0);  // size
  if (len <= 0) {
    webView->isolated_cache.clear();
    return webView->isolated_cache.c_str();
  }
  std::vector<char> buf(static_cast<size_t>(len) + 1, 0);
  mbEvalJSIsolated(webView->view, script, buf.data(), len + 1);
  webView->isolated_cache.assign(buf.data());
  return webView->isolated_cache.c_str();
}

jsExecState wkeGlobalExec(wkeWebView webView) {
  // No real exec state is needed — the jsValue handle carries the result. Return
  // a non-null token so callers' null checks pass.
  return reinterpret_cast<jsExecState>(webView);
}

// --- Sub-frame scripting (wkeRunJsByFrame) -------------------------------------
wkeWebFrameHandle wkeWebFrameGetMainFrame(wkeWebView webView) {
  if (!webView || !webView->view)
    return nullptr;
  return FrameHandleOf(-1);
}

int wkeWebFrameGetSubFrameCount(wkeWebView webView) {
  if (!webView || !webView->view)
    return 0;
  return mbGetFrameCount(webView->view);
}

wkeWebFrameHandle wkeWebFrameGetSubFrame(wkeWebView webView, int index) {
  if (!webView || !webView->view || index < 0 ||
      index >= mbGetFrameCount(webView->view))
    return nullptr;
  return FrameHandleOf(index);
}

bool wkeIsMainFrame(wkeWebView webView, wkeWebFrameHandle frameId) {
  return webView && webView->view && FrameIndexOf(frameId) == -1;
}

jsValue wkeRunJsByFrame(wkeWebView webView, wkeWebFrameHandle frameId,
                        const utf8* script, bool isInClosure) {
  if (!webView || !webView->view || !script)
    return 0;
  const int idx = FrameIndexOf(frameId);
  if (idx == -2)
    return 0;  // invalid handle
  // isInClosure: run the script as a function body (its `return` is the result),
  // mirroring upstream; otherwise eval it as a bare expression.
  const std::string src =
      isInClosure ? ("(function(){" + std::string(script) + "})()")
                  : std::string(script);
  const jsValue handle = StoreEvalInFrame(webView, idx, src);
  DrainConsoleToCallback(webView);  // deliver console output the script logged
  DrainBridgeToCallback(webView);   // deliver any window.mbBridge calls it made
  return handle;
}

// --- Native function binding (wkeJsBindFunction): JS -> C synchronously --------
// Built on the host's mbJsBindFunction (string args -> string return). Each wke
// binding wraps the classic jsValue-based callback: the host shim parks the
// string args as jsValues (read via jsArg/jsArgCount), calls the wke function,
// and stringifies its returned jsValue back for JS. (Return is coerced to a JS
// string — this port's mb primitive is string-valued.)
namespace {
struct WkeBinding {
  wkeJsNativeFunction fn = nullptr;
  void* param = nullptr;
  wkeWebView wv = nullptr;
  std::string ret;  // backs the const char* handed back to the host/v8
};
std::vector<std::unique_ptr<WkeBinding>>& WkeBindings() {
  static auto* v = new std::vector<std::unique_ptr<WkeBinding>>();
  return *v;
}
// Drop a destroyed view's native bindings (its installed v8 functions die with
// the view's context, so nothing references these any more). Prevents an
// unbounded leak across view create/destroy churn. (Declared near the top.)
void ForgetBindingsForView(wkeWebView wv) {
  auto& b = WkeBindings();
  b.erase(std::remove_if(b.begin(), b.end(),
                         [wv](const std::unique_ptr<WkeBinding>& x) {
                           return x->wv == wv;
                         }),
          b.end());
}
std::vector<jsValue>& CurrentArgs() {  // args of the in-flight bound call
  static auto* v = new std::vector<jsValue>();
  return *v;
}

const char* WkeNativeShim(void* userdata, int argc, const char** argv,
                          const int* argtypes, int* out_type) {
  auto* b = static_cast<WkeBinding*>(userdata);
  if (!b || !b->fn)
    return nullptr;
  std::vector<jsValue> saved = CurrentArgs();  // re-entrancy safe
  CurrentArgs().clear();
  for (int i = 0; i < argc; ++i) {
    const char* a = argv[i] ? argv[i] : "";
    const int t = argtypes ? argtypes[i] : 0;
    // Preserve each arg's JS type so jsTypeOf/jsIs* on jsArg are accurate.
    const char* ty = "string";
    std::string lit = JsStringLiteral(a);
    switch (t) {
      case 1: ty = "number"; lit = a; break;
      case 2: ty = "boolean"; lit = a; break;
      case 3: ty = "null"; lit = "null"; break;
      case 4: ty = "undefined"; lit = "undefined"; break;
      case 5: ty = "object"; lit = "undefined"; break;
      case 6: ty = "array"; lit = "undefined"; break;
      case 7: ty = "function"; lit = "undefined"; break;
      default: break;  // string
    }
    CurrentArgs().push_back(MakeLiteral(a, ty, lit));
  }
  const jsValue r = b->fn(reinterpret_cast<jsExecState>(b->wv), b->param);
  CurrentArgs() = saved;
  const JsRecord* rec = JsLookup(r);
  b->ret = rec ? rec->value : std::string();
  // Preserve the returned jsValue's JS type (number/boolean/null/undefined) so
  // JS receives a real value, not always a string.
  if (out_type && rec) {
    if (rec->type == "number")
      *out_type = 1;
    else if (rec->type == "boolean")
      *out_type = 2;
    else if (rec->type == "null")
      *out_type = 3;
    else if (rec->type == "undefined")
      *out_type = 4;
  }
  return b->ret.c_str();
}
}  // namespace

int jsArgCount(jsExecState /*es*/) {
  return static_cast<int>(CurrentArgs().size());
}

jsValue jsArg(jsExecState /*es*/, int idx) {
  const std::vector<jsValue>& a = CurrentArgs();
  if (idx < 0 || idx >= static_cast<int>(a.size()))
    return MakeLiteral("undefined", "undefined", "undefined");
  return a[idx];
}

void wkeJsBindFunction(wkeWebView webView, const char* name,
                       wkeJsNativeFunction fn, void* param) {
  // Bind `fn` as window[name](...): JS calls it synchronously, reading its args
  // via jsArg/jsArgCount and returning a jsValue (delivered to JS as a string).
  if (!webView || !webView->view || !name || !fn)
    return;
  auto b = std::make_unique<WkeBinding>();
  b->fn = fn;
  b->param = param;
  b->wv = webView;
  WkeBinding* raw = b.get();
  WkeBindings().push_back(std::move(b));
  mbJsBindFunction(webView->view, name, &WkeNativeShim, raw);
}

namespace {
// Parse the leading number from `s` locale-independently. std::from_chars always uses
// the C locale ('.'-decimal), so "1.5" parses as 1.5 even under a comma-decimal
// LC_NUMERIC where strtod/atof would stop at "1". Handles exponential notation
// ("1e+21") and (case-insensitively) inf/nan; 0.0 if nothing parses (matching strtod).
double ParseCNumber(const std::string& s) {
  const char* begin = s.c_str();
  const char* end = begin + s.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(*begin)))
    ++begin;
  double d = 0.0;
  const auto res = std::from_chars(begin, end, d);
  return res.ec == std::errc() ? d : 0.0;
}
// Numeric value of a jsValue, keyed off its JS TYPE. ParseCNumber (not atoi/atof) parses
// the number forms V8 stringifies, including exponential notation ("1e+21") — atoi("1e+21")
// returned 1. boolean -> 1/0; null/undefined/object -> 0/NaN.
double JsToNumber(const JsRecord* r) {
  if (!r)
    return 0.0;
  if (r->type == "boolean")
    return r->value == "true" ? 1.0 : 0.0;
  if (r->type == "null")
    return 0.0;
  if (r->type == "number" || r->type == "string")
    return ParseCNumber(r->value);
  return std::nan("");  // undefined / object / array / function
}
}  // namespace

int jsToInt(jsExecState /*es*/, jsValue v) {
  const double d = JsToNumber(JsLookup(v));
  if (std::isnan(d))
    return 0;
  if (d >= static_cast<double>(INT_MAX))
    return INT_MAX;
  if (d <= static_cast<double>(INT_MIN))
    return INT_MIN;
  return static_cast<int>(d);
}

double jsToDouble(jsExecState /*es*/, jsValue v) {
  return JsToNumber(JsLookup(v));
}

float jsToFloat(jsExecState /*es*/, jsValue v) {
  return static_cast<float>(JsToNumber(JsLookup(v)));
}

bool jsToBoolean(jsExecState /*es*/, jsValue v) {
  const JsRecord* r = JsLookup(v);
  if (!r)
    return false;
  // ECMAScript ToBoolean, keyed off the stored JS TYPE. Operating on the coerced
  // string (the old code) was wrong: null/undefined stringify to non-empty
  // "null"/"undefined" (truthy), NaN to "NaN" (truthy), while the strings "0" and
  // "false" are actually TRUTHY (any non-empty string is). Decide by type instead.
  const std::string& t = r->type;
  if (t == "null" || t == "undefined")
    return false;
  if (t == "boolean")
    return r->value == "true";
  if (t == "number") {
    const double d = ParseCNumber(r->value);  // C-locale parse ('.'-decimal)
    return d != 0.0 && !std::isnan(d);  // 0, -0, NaN are falsey
  }
  if (t == "string")
    return !r->value.empty();  // ANY non-empty string is truthy (incl. "0"/"false")
  return true;  // object / array / function are always truthy
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
  wkeWebView wv = OwnerView(v, reinterpret_cast<wkeWebView>(es));
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
  wkeWebView wv = OwnerView(object, reinterpret_cast<wkeWebView>(es));
  if (!wv || !wv->view)
    return 0;
  // Read window.__mbslots[object].length safely (0 if not present). Gate on
  // Array.isArray so a non-array (e.g. a function's .length arity, or a string's
  // length) reads 0 — classic wke's jsGetLength is array-oriented.
  const std::string script = "(function(){try{var o=window.__mbslots[" +
                             std::to_string(object) +
                             "];return Array.isArray(o)?o.length:0}catch(e){return 0}})()";
  char buf[32] = {0};
  mbEvalJS(wv->view, script.c_str(), buf, sizeof(buf));
  int len = 0;
  std::from_chars(buf, buf + std::strlen(buf), len);  // C-locale int parse
  return len;
}

jsValue jsGetAt(jsExecState es, jsValue object, int index) {
  wkeWebView wv = OwnerView(object, reinterpret_cast<wkeWebView>(es));
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
  wkeWebView wv = OwnerView(object, reinterpret_cast<wkeWebView>(es));
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

  wkeWebView wv = OwnerView(object, reinterpret_cast<wkeWebView>(es));
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
namespace {
// A double formatted as a JS numeric literal that round-trips. std::to_string is %f (6
// fractional digits), which mangled jsDouble(1e-7) -> "0.000000" and lost precision on the
// way into AND out of the page; %.17g round-trips every double. NaN/Infinity get their JS
// spellings (the %g forms "nan"/"inf" aren't valid JS literals).
std::string NumberToJs(double d) {
  if (std::isnan(d))
    return "NaN";
  if (std::isinf(d))
    return d < 0 ? "-Infinity" : "Infinity";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.17g", d);
  return std::string(buf);
}
}  // namespace

jsValue jsInt(int n) {
  return MakeLiteral(std::to_string(n), "number", std::to_string(n));
}
jsValue jsDouble(double d) {
  const std::string s = NumberToJs(d);
  return MakeLiteral(s, "number", s);
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
  wkeWebView wv = OwnerView(object, reinterpret_cast<wkeWebView>(es));
  if (!wv || !wv->view || !prop)
    return;
  EvalVoid(wv, "(function(){try{window.__mbslots[" + std::to_string(object) +
                   "][" + JsStringLiteral(prop) + "]=(" + LiteralOf(value) +
                   ")}catch(e){}})()");
}

void jsSetAt(jsExecState es, jsValue object, int index, jsValue value) {
  wkeWebView wv = OwnerView(object, reinterpret_cast<wkeWebView>(es));
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

// Process-wide forwarding state for the network-interception peers (the host hooks are
// process-wide). POD: no exit-time destructor.
namespace {
struct WkeNetReqHook {
  wkeWebView wv;
  wkeNetRequestCallback cb;
  void* param;
};
WkeNetReqHook g_wke_net_req{nullptr, nullptr, nullptr};
struct WkeNetRespHook {
  wkeWebView wv;
  wkeNetResponseCallback cb;
  void* param;
};
WkeNetRespHook g_wke_net_resp{nullptr, nullptr, nullptr};
// The miniblink49 wkeOnLoadUrlEnd hook: like wkeNetOnResponse but passes the response JOB
// handle (= the mbResponse*) so the callback can REWRITE the body via wkeNetSetData. Shares
// the single process-wide mb response callback with wkeNetOnResponse via one dispatcher.
struct WkeLoadEndHook {
  wkeWebView wv;
  wkeLoadUrlEndCallback cb;
  void* param;
};
WkeLoadEndHook g_wke_load_end{nullptr, nullptr, nullptr};

// The miniblink49 net JOB. miniblink49 uses one opaque `void* job` for both the request-begin
// (wkeOnLoadUrlBegin) and response-end (wkeOnLoadUrlEnd) hooks, and the same wkeNetSetData/
// SetMIMEType/SetHTTPHeaderField mutate it. We tag the job so those setters dispatch to the right
// side: a REQUEST job collects a mock (served WITHOUT a network fetch); a RESPONSE job wraps the
// live mbResponse* (rewrites the already-fetched body).
struct WkeNetJob {
  enum Kind { kRequest, kResponse } kind;
  // kResponse:
  mbResponse* resp = nullptr;
  // kRequest (mock the begin-callback fills via wkeNetSetData/wkeNetSetMIMEType):
  bool req_mocked = false;
  std::string req_data;
  std::string req_content_type;
};

struct WkeLoadBeginHook {
  wkeWebView wv;
  wkeLoadUrlBeginCallback cb;
  void* param;
};
WkeLoadBeginHook g_wke_load_begin{nullptr, nullptr, nullptr};

// One mb response callback fans out to both response hooks (read-only wkeNetOnResponse
// first, then the rewrite-capable wkeOnLoadUrlEnd), so the two coexist instead of the last
// registration clobbering the other.
void WkeDispatchResponse(mbResponse* r, void*) {
  int len = 0;
  const char* body = mbResponseBody(r, &len);
  const char* url = mbResponseURL(r);
  if (g_wke_net_resp.cb)
    g_wke_net_resp.cb(g_wke_net_resp.wv, g_wke_net_resp.param, url, body, len);
  if (g_wke_load_end.cb) {
    WkeNetJob job;
    job.kind = WkeNetJob::kResponse;
    job.resp = r;
    g_wke_load_end.cb(g_wke_load_end.wv, g_wke_load_end.param, url, &job,
                      const_cast<char*>(body), len);
  }
}

// Register/clear the shared mb response callback based on whether EITHER hook is active.
void WkeSyncResponseHook() {
  if (g_wke_net_resp.cb || g_wke_load_end.cb)
    mbSetResponseCallback(&WkeDispatchResponse, nullptr);
  else
    mbSetResponseCallback(nullptr, nullptr);
}

// Drop any process-wide net hooks owned by a view being destroyed, deregistering the
// matching host callback so a freed wkeWebView can never be invoked on a later request
// (the hooks are process-global; without this they keep a dangling handle -> UAF).
void ClearNetHooksForView(wkeWebView wv) {
  if (g_wke_net_req.wv == wv) {
    g_wke_net_req = {nullptr, nullptr, nullptr};
    mbSetRequestCallback(nullptr, nullptr);
  }
  if (g_wke_load_begin.wv == wv) {
    g_wke_load_begin = {nullptr, nullptr, nullptr};
    mbSetRequestMockCallback(nullptr, nullptr);
  }
  if (g_wke_net_resp.wv == wv)
    g_wke_net_resp = {nullptr, nullptr, nullptr};
  if (g_wke_load_end.wv == wv)
    g_wke_load_end = {nullptr, nullptr, nullptr};
  WkeSyncResponseHook();  // deregister the shared response cb if both sides now clear
}
}  // namespace

void wkeNetOnRequest(wkeWebView webView, wkeNetRequestCallback callback,
                     void* param) {
  g_wke_net_req = {webView, callback, param};
  if (callback) {
    mbSetRequestCallback(
        [](const char* url, void*) -> int {
          return (g_wke_net_req.cb &&
                  g_wke_net_req.cb(g_wke_net_req.wv, g_wke_net_req.param, url))
                     ? 1
                     : 0;
        },
        nullptr);
  } else {
    mbSetRequestCallback(nullptr, nullptr);
  }
}

void wkeNetOnResponse(wkeWebView webView, wkeNetResponseCallback callback,
                      void* param) {
  g_wke_net_resp = {webView, callback, param};
  WkeSyncResponseHook();
}

// miniblink49 parity: hook a response after its data arrives, with the JOB handle for
// rewriting it. The job is a kResponse WkeNetJob; pass it to wkeNetSetData to replace the body.
void wkeOnLoadUrlEnd(wkeWebView webView, wkeLoadUrlEndCallback callback,
                     void* param) {
  g_wke_load_end = {webView, callback, param};
  WkeSyncResponseHook();
}

// miniblink49 parity: fire a callback BEFORE each request, with a request JOB the callback can
// turn into a canned response via wkeNetSetData (served with NO network fetch — the classic
// offline-mock / API-substitution hook). If the callback doesn't set data, the request fetches
// normally. Backed by the host's per-URL request-mock callback. NULL clears it.
void wkeOnLoadUrlBegin(wkeWebView webView, wkeLoadUrlBeginCallback callback,
                       void* param) {
  g_wke_load_begin = {webView, callback, param};
  if (callback) {
    mbSetRequestMockCallback(
        [](const char* url, mbRequestMock* mock, void*) -> int {
          if (!g_wke_load_begin.cb)
            return 0;
          WkeNetJob job;
          job.kind = WkeNetJob::kRequest;
          g_wke_load_begin.cb(g_wke_load_begin.wv, g_wke_load_begin.param, url,
                              &job);
          if (!job.req_mocked)
            return 0;  // callback didn't supply data -> fetch normally
          mbRequestMockResponse(
              mock, job.req_data.data(), static_cast<int>(job.req_data.size()),
              job.req_content_type.empty() ? nullptr
                                           : job.req_content_type.c_str(),
              /*status=*/200);
          return 1;
        },
        nullptr);
  } else {
    mbSetRequestMockCallback(nullptr, nullptr);
  }
}

// miniblink49 parity: from a wkeOnLoadUrlBegin callback, ask to receive the response (the
// wkeOnLoadUrlEnd hook already fires for every response process-wide, so this is a no-op marker).
void wkeNetHookRequest(void* /*job*/) {}

// Set the job's data: for a RESPONSE job (wkeOnLoadUrlEnd) rewrite the fetched body; for a
// REQUEST job (wkeOnLoadUrlBegin) supply a canned response served without a fetch.
void wkeNetSetData(void* j, void* buf, int len) {
  auto* job = static_cast<WkeNetJob*>(j);
  if (!job)
    return;
  if (job->kind == WkeNetJob::kRequest) {
    job->req_mocked = true;
    job->req_data.assign(static_cast<const char*>(buf), buf ? len : 0);
  } else if (buf || len == 0) {
    mbResponseSetBody(job->resp, static_cast<const char*>(buf), len);
  }
}

// Override the Content-Type: a RESPONSE job sets the response header; a REQUEST job sets the
// canned mock's content type (force a payload to parse as a different type).
void wkeNetSetMIMEType(void* j, const char* type) {
  auto* job = static_cast<WkeNetJob*>(j);
  if (!job || !type)
    return;
  if (job->kind == WkeNetJob::kRequest)
    job->req_content_type = type;
  else
    mbResponseSetHeader(job->resp, "Content-Type", type);
}

namespace {
// Encode a wchar_t string (UTF-32 on this platform) to UTF-8 (header names/values are
// ASCII in practice, but encode the general case).
std::string WkeWideToUtf8(const wchar_t* s) {
  std::string out;
  for (; s && *s; ++s) {
    unsigned cp = static_cast<unsigned>(*s);
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}
}  // namespace

// Inject/override an arbitrary response header from a wkeOnLoadUrlEnd callback (job ==
// mbResponse*). Only `response`==true is wired (request-side header mutation needs the
// request-job model). key/value are wide strings (UTF-32 on this platform) per the
// miniblink49 signature; converted to UTF-8.
void wkeNetSetHTTPHeaderField(void* j, wchar_t* key, wchar_t* value, bool response) {
  auto* job = static_cast<WkeNetJob*>(j);
  if (!job || !key || !value)
    return;
  if (job->kind == WkeNetJob::kResponse) {
    if (response)  // response-side: inject/override an arbitrary response header
      mbResponseSetHeader(job->resp, WkeWideToUtf8(key).c_str(),
                          WkeWideToUtf8(value).c_str());
  } else {
    // Request job (wkeOnLoadUrlBegin): the canned-mock API only carries Content-Type, so
    // honor a Content-Type override; other headers aren't representable on a mock.
    if (response && WkeWideToUtf8(key) == "Content-Type")
      job->req_content_type = WkeWideToUtf8(value);
  }
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
