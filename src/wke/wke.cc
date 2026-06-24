// wke.cc — implementation of the wke compatibility slice over mb_capi.
//
// Each wke function is a thin wrapper over the modern mb_capi host. The wkeWebView
// handle wraps an mbView* plus the small per-view state wke exposes synchronously
// (size, the last load's scheme for success reporting, and string caches for the
// const utf8* getters).

#include "wke/wke.h"

#include <cstring>
#include <string>

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

void wkePaint(wkeWebView webView, void* bits, int pitch) {
  if (!webView || !webView->view || !bits)
    return;
  const int stride = pitch > 0 ? pitch : webView->width * 4;
  mbPaintToBitmap(webView->view, bits, webView->width, webView->height, stride);
}
