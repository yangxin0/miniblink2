// mb_capi.cc — extern "C" ABI implementation. Pure forwarding to the C++ host;
// no Blink types cross this boundary.

#include "miniblink_host/capi/mb_capi.h"

#include <cstdint>
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

void mbPostURL(mbView* v, const char* utf8_url, const char* utf8_body,
               const char* content_type) {
  if (v && v->impl)
    v->impl->PostURL(utf8_url, utf8_body, content_type);
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

int mbClickSelector(mbView* v, const char* css_selector) {
  if (!v || !v->impl || !css_selector)
    return 0;
  return v->impl->ClickSelector(css_selector) ? 1 : 0;
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

int mbFillSelector(mbView* v, const char* css_selector, const char* utf8_text) {
  if (!v || !v->impl || !css_selector)
    return 0;
  return v->impl->FillSelector(css_selector, utf8_text) ? 1 : 0;
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
  if (out && out_cap > 0) {
    int n = static_cast<int>(result.size());
    int copy = n < out_cap - 1 ? n : out_cap - 1;
    for (int i = 0; i < copy; ++i)
      out[i] = result[i];
    out[copy] = '\0';
  }
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
  if (out && out_cap > 0) {
    int n = static_cast<int>(result.size());
    int copy = n < out_cap - 1 ? n : out_cap - 1;
    for (int i = 0; i < copy; ++i)
      out[i] = result[i];
    out[copy] = '\0';
  }
  return static_cast<int>(result.size());
}

int mbGetAllCookies(mbView* v, char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->GetAllCookies();
  if (out && out_cap > 0) {
    int n = static_cast<int>(result.size());
    int copy = n < out_cap - 1 ? n : out_cap - 1;
    for (int i = 0; i < copy; ++i)
      out[i] = result[i];
    out[copy] = '\0';
  }
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
  if (out && out_cap > 0) {
    int n = static_cast<int>(result.size());
    int copy = n < out_cap - 1 ? n : out_cap - 1;
    for (int i = 0; i < copy; ++i)
      out[i] = result[i];
    out[copy] = '\0';
  }
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

int mbGetLocalStorage(mbView* v, const char* key, char* out, int out_cap) {
  if (!v || !v->impl || !key)
    return -1;
  std::string result;
  if (!v->impl->GetLocalStorage(key, &result))
    return -1;  // absent, or storage unavailable on this origin
  if (out && out_cap > 0) {
    int n = static_cast<int>(result.size());
    int copy = n < out_cap - 1 ? n : out_cap - 1;
    for (int i = 0; i < copy; ++i)
      out[i] = result[i];
    out[copy] = '\0';
  }
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
  if (out && out_cap > 0) {
    int n = static_cast<int>(result.size());
    int copy = n < out_cap - 1 ? n : out_cap - 1;
    for (int i = 0; i < copy; ++i)
      out[i] = result[i];
    out[copy] = '\0';
  }
  return static_cast<int>(result.size());
}

int mbSetSessionStorage(mbView* v, const char* key, const char* value) {
  if (!v || !v->impl || !key)
    return 0;
  return v->impl->SetSessionStorage(key, value) ? 1 : 0;
}

int mbDrainConsole(mbView* v, char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->DrainConsole();
  if (out && out_cap > 0) {
    int n = static_cast<int>(result.size());
    int copy = n < out_cap - 1 ? n : out_cap - 1;
    for (int i = 0; i < copy; ++i)
      out[i] = result[i];
    out[copy] = '\0';
  }
  return static_cast<int>(result.size());
}

int mbGetURL(mbView* v, char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->GetURL();
  if (out && out_cap > 0) {
    int n = static_cast<int>(result.size());
    int copy = n < out_cap - 1 ? n : out_cap - 1;
    for (int i = 0; i < copy; ++i)
      out[i] = result[i];
    out[copy] = '\0';
  }
  return static_cast<int>(result.size());
}

int mbGetTitle(mbView* v, char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->GetTitle();
  if (out && out_cap > 0) {
    int n = static_cast<int>(result.size());
    int copy = n < out_cap - 1 ? n : out_cap - 1;
    for (int i = 0; i < copy; ++i)
      out[i] = result[i];
    out[copy] = '\0';
  }
  return static_cast<int>(result.size());
}

int mbGetText(mbView* v, char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->GetText();
  if (out && out_cap > 0) {
    int n = static_cast<int>(result.size());
    int copy = n < out_cap - 1 ? n : out_cap - 1;
    for (int i = 0; i < copy; ++i)
      out[i] = result[i];
    out[copy] = '\0';
  }
  return static_cast<int>(result.size());
}

int mbGetHTML(mbView* v, char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->GetHTML();
  if (out && out_cap > 0) {
    int n = static_cast<int>(result.size());
    int copy = n < out_cap - 1 ? n : out_cap - 1;
    for (int i = 0; i < copy; ++i)
      out[i] = result[i];
    out[copy] = '\0';
  }
  return static_cast<int>(result.size());
}

int mbGetTextForSelector(mbView* v, const char* css_selector, char* out,
                         int out_cap) {
  if (!v || !v->impl)
    return -1;
  std::string result;
  if (!v->impl->GetTextForSelector(css_selector, &result))
    return -1;  // no element matched
  if (out && out_cap > 0) {
    int n = static_cast<int>(result.size());
    int copy = n < out_cap - 1 ? n : out_cap - 1;
    for (int i = 0; i < copy; ++i)
      out[i] = result[i];
    out[copy] = '\0';
  }
  return static_cast<int>(result.size());
}

int mbGetAllTextForSelector(mbView* v, const char* css_selector, char* out,
                            int out_cap) {
  if (!v || !v->impl)
    return -1;
  std::string result;
  if (!v->impl->GetAllTextForSelector(css_selector, &result))
    return -1;  // invalid selector
  if (out && out_cap > 0) {
    int n = static_cast<int>(result.size());
    int copy = n < out_cap - 1 ? n : out_cap - 1;
    for (int i = 0; i < copy; ++i)
      out[i] = result[i];
    out[copy] = '\0';
  }
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
  if (out && out_cap > 0) {
    int n = static_cast<int>(result.size());
    int copy = n < out_cap - 1 ? n : out_cap - 1;
    for (int i = 0; i < copy; ++i)
      out[i] = result[i];
    out[copy] = '\0';
  }
  return static_cast<int>(result.size());
}

int mbGetAllValueForSelector(mbView* v, const char* css_selector, char* out,
                             int out_cap) {
  if (!v || !v->impl)
    return -1;
  std::string result;
  if (!v->impl->GetAllValueForSelector(css_selector, &result))
    return -1;  // invalid selector
  if (out && out_cap > 0) {
    int n = static_cast<int>(result.size());
    int copy = n < out_cap - 1 ? n : out_cap - 1;
    for (int i = 0; i < copy; ++i)
      out[i] = result[i];
    out[copy] = '\0';
  }
  return static_cast<int>(result.size());
}

int mbGetAllAttributeForSelector(mbView* v, const char* css_selector,
                                 const char* attr, char* out, int out_cap) {
  if (!v || !v->impl)
    return -1;
  std::string result;
  if (!v->impl->GetAllAttributeForSelector(css_selector, attr, &result))
    return -1;  // invalid selector
  if (out && out_cap > 0) {
    int n = static_cast<int>(result.size());
    int copy = n < out_cap - 1 ? n : out_cap - 1;
    for (int i = 0; i < copy; ++i)
      out[i] = result[i];
    out[copy] = '\0';
  }
  return static_cast<int>(result.size());
}

int mbGetAttribute(mbView* v, const char* css_selector, const char* attr,
                   char* out, int out_cap) {
  if (!v || !v->impl)
    return -1;
  std::string result;
  if (!v->impl->GetAttribute(css_selector, attr, &result))
    return -1;  // no element matched, or attribute absent
  if (out && out_cap > 0) {
    int n = static_cast<int>(result.size());
    int copy = n < out_cap - 1 ? n : out_cap - 1;
    for (int i = 0; i < copy; ++i)
      out[i] = result[i];
    out[copy] = '\0';
  }
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
  if (out && out_cap > 0) {
    int n = static_cast<int>(result.size());
    int copy = n < out_cap - 1 ? n : out_cap - 1;
    for (int i = 0; i < copy; ++i)
      out[i] = result[i];
    out[copy] = '\0';
  }
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
  if (out && out_cap > 0) {
    int n = static_cast<int>(result.size());
    int copy = n < out_cap - 1 ? n : out_cap - 1;
    for (int i = 0; i < copy; ++i)
      out[i] = result[i];
    out[copy] = '\0';
  }
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
  if (out && out_cap > 0) {
    int n = static_cast<int>(result.size());
    int copy = n < out_cap - 1 ? n : out_cap - 1;
    for (int i = 0; i < copy; ++i)
      out[i] = result[i];
    out[copy] = '\0';
  }
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
  if (out && out_cap > 0) {
    int n = static_cast<int>(result.size());
    int copy = n < out_cap - 1 ? n : out_cap - 1;
    for (int i = 0; i < copy; ++i)
      out[i] = result[i];
    out[copy] = '\0';
  }
  return static_cast<int>(result.size());
}

int mbEvalJS(mbView* v, const char* utf8_script, char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->EvalToString(utf8_script);
  if (out && out_cap > 0) {
    int n = static_cast<int>(result.size());
    int copy = n < out_cap - 1 ? n : out_cap - 1;
    for (int i = 0; i < copy; ++i)
      out[i] = result[i];
    out[copy] = '\0';
  }
  return static_cast<int>(result.size());
}

int mbEvalJSEx(mbView* v, const char* utf8_script, char* out_value,
               int value_cap, char* out_type, int type_cap) {
  if (!v || !v->impl)
    return 0;
  std::string type;
  std::string value = v->impl->EvalWithType(utf8_script, &type);
  if (out_value && value_cap > 0) {
    int n = static_cast<int>(value.size());
    int copy = n < value_cap - 1 ? n : value_cap - 1;
    for (int i = 0; i < copy; ++i)
      out_value[i] = value[i];
    out_value[copy] = '\0';
  }
  if (out_type && type_cap > 0) {
    int n = static_cast<int>(type.size());
    int copy = n < type_cap - 1 ? n : type_cap - 1;
    for (int i = 0; i < copy; ++i)
      out_type[i] = type[i];
    out_type[copy] = '\0';
  }
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
