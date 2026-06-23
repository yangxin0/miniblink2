// mb_capi.cc — extern "C" ABI implementation. Pure forwarding to the C++ host;
// no Blink types cross this boundary.

#include "miniblink_host/capi/mb_capi.h"

#include <memory>
#include <string>

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

void mbRunJS(mbView* v, const char* utf8_script) {
  if (v && v->impl)
    v->impl->RunJS(utf8_script);
}

void mbSetInitScript(mbView* v, const char* utf8_script) {
  if (v && v->impl)
    v->impl->SetInitScript(utf8_script);
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

void mbSetCookie(mbView* v, const char* url, const char* cookie) {
  if (v && v->impl)
    v->impl->SetCookie(url, cookie);
}

void mbClearCookies(mbView* v) {
  if (v && v->impl)
    v->impl->ClearCookies();
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

void mbReload(mbView* v) {
  if (v && v->impl)
    v->impl->Reload();
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
