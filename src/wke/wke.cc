// wke.cc — implementation of the wke compatibility slice over mb_capi.
//
// Each wke function is a thin wrapper over the modern mb_capi host. The wkeWebView
// handle wraps an mbView* plus the small per-view state wke exposes synchronously
// (size, the last load's scheme for success reporting, and string caches for the
// const utf8* getters).

#include "wke/wke.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "miniblink_host/capi/mb_capi.h"

struct _tagWkeWebView {
  mbView* view = nullptr;
  int width = 0;
  int height = 0;
  bool last_was_http = false;  // success reporting: http uses the status code
  std::string url_cache;       // backs wkeGetURL's const utf8* return
  std::string title_cache;     // backs wkeGetTitle's const utf8* return
};

namespace {
bool IsHttpUrl(const utf8* url) {
  return url && (std::strncmp(url, "http://", 7) == 0 ||
                 std::strncmp(url, "https://", 8) == 0);
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
  return wv;
}

void wkeDestroyWebView(wkeWebView webView) {
  if (!webView)
    return;
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
}

void wkeLoadHTML(wkeWebView webView, const utf8* html) {
  if (!webView || !webView->view)
    return;
  webView->last_was_http = false;
  mbLoadHTML(webView->view, html, "about:blank");
  mbWait(webView->view, 30);
}

void wkeReload(wkeWebView webView) {
  if (webView && webView->view)
    mbReload(webView->view);
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

void wkeSetTransparent(wkeWebView webView, bool transparent) {
  if (webView && webView->view)
    mbSetTransparentBackground(webView->view, transparent ? 1 : 0);
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
// jsValue handle -> the string result of the wkeRunJS that produced it. Heap-
// owned with no destructor (Blink builds with -Wexit-time-destructors).
std::map<jsValue, std::string>& JsRegistry() {
  static auto* r = new std::map<jsValue, std::string>();
  return *r;
}
std::string& JsTempBuf() {
  static auto* s = new std::string();
  return *s;
}
// Start above wke's small reserved constants (jsUndefined/jsNull/jsTrue/jsFalse).
jsValue g_next_js_value = 0x10000;

const std::string* JsLookup(jsValue v) {
  auto& reg = JsRegistry();
  auto it = reg.find(v);
  return it == reg.end() ? nullptr : &it->second;
}
}  // namespace

jsValue wkeRunJS(wkeWebView webView, const utf8* script) {
  if (!webView || !webView->view || !script)
    return 0;
  std::vector<char> buf(1 << 16, 0);  // 64 KiB result cap
  mbEvalJS(webView->view, script, buf.data(), static_cast<int>(buf.size()));
  auto& reg = JsRegistry();
  if (reg.size() >= 4096)
    reg.clear();  // bound the (non-GC'd) registry; very old handles expire to ""
  const jsValue handle = g_next_js_value++;
  reg[handle] = buf.data();  // up to the first NUL
  return handle;
}

jsExecState wkeGlobalExec(wkeWebView webView) {
  // No real exec state is needed — the jsValue handle carries the result. Return
  // a non-null token so callers' null checks pass.
  return reinterpret_cast<jsExecState>(webView);
}

int jsToInt(jsExecState /*es*/, jsValue v) {
  const std::string* s = JsLookup(v);
  return s ? std::atoi(s->c_str()) : 0;
}

double jsToDouble(jsExecState /*es*/, jsValue v) {
  const std::string* s = JsLookup(v);
  return s ? std::atof(s->c_str()) : 0.0;
}

bool jsToBoolean(jsExecState /*es*/, jsValue v) {
  const std::string* s = JsLookup(v);
  if (!s)
    return false;
  // JS truthiness over the coerced string: "true" / any non-empty value that
  // isn't "false" or "0".
  return *s == "true" || (!s->empty() && *s != "false" && *s != "0");
}

const utf8* jsToTempString(jsExecState /*es*/, jsValue v) {
  const std::string* s = JsLookup(v);
  JsTempBuf() = s ? *s : std::string();
  return JsTempBuf().c_str();
}
