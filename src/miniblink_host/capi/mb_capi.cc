// mb_capi.cc — extern "C" ABI implementation. Pure forwarding to the C++ host;
// no Blink types cross this boundary.

#include "miniblink_host/capi/mb_capi.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "miniblink_host/loader/mb_url_loader.h"
#include "miniblink_host/runtime/mb_runtime.h"
#include "miniblink_host/view/mb_webview.h"

// Opaque handle: wraps the C++ view.
struct mbView {
  std::unique_ptr<mb::MbWebView> impl;
};

namespace {
// Copy `result` into out[out_cap] (NUL-terminated). When the buffer is too small,
// truncate at a UTF-8 character boundary so the output never ends mid-multibyte
// (which would be invalid UTF-8 — matters for CJK/emoji scraping). Callers
// normally size first with out=NULL, so truncation is the undersized-buffer path.
void CopyToBuffer(const std::string& result, char* out, int out_cap) {
  if (!out || out_cap <= 0)
    return;
  size_t copy = result.size() < static_cast<size_t>(out_cap - 1)
                    ? result.size()
                    : static_cast<size_t>(out_cap - 1);
  // If the byte just past the cut is a UTF-8 continuation byte (10xxxxxx), we'd
  // be splitting a multi-byte sequence — back off to the character boundary.
  // (result[result.size()] is '\0' in C++11+, so the no-truncation case is safe.)
  while (copy > 0 &&
         (static_cast<unsigned char>(result[copy]) & 0xC0) == 0x80)
    --copy;
  std::memcpy(out, result.data(), copy);
  out[copy] = '\0';
}
}  // namespace

extern "C" {

int mbInitialize(void) {
  return mb::MbRuntime::Initialize() ? 1 : 0;
}

void mbShutdown(void) {
  mb::MbRuntime::Shutdown();
}

void mbPumpMessages(void) {
  if (auto* rt = mb::MbRuntime::Get())
    rt->PumpOnce();
}

void mbWait(mbView* v, int ms) {
  if (v && v->impl)
    v->impl->WaitMs(ms);
}

int mbWaitForSelector(mbView* v, const char* css_selector, int timeout_ms) {
  if (!v || !v->impl || !css_selector)
    return 0;
  return v->impl->WaitForSelector(css_selector, timeout_ms) ? 1 : 0;
}

int mbWaitForVisibleSelector(mbView* v, const char* css_selector, int timeout_ms) {
  if (!v || !v->impl || !css_selector)
    return 0;
  return v->impl->WaitForVisibleSelector(css_selector, timeout_ms) ? 1 : 0;
}

int mbWaitForSelectorHidden(mbView* v, const char* css_selector, int timeout_ms) {
  if (!v || !v->impl || !css_selector)
    return 0;
  return v->impl->WaitForSelectorHidden(css_selector, timeout_ms) ? 1 : 0;
}

int mbWaitForNetworkIdle(mbView* v, int idle_ms, int timeout_ms) {
  if (!v || !v->impl)
    return 0;
  return v->impl->WaitForNetworkIdle(idle_ms, timeout_ms) ? 1 : 0;
}

int mbWaitForFunction(mbView* v, const char* js_expr, int timeout_ms) {
  if (!v || !v->impl || !js_expr)
    return 0;
  return v->impl->WaitForFunction(js_expr, timeout_ms) ? 1 : 0;
}

mbView* mbCreateView(int width, int height) {
  if (!mb::MbRuntime::Get())
    return nullptr;  // must mbInitialize() first
  auto view = std::make_unique<mbView>();
  view->impl = mb::MbWebView::Create(width, height);
  if (!view->impl)
    return nullptr;
  return view.release();
}

void mbDestroyView(mbView* v) {
  delete v;  // unique_ptr<MbWebView> dtor closes the WebView
}

void mbResize(mbView* v, int width, int height) {
  if (v && v->impl)
    v->impl->Resize(width, height);
}

void mbLoadHTML(mbView* v, const char* utf8_html, const char* base_url) {
  if (v && v->impl)
    v->impl->LoadHTML(utf8_html, base_url);
}

void mbLoadURL(mbView* v, const char* utf8_url) {
  if (v && v->impl)
    v->impl->LoadURL(utf8_url);
}

void mbOnLoadFinish(mbView* v, mbLoadFinishCallback cb, void* userdata) {
  if (!v || !v->impl)
    return;
  if (cb)
    v->impl->SetLoadFinishCallback([v, cb, userdata]() { cb(v, userdata); });
  else
    v->impl->SetLoadFinishCallback({});
}

int mbIsLoadFinished(mbView* v) {
  return (v && v->impl && v->impl->load_finished()) ? 1 : 0;
}

void mbPostURL(mbView* v, const char* utf8_url, const char* utf8_body,
               const char* content_type) {
  if (v && v->impl)
    v->impl->PostURL(utf8_url, utf8_body,
                     utf8_body ? std::strlen(utf8_body) : 0, content_type);
}

void mbPostURLData(mbView* v, const char* utf8_url, const char* body, int body_len,
                   const char* content_type) {
  if (v && v->impl)
    v->impl->PostURL(utf8_url, body,
                     body_len > 0 ? static_cast<size_t>(body_len) : 0, content_type);
}

void mbRunJS(mbView* v, const char* utf8_script) {
  if (v && v->impl)
    v->impl->RunJS(utf8_script);
}

void mbSetInitScript(mbView* v, const char* utf8_script) {
  if (v && v->impl)
    v->impl->SetInitScript(utf8_script);
}

int mbInsertCSS(mbView* v, const char* css) {
  if (!v || !v->impl || !css)
    return 0;
  return v->impl->InsertCSS(css) ? 1 : 0;
}

void mbSendMouseClick(mbView* v, int x, int y) {
  if (v && v->impl)
    v->impl->SendMouseClick(x, y);
}

void mbSendMouseDown(mbView* v, int x, int y) {
  if (v && v->impl)
    v->impl->SendMouseDown(x, y);
}

void mbSendMouseUp(mbView* v, int x, int y) {
  if (v && v->impl)
    v->impl->SendMouseUp(x, y);
}

void mbSendTouchTap(mbView* v, int x, int y) {
  if (v && v->impl)
    v->impl->SendTouchTap(x, y);
}

void mbSendTouchSwipe(mbView* v, int x1, int y1, int x2, int y2) {
  if (v && v->impl)
    v->impl->SendTouchSwipe(x1, y1, x2, y2);
}

int mbClickSelector(mbView* v, const char* css_selector) {
  if (!v || !v->impl || !css_selector)
    return 0;
  return v->impl->ClickSelector(css_selector) ? 1 : 0;
}

int mbDispatchEvent(mbView* v, const char* css_selector, const char* type) {
  if (!v || !v->impl || !css_selector || !type)
    return 0;
  return v->impl->DispatchEvent(css_selector, type) ? 1 : 0;
}

int mbDragSelector(mbView* v, const char* from_selector,
                   const char* to_selector) {
  if (!v || !v->impl || !from_selector || !to_selector)
    return 0;
  return v->impl->DragSelector(from_selector, to_selector) ? 1 : 0;
}

int mbHoverSelector(mbView* v, const char* css_selector) {
  if (!v || !v->impl || !css_selector)
    return 0;
  return v->impl->HoverSelector(css_selector) ? 1 : 0;
}

int mbScrollIntoView(mbView* v, const char* css_selector) {
  if (!v || !v->impl || !css_selector)
    return 0;
  return v->impl->ScrollIntoView(css_selector) ? 1 : 0;
}

int mbDoubleClickSelector(mbView* v, const char* css_selector) {
  if (!v || !v->impl || !css_selector)
    return 0;
  return v->impl->DoubleClickSelector(css_selector) ? 1 : 0;
}

int mbRightClickSelector(mbView* v, const char* css_selector) {
  if (!v || !v->impl || !css_selector)
    return 0;
  return v->impl->RightClickSelector(css_selector) ? 1 : 0;
}

int mbFocusSelector(mbView* v, const char* css_selector) {
  if (!v || !v->impl || !css_selector)
    return 0;
  return v->impl->FocusSelector(css_selector) ? 1 : 0;
}

int mbBlurSelector(mbView* v, const char* css_selector) {
  if (!v || !v->impl || !css_selector)
    return 0;
  return v->impl->BlurSelector(css_selector) ? 1 : 0;
}

int mbGetContentSize(mbView* v, int* w, int* h) {
  if (!v || !v->impl)
    return 0;
  return v->impl->GetContentSize(w, h) ? 1 : 0;
}

int mbGetViewSize(mbView* v, int* w, int* h) {
  if (!v || !v->impl)
    return 0;
  return v->impl->GetViewSize(w, h) ? 1 : 0;
}

int mbFillSelector(mbView* v, const char* css_selector, const char* utf8_text) {
  if (!v || !v->impl || !css_selector)
    return 0;
  return v->impl->FillSelector(css_selector, utf8_text) ? 1 : 0;
}

int mbSetFileForSelector(mbView* v, const char* css_selector,
                         const char* paths_newline) {
  if (!v || !v->impl || !css_selector)
    return 0;
  return v->impl->SetFileForSelector(css_selector, paths_newline) ? 1 : 0;
}

int mbSelectOption(mbView* v, const char* css_selector, const char* value) {
  if (!v || !v->impl || !css_selector)
    return 0;
  return v->impl->SelectOption(css_selector, value) ? 1 : 0;
}

void mbSendMouseMove(mbView* v, int x, int y) {
  if (v && v->impl)
    v->impl->SendMouseMove(x, y);
}

void mbSetDeviceScaleFactor(mbView* v, float scale) {
  if (v && v->impl)
    v->impl->SetDeviceScaleFactor(scale);
}

void mbSetUserAgent(mbView* v, const char* utf8_ua) {
  if (v && v->impl)
    v->impl->SetUserAgent(utf8_ua);
}

int mbGetUserAgent(mbView* v, char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->GetUserAgent();
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(result.size());
}

void mbSetProxy(const char* proxy) {
  // Process-wide (no view param): applies to every network fetch. A proxy string
  // routes traffic through it; NULL or "" forces a direct connection (overriding
  // *_proxy env vars). Never calling this honors libcurl's env-var defaults.
  mb::MbSetProxy(proxy ? proxy : "");
}

void mbSetIgnoreCertErrors(int ignore) {
  // Process-wide: skip TLS cert verification (like curl -k) when ignore != 0.
  mb::MbSetIgnoreCertErrors(ignore != 0);
}

void mbSetFollowRedirects(int follow) {
  // Process-wide: follow 3xx redirects (default) or stop at the redirect response
  // so mbGetHttpStatus/mbGetResponseHeaders expose the 30x + Location.
  mb::MbSetFollowRedirects(follow != 0);
}

void mbSetTransparentBackground(mbView* v, int transparent) {
  if (v && v->impl)
    v->impl->SetTransparentBackground(transparent != 0);
}

void mbSetLoadImages(mbView* v, int enabled) {
  if (v && v->impl)
    v->impl->SetLoadImages(enabled != 0);
}

void mbSetDarkMode(mbView* v, int dark) {
  if (v && v->impl)
    v->impl->SetDarkMode(dark != 0);
}

void mbSetFocus(mbView* v, int focused) {
  if (v && v->impl)
    v->impl->SetFocus(focused != 0);
}

void mbJsBindFunction(mbView* v, const char* name, mbJsNativeFn fn,
                      void* userdata) {
  if (v && v->impl)
    v->impl->BindJsFunction(name, fn, userdata);
}

void mbSetLocale(mbView* v, const char* utf8_languages) {
  if (v && v->impl)
    v->impl->SetLocale(utf8_languages);
}

void mbSetTimezone(mbView* v, const char* iana_tz) {
  if (v && v->impl)
    v->impl->SetTimezone(iana_tz);
}

void mbSetExtraHeaders(mbView* v, const char* utf8_headers) {
  if (v && v->impl)
    v->impl->SetExtraHeaders(utf8_headers);
}

void mbSendText(mbView* v, const char* utf8_text) {
  if (v && v->impl)
    v->impl->SendText(utf8_text);
}

void mbSendKey(mbView* v, const char* key_name) {
  if (v && v->impl)
    v->impl->SendKey(key_name);
}

void mbSendKeyUp(mbView* v, int windows_key_code) {
  if (v && v->impl)
    v->impl->SendKeyUp(windows_key_code);
}

void mbSendScroll(mbView* v, int x, int y, int dx, int dy) {
  if (v && v->impl)
    v->impl->SendScroll(x, y, dx, dy);
}

void mbScrollTo(mbView* v, int x, int y) {
  if (v && v->impl)
    v->impl->ScrollTo(x, y);
}

int mbScrollToBottom(mbView* v, int max_steps) {
  if (!v || !v->impl)
    return 0;
  return v->impl->ScrollToBottom(max_steps);
}

int mbGetCookies(mbView* v, const char* url, char* out, int out_cap) {
  if (!v || !v->impl || !url)
    return 0;
  std::string result = v->impl->GetCookies(url);
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(result.size());
}

int mbGetCookie(mbView* v, const char* url, const char* name, char* out,
                int out_cap) {
  if (!v || !v->impl || !url || !name)
    return -1;
  std::string result;
  if (!v->impl->GetCookieValue(url, name, &result))
    return -1;  // cookie not present
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(result.size());
}

int mbGetAllCookies(mbView* v, char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->GetAllCookies();
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(result.size());
}

void mbSetCookie(mbView* v, const char* url, const char* cookie) {
  if (v && v->impl)
    v->impl->SetCookie(url, cookie);
}

void mbClearCookies(mbView* v) {
  if (v && v->impl)
    v->impl->ClearCookies();
}

int mbSaveCookies(const char* path) {
  // Process-wide (the jar is shared): write the whole cookie jar to `path`.
  return (path && mb::MbSaveCookies(path)) ? 1 : 0;
}

int mbLoadCookies(const char* path) {
  // Process-wide: load a previously-saved jar from `path` into the shared jar.
  return (path && mb::MbLoadCookies(path)) ? 1 : 0;
}

int mbGetRequestLog(char* out, int out_cap) {
  // Process-wide: newline-separated subresource URLs the loader has fetched.
  std::string result = mb::MbGetRequestLog();
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(result.size());
}

void mbClearRequestLog(void) {
  mb::MbClearRequestLog();
}

void mbBlockUrl(const char* substring) {
  if (substring)
    mb::MbBlockUrl(substring);
}

void mbClearUrlBlocks(void) {
  mb::MbClearUrlBlocks();
}

void mbSetRequestCallback(mbRequestCallback cb, void* userdata) {
  // mbRequestCallback and MbRequestHookFn share the (const char*, void*)->int shape.
  mb::MbSetRequestHook(reinterpret_cast<mb::MbRequestHookFn>(cb), userdata);
}

// The opaque mbResponse handle is a transient view of the loader's per-response state
// (URL + status + mutable body), valid only for the duration of one callback.
struct mbResponse {
  const std::string& url;
  int status;
  std::string* body;
};

namespace {
mbResponseCallback g_response_cb = nullptr;
void* g_response_ud = nullptr;
}  // namespace

void mbSetResponseCallback(mbResponseCallback cb, void* userdata) {
  g_response_cb = cb;
  g_response_ud = userdata;
  if (cb) {
    mb::MbSetResponseHook(
        [](const std::string& url, int status, std::string* body) {
          mbResponse r{url, status, body};
          if (g_response_cb)
            g_response_cb(&r, g_response_ud);
        });
  } else {
    mb::MbSetResponseHook({});
  }
}

const char* mbResponseURL(mbResponse* r) {
  return r ? r->url.c_str() : "";
}

int mbResponseStatus(mbResponse* r) {
  return r ? r->status : 0;
}

const char* mbResponseBody(mbResponse* r, int* out_len) {
  const std::string* b = (r && r->body) ? r->body : nullptr;
  if (out_len)
    *out_len = b ? static_cast<int>(b->size()) : 0;
  return b ? b->data() : "";
}

void mbResponseSetBody(mbResponse* r, const char* body, int len) {
  if (r && r->body && body && len >= 0)
    r->body->assign(body, static_cast<size_t>(len));
}

void mbMockResponse(const char* url_substring, const char* body,
                    const char* content_type, int status) {
  if (url_substring)
    mb::MbAddMock(url_substring, body ? body : "",
                  content_type ? content_type : "", status);
}

void mbClearMocks(void) {
  mb::MbClearMocks();
}

void mbRewriteUrl(const char* from, const char* to) {
  if (from)
    mb::MbAddUrlRewrite(from, to ? to : "");
}

void mbClearUrlRewrites(void) {
  mb::MbClearUrlRewrites();
}

void mbSetRequestHeader(const char* url_substring, const char* name,
                        const char* value) {
  if (url_substring && name)
    mb::MbAddRequestHeader(url_substring, name, value ? value : "");
}

void mbClearRequestHeaders(void) {
  mb::MbClearRequestHeaders();
}

int mbGetLocalStorage(mbView* v, const char* key, char* out, int out_cap) {
  if (!v || !v->impl || !key)
    return -1;
  std::string result;
  if (!v->impl->GetLocalStorage(key, &result))
    return -1;  // absent, or storage unavailable on this origin
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(result.size());
}

int mbSetLocalStorage(mbView* v, const char* key, const char* value) {
  if (!v || !v->impl || !key)
    return 0;
  return v->impl->SetLocalStorage(key, value) ? 1 : 0;
}

int mbGetSessionStorage(mbView* v, const char* key, char* out, int out_cap) {
  if (!v || !v->impl || !key)
    return -1;
  std::string result;
  if (!v->impl->GetSessionStorage(key, &result))
    return -1;
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(result.size());
}

int mbSetSessionStorage(mbView* v, const char* key, const char* value) {
  if (!v || !v->impl || !key)
    return 0;
  return v->impl->SetSessionStorage(key, value) ? 1 : 0;
}

void mbClearStorage(mbView* v) {
  if (v && v->impl)
    v->impl->ClearStorage();
}

int mbDrainConsole(mbView* v, char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->DrainConsole();
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(result.size());
}

int mbGetURL(mbView* v, char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->GetURL();
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(result.size());
}

int mbGetTitle(mbView* v, char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->GetTitle();
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(result.size());
}

int mbGetText(mbView* v, char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->GetText();
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(result.size());
}

int mbGetHTML(mbView* v, char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->GetHTML();
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(result.size());
}

int mbGetTextForSelector(mbView* v, const char* css_selector, char* out,
                         int out_cap) {
  if (!v || !v->impl)
    return -1;
  std::string result;
  if (!v->impl->GetTextForSelector(css_selector, &result))
    return -1;  // no element matched
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(result.size());
}

int mbGetAllTextForSelector(mbView* v, const char* css_selector, char* out,
                            int out_cap) {
  if (!v || !v->impl)
    return -1;
  std::string result;
  if (!v->impl->GetAllTextForSelector(css_selector, &result))
    return -1;  // invalid selector
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(result.size());
}

int mbSetHtmlForSelector(mbView* v, const char* css_selector, const char* html) {
  if (!v || !v->impl || !css_selector)
    return 0;
  return v->impl->SetHtmlForSelector(css_selector, html) ? 1 : 0;
}

int mbGetHtmlForSelector(mbView* v, const char* css_selector, char* out,
                         int out_cap) {
  if (!v || !v->impl)
    return -1;
  std::string result;
  if (!v->impl->GetHtmlForSelector(css_selector, &result))
    return -1;  // no element matched
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(result.size());
}

int mbGetAllValueForSelector(mbView* v, const char* css_selector, char* out,
                             int out_cap) {
  if (!v || !v->impl)
    return -1;
  std::string result;
  if (!v->impl->GetAllValueForSelector(css_selector, &result))
    return -1;  // invalid selector
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(result.size());
}

int mbGetAllAttributeForSelector(mbView* v, const char* css_selector,
                                 const char* attr, char* out, int out_cap) {
  if (!v || !v->impl)
    return -1;
  std::string result;
  if (!v->impl->GetAllAttributeForSelector(css_selector, attr, &result))
    return -1;  // invalid selector
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(result.size());
}

int mbGetAttribute(mbView* v, const char* css_selector, const char* attr,
                   char* out, int out_cap) {
  if (!v || !v->impl)
    return -1;
  std::string result;
  if (!v->impl->GetAttribute(css_selector, attr, &result))
    return -1;  // no element matched, or attribute absent
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(result.size());
}

int mbSetAttribute(mbView* v, const char* css_selector, const char* attr,
                   const char* value) {
  if (!v || !v->impl || !css_selector || !attr)
    return 0;
  return v->impl->SetAttribute(css_selector, attr, value) ? 1 : 0;
}

int mbGetValueForSelector(mbView* v, const char* css_selector, char* out,
                          int out_cap) {
  if (!v || !v->impl)
    return -1;
  std::string result;
  if (!v->impl->GetValueForSelector(css_selector, &result))
    return -1;  // no element matched, or no value property
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(result.size());
}

int mbGetCheckedForSelector(mbView* v, const char* css_selector) {
  if (!v || !v->impl || !css_selector)
    return -1;
  return v->impl->GetCheckedForSelector(css_selector);
}

int mbIsVisibleForSelector(mbView* v, const char* css_selector) {
  if (!v || !v->impl || !css_selector)
    return -1;
  return v->impl->IsVisibleForSelector(css_selector);
}

int mbCountSelector(mbView* v, const char* css_selector) {
  if (!v || !v->impl || !css_selector)
    return -1;
  return v->impl->CountSelector(css_selector);
}

int mbGetComputedStyle(mbView* v, const char* css_selector, const char* property,
                       char* out, int out_cap) {
  if (!v || !v->impl)
    return -1;
  std::string result;
  if (!v->impl->GetComputedStyle(css_selector, property, &result))
    return -1;  // no element matched
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(result.size());
}

void mbReload(mbView* v) {
  if (v && v->impl)
    v->impl->Reload();
}

int mbGetHttpStatus(mbView* v) {
  return (v && v->impl) ? v->impl->GetHttpStatus() : 0;
}

int mbGetResponseHeaders(mbView* v, char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  const std::string& result = v->impl->GetResponseHeaders();
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(result.size());
}

int mbCanGoBack(mbView* v) {
  return (v && v->impl && v->impl->CanGoBack()) ? 1 : 0;
}

int mbCanGoForward(mbView* v) {
  return (v && v->impl && v->impl->CanGoForward()) ? 1 : 0;
}

int mbGoBack(mbView* v) {
  return (v && v->impl && v->impl->GoBack()) ? 1 : 0;
}

int mbGoForward(mbView* v) {
  return (v && v->impl && v->impl->GoForward()) ? 1 : 0;
}

int mbEvalJSIsolated(mbView* v, const char* utf8_script, char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->EvalIsolated(utf8_script);
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(result.size());
}

int mbEvalJS(mbView* v, const char* utf8_script, char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->EvalToString(utf8_script);
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(result.size());
}

int mbGetFrameCount(mbView* v) {
  return (v && v->impl) ? v->impl->GetFrameCount() : 0;
}

int mbEvalJSInFrame(mbView* v, int frame_index, const char* utf8_script, char* out,
                    int out_cap) {
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->EvalInFrame(frame_index, utf8_script);
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(result.size());
}

int mbEvalJSEx(mbView* v, const char* utf8_script, char* out_value,
               int value_cap, char* out_type, int type_cap) {
  if (!v || !v->impl)
    return 0;
  std::string type;
  std::string value = v->impl->EvalWithType(utf8_script, &type);
  CopyToBuffer(value, out_value, value_cap);  // value is arbitrary page content
  CopyToBuffer(type, out_type, type_cap);      // type is ASCII, but stay uniform
  return static_cast<int>(value.size());
}

int mbPaintToBitmap(mbView* v, void* out_bgra, int width, int height, int stride) {
  if (!v || !v->impl)
    return 0;
  return v->impl->PaintToBitmap(out_bgra, width, height, stride) ? 1 : 0;
}

int mbSavePng(mbView* v, const char* path, int width, int height) {
  if (!v || !v->impl || !path)
    return 0;
  return v->impl->SavePng(path, width, height) ? 1 : 0;
}

int mbEncodePng(mbView* v, int width, int height, const unsigned char** out_data) {
  if (!v || !v->impl || width <= 0 || height <= 0)
    return 0;
  if (!v->impl->EncodePng(width, height))
    return 0;
  const std::vector<uint8_t>& data = v->impl->EncodedData();
  if (out_data)
    *out_data = data.data();  // valid until the next mbEncodePng / mbDestroyView
  return static_cast<int>(data.size());
}

int mbSavePdf(mbView* v, const char* path) {
  if (!v || !v->impl || !path)
    return 0;
  return v->impl->SavePdf(path) ? 1 : 0;
}

int mbSavePngRect(mbView* v, const char* path, int x, int y, int w, int h) {
  if (!v || !v->impl || !path)
    return 0;
  return v->impl->SavePngRect(path, x, y, w, h) ? 1 : 0;
}

int mbSaveElementPng(mbView* v, const char* css_selector, const char* path) {
  if (!v || !v->impl || !css_selector || !path)
    return 0;
  return v->impl->SaveElementPng(css_selector, path) ? 1 : 0;
}

int mbGetElementRect(mbView* v, const char* css_selector, int* x, int* y, int* w,
                     int* h) {
  if (!v || !v->impl)
    return 0;
  return v->impl->GetElementRect(css_selector, x, y, w, h) ? 1 : 0;
}

int mbPaintRectToBitmap(mbView* v, void* out_bgra, int x, int y, int w, int h,
                        int stride) {
  if (!v || !v->impl)
    return 0;
  return v->impl->PaintRectToBitmap(out_bgra, x, y, w, h, stride) ? 1 : 0;
}

}  // extern "C"
