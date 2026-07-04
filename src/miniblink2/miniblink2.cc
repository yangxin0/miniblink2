// miniblink2.cc — extern "C" implementation of the miniblink2 public API (mb* ABI). Pure forwarding to the C++ host;
// no Blink types cross this boundary.

#include "miniblink2/automation.h"
#include "miniblink2/view.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "miniblink_host/frame/mb_frame_broker.h"
#include "miniblink_host/frame/mb_indexeddb.h"
#include "miniblink_host/frame/mb_opfs.h"
#include "miniblink_host/frame/mb_notification_service.h"
#include "miniblink_host/loader/mb_url_loader.h"
#include "miniblink_host/runtime/mb_runtime.h"
#include "miniblink_host/session/mb_session.h"
#include "miniblink_host/view/mb_webview.h"

// --- ThinLTO keep-alive for the Rust global allocator ------------------------------------
// rust_allocator_internal::{alloc,dealloc,realloc,alloc_zeroed,alloc_error_handler_impl} are
// DEFINED in build/rust/allocator/allocator_impls.o (bitcode) but REFERENCED only by the NATIVE
// Rust allocator rlib. In a --size-optimized (ThinLTO) build LTO sees no *bitcode* user and
// internalizes/drops them, so a static libminiblink2.a link leaves the native rlib's references
// undefined (the dylib is unaffected — it never force-pulls that rlib). This mb_capi object is
// ALWAYS pulled into any consumer link (every mb* entry point lives here), so referencing
// the allocator from it forces LTO to keep the real definitions live — the static .a is then
// self-sufficient with NO consumer -Wl,-u. This is NOT a trap stub (cf. mb_dawn_stubs.cc): these
// run on every Rust allocation and must resolve to the real PartitionAlloc-backed functions.
namespace rust_allocator_internal {
unsigned char* alloc(size_t size, size_t align);
void dealloc(unsigned char* p, size_t size, size_t align);
unsigned char* realloc(unsigned char* p, size_t old_size, size_t align, size_t new_size);
unsigned char* alloc_zeroed(size_t size, size_t align);
void alloc_error_handler_impl();
}  // namespace rust_allocator_internal
extern "C" __attribute__((used, retain)) void* const mb_rust_alloc_keep[] = {
    reinterpret_cast<void*>(&rust_allocator_internal::alloc),
    reinterpret_cast<void*>(&rust_allocator_internal::dealloc),
    reinterpret_cast<void*>(&rust_allocator_internal::realloc),
    reinterpret_cast<void*>(&rust_allocator_internal::alloc_zeroed),
    reinterpret_cast<void*>(&rust_allocator_internal::alloc_error_handler_impl),
};

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

namespace {

// Engine-call depth for the C ABI: >0 while any engine-entering export is on
// the stack. Single-threaded by contract (the engine lives on the caller's
// main thread), so a plain int suffices.
int g_engine_depth = 0;
struct EngineScope {
  EngineScope() { ++g_engine_depth; }
  ~EngineScope() { --g_engine_depth; }
  EngineScope(const EngineScope&) = delete;
  EngineScope& operator=(const EngineScope&) = delete;
};

// Callbacks parked by mbDefer until the engine is off the stack.
std::vector<std::pair<mbDeferredCallback, void*>>& DeferredQueue() {
  // Intentionally leaked: no exit-time destructor (the process outlives it).
  static auto* q = new std::vector<std::pair<mbDeferredCallback, void*>>();
  return *q;
}

void DrainDeferred() {
  // Swap out first: a drained callback may mbDefer again (repark for the next
  // update) or start a load that pumps - the queue must not be iterated live.
  std::vector<std::pair<mbDeferredCallback, void*>> ready;
  ready.swap(DeferredQueue());
  for (auto& [cb, userdata] : ready)
    cb(userdata);
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
  EngineScope engine_scope;
  if (auto* rt = mb::MbRuntime::Get())
    rt->PumpOnce();
}

namespace {
double g_max_update_time = 0;  // seconds; 0 = drain to idle
}  // namespace

void mbSetMaxUpdateTime(double seconds) {
  g_max_update_time = seconds > 0 ? seconds : 0;
}

void mbUpdate(void) {
  if (g_engine_depth > 0)
    return;  // fired inside an engine call; the outermost caller updates next
  {
    EngineScope engine_scope;
    if (auto* rt = mb::MbRuntime::Get())
      rt->PumpOnce(g_max_update_time);
  }
  DrainDeferred();
}

void mbPurgeMemory(void) {
  EngineScope engine_scope;
  if (auto* rt = mb::MbRuntime::Get())
    rt->PurgeMemory();
}

void mbLogMemoryUsage(void) {
  if (auto* rt = mb::MbRuntime::Get())
    rt->LogMemoryUsage();
}

int mbInEngineCall(void) {
  return g_engine_depth > 0 ? 1 : 0;
}

void mbDefer(mbDeferredCallback cb, void* userdata) {
  if (!cb)
    return;
  if (g_engine_depth == 0) {
    cb(userdata);
    return;
  }
  DeferredQueue().emplace_back(cb, userdata);
}

void mbWait(mbView* v, int ms) {
  EngineScope engine_scope;
  if (v && v->impl)
    v->impl->WaitMs(ms);
}

int mbWaitForSelector(mbView* v, const char* css_selector, int timeout_ms) {
  EngineScope engine_scope;
  if (!v || !v->impl || !css_selector)
    return 0;
  return v->impl->WaitForSelector(css_selector, timeout_ms) ? 1 : 0;
}

int mbWaitForVisibleSelector(mbView* v, const char* css_selector, int timeout_ms) {
  EngineScope engine_scope;
  if (!v || !v->impl || !css_selector)
    return 0;
  return v->impl->WaitForVisibleSelector(css_selector, timeout_ms) ? 1 : 0;
}

int mbWaitForSelectorHidden(mbView* v, const char* css_selector, int timeout_ms) {
  EngineScope engine_scope;
  if (!v || !v->impl || !css_selector)
    return 0;
  return v->impl->WaitForSelectorHidden(css_selector, timeout_ms) ? 1 : 0;
}

int mbWaitForNetworkIdle(mbView* v, int idle_ms, int timeout_ms) {
  EngineScope engine_scope;
  if (!v || !v->impl)
    return 0;
  return v->impl->WaitForNetworkIdle(idle_ms, timeout_ms) ? 1 : 0;
}

int mbWaitForFunction(mbView* v, const char* js_expr, int timeout_ms) {
  EngineScope engine_scope;
  if (!v || !v->impl || !js_expr)
    return 0;
  return v->impl->WaitForFunction(js_expr, timeout_ms) ? 1 : 0;
}

struct mbSession {
  mb::MbSession* impl = nullptr;
};

mbSession* mbCreateSession(const char* name, const char* persist_path) {
  EngineScope engine_scope;
  auto s = std::make_unique<mbSession>();
  s->impl = mb::MbSession::Create(name && *name ? name : "unnamed",
                                  persist_path ? persist_path : "");
  s->impl->set_host_handle(s.get());
  return s.release();
}

void mbDestroySession(mbSession* s) {
  if (!s)
    return;
  if (s->impl)
    s->impl->Detach();
  delete s;
}

mbSession* mbDefaultSession(void) {
  static mbSession* def = [] {
    auto* h = new mbSession();
    h->impl = mb::MbSession::Default();
    h->impl->set_host_handle(h);
    return h;
  }();
  return def;
}

void mbSessionClearStorage(mbSession* s) {
  EngineScope engine_scope;
  if (s && s->impl)
    s->impl->ClearStorage();
}

void mbSessionFlush(mbSession* s) {
  EngineScope engine_scope;
  if (s && s->impl)
    s->impl->FlushToDisk();
}

mbView* mbCreateViewInSession(int width, int height, mbSession* session) {
  mbView* v = mbCreateView(width, height);
  if (v && v->impl && session && session->impl)
    v->impl->SetSession(session->impl);
  return v;
}

mbSession* mbViewGetSession(mbView* v) {
  if (!v || !v->impl || !v->impl->session())
    return mbDefaultSession();
  mbSession* h = v->impl->session()->host_handle();
  return h ? h : mbDefaultSession();
}

mbView* mbCreateView(int width, int height) {
  EngineScope engine_scope;
  if (!mb::MbRuntime::Get())
    return nullptr;  // must mbInitialize() first
  auto view = std::make_unique<mbView>();
  view->impl = mb::MbWebView::Create(width, height);
  if (!view->impl)
    return nullptr;
  return view.release();
}

void mbDestroyView(mbView* v) {
  EngineScope engine_scope;
  delete v;  // unique_ptr<MbWebView> dtor closes the WebView
}

void mbSetCompositingEnabled(int on) {
  mb::MbWebView::SetCompositingEnabled(on != 0);
}

void mbSetScriptTimeout(int ms) {
  if (auto* rt = mb::MbRuntime::Get())
    rt->SetScriptTimeoutMs(ms);
}

int mbViewFrameSinkRequested(mbView* v) {
  if (!v || !v->impl)
    return -1;
  return v->impl->CompositorFrameSinkCount();
}

void mbViewComposite(mbView* v) {
  EngineScope engine_scope;
  if (v && v->impl)
    v->impl->Composite();
}

unsigned int mbViewCompositorPixel(mbView* v, int x, int y) {
  if (!v || !v->impl)
    return 0;
  return v->impl->CompositorPixel(x, y);
}

void mbResize(mbView* v, int width, int height) {
  EngineScope engine_scope;
  if (v && v->impl)
    v->impl->Resize(width, height);
}

void mbLoadHTML(mbView* v, const char* utf8_html, const char* base_url) {
  EngineScope engine_scope;
  if (v && v->impl)
    v->impl->LoadHTML(utf8_html, base_url);
}

void mbLoadURL(mbView* v, const char* utf8_url) {
  EngineScope engine_scope;
  if (v && v->impl)
    v->impl->LoadURL(utf8_url);
}

int mbDownloadURL(mbView* v, const char* url, const char* dest_path) {
  EngineScope engine_scope;
  if (!v || !v->impl || !url || !dest_path)
    return 0;
  return v->impl->DownloadURL(url, dest_path) ? 1 : 0;
}

void mbOnLoadFinish(mbView* v, mbLoadFinishCallback cb, void* userdata) {
  if (!v || !v->impl)
    return;
  if (cb)
    v->impl->SetLoadFinishCallback([v, cb, userdata]() { cb(v, userdata); });
  else
    v->impl->SetLoadFinishCallback({});
}

void mbOnBeginLoading(mbView* v, mbBeginLoadingCallback cb, void* userdata) {
  if (!v || !v->impl)
    return;
  if (cb)
    v->impl->SetBeginLoadingCallback(
        [v, cb, userdata](const std::string& url) { cb(v, userdata, url.c_str()); });
  else
    v->impl->SetBeginLoadingCallback({});
}

void mbOnFailLoading(mbView* v, mbFailLoadingCallback cb, void* userdata) {
  if (!v || !v->impl)
    return;
  if (cb)
    v->impl->SetFailLoadingCallback(
        [v, cb, userdata](const std::string& url, const std::string& error) {
          cb(v, userdata, url.c_str(), error.c_str());
        });
  else
    v->impl->SetFailLoadingCallback({});
}

void mbOnDOMContentLoaded(mbView* v, mbDOMContentLoadedCallback cb, void* userdata) {
  if (!v || !v->impl)
    return;
  if (cb)
    v->impl->SetDOMContentLoadedCallback([v, cb, userdata]() { cb(v, userdata); });
  else
    v->impl->SetDOMContentLoadedCallback({});
}

int mbIsLoadFinished(mbView* v) {
  return (v && v->impl && v->impl->load_finished()) ? 1 : 0;
}

void mbOnNavigation(mbView* v, mbNavigationCallback cb, void* userdata) {
  if (!v || !v->impl)
    return;
  if (cb)
    v->impl->SetNavigationCallback(
        [v, cb, userdata](const std::string& url) { return cb(v, userdata, url.c_str()); });
  else
    v->impl->SetNavigationCallback({});
}

void mbOnUrlChanged(mbView* v, mbUrlChangedCallback cb, void* userdata) {
  if (!v || !v->impl)
    return;
  if (cb)
    v->impl->SetUrlChangedCallback(
        [v, cb, userdata](const std::string& url) { cb(v, userdata, url.c_str()); });
  else
    v->impl->SetUrlChangedCallback({});
}

void mbOnTitleChanged(mbView* v, mbTitleChangedCallback cb, void* userdata) {
  if (!v || !v->impl)
    return;
  if (cb)
    v->impl->SetTitleChangedCallback([v, cb, userdata](const std::string& title) {
      cb(v, userdata, title.c_str());
    });
  else
    v->impl->SetTitleChangedCallback({});
}

void mbOnFaviconChanged(mbView* v, mbFaviconChangedCallback cb, void* userdata) {
  if (!v || !v->impl)
    return;
  if (cb)
    v->impl->SetFaviconChangedCallback(
        [v, cb, userdata](const std::string& urls) {
          cb(v, userdata, urls.c_str());
        });
  else
    v->impl->SetFaviconChangedCallback({});
}

void mbOnDownload(mbView* v, mbDownloadCallback cb, void* userdata) {
  if (!v || !v->impl)
    return;
  if (cb)
    v->impl->SetDownloadCallback(
        [v, cb, userdata](const std::string& url, const std::string& mime,
                          const std::string& filename, const std::string& body) {
          cb(v, userdata, url.c_str(), mime.c_str(), filename.c_str(),
             body.data(), static_cast<int>(body.size()));
        });
  else
    v->impl->SetDownloadCallback({});
}

void mbOnNewWindow(mbView* v, mbNewWindowCallback cb, void* userdata) {
  if (!v || !v->impl)
    return;
  if (cb)
    v->impl->SetNewWindowCallback(
        [v, cb, userdata](const std::string& url, const std::string& name) {
          cb(v, userdata, url.c_str(), name.c_str());
        });
  else
    v->impl->SetNewWindowCallback({});
}

void mbPostURL(mbView* v, const char* utf8_url, const char* utf8_body,
               const char* content_type) {
  EngineScope engine_scope;
  if (v && v->impl)
    v->impl->PostURL(utf8_url, utf8_body,
                     utf8_body ? std::strlen(utf8_body) : 0, content_type);
}

void mbPostURLData(mbView* v, const char* utf8_url, const char* body, int body_len,
                   const char* content_type) {
  EngineScope engine_scope;
  if (v && v->impl)
    v->impl->PostURL(utf8_url, body,
                     body_len > 0 ? static_cast<size_t>(body_len) : 0, content_type);
}

void mbRunJS(mbView* v, const char* utf8_script) {
  EngineScope engine_scope;
  if (v && v->impl)
    v->impl->RunJS(utf8_script);
}

void mbSetInitScript(mbView* v, const char* utf8_script) {
  if (v && v->impl)
    v->impl->SetInitScript(utf8_script);
}

void mbSetUserStylesheet(mbView* v, const char* css) {
  EngineScope engine_scope;
  if (v && v->impl)
    v->impl->SetUserStylesheet(css);
}

int mbEvalJSCatch(mbView* v, const char* utf8_script, char* out_value,
                  int value_cap, char* out_exception, int exc_cap) {
  EngineScope engine_scope;
  if (!v || !v->impl)
    return 0;
  std::string exc;
  std::string value = v->impl->EvalCatch(utf8_script, &exc);
  CopyToBuffer(value, out_value, value_cap);
  CopyToBuffer(exc, out_exception, exc_cap);
  return static_cast<int>(std::min<size_t>(value.size(), INT_MAX));
}

int mbInsertCSS(mbView* v, const char* css) {
  EngineScope engine_scope;
  if (!v || !v->impl || !css)
    return 0;
  return v->impl->InsertCSS(css) ? 1 : 0;
}

void mbSendMouseClick(mbView* v, int x, int y) {
  EngineScope engine_scope;
  if (v && v->impl)
    v->impl->SendMouseClick(x, y);
}

void mbSendMouseClickEx(mbView* v, int x, int y, int button, int modifiers) {
  EngineScope engine_scope;
  if (v && v->impl)
    v->impl->SendMouseClickEx(x, y, button, modifiers);
}

void mbSendMouseDown(mbView* v, int x, int y) {
  EngineScope engine_scope;
  if (v && v->impl)
    v->impl->SendMouseDown(x, y);
}

void mbSendMouseUp(mbView* v, int x, int y) {
  EngineScope engine_scope;
  if (v && v->impl)
    v->impl->SendMouseUp(x, y);
}

void mbSendTouchTap(mbView* v, int x, int y) {
  EngineScope engine_scope;
  if (v && v->impl)
    v->impl->SendTouchTap(x, y);
}

void mbSendTouchSwipe(mbView* v, int x1, int y1, int x2, int y2) {
  EngineScope engine_scope;
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

int mbDragDropSelector(mbView* v, const char* from_selector,
                       const char* to_selector) {
  if (!v || !v->impl || !from_selector || !to_selector)
    return 0;
  return v->impl->DragDropSelector(from_selector, to_selector) ? 1 : 0;
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

void mbSetJsDialogCallback(mbView* v, mbJsDialogCallback cb, void* userdata) {
  if (!v || !v->impl)
    return;
  // Wrap the C callback + userdata in a std::function instead of reinterpret_cast-ing
  // the C fn-ptr to the host type (the two are layout-identical but differ in C/C++
  // language linkage — calling through a punned pointer is UB and -Werror-fragile).
  if (!cb) {
    v->impl->SetJsDialogCallback({});
    return;
  }
  v->impl->SetJsDialogCallback(
      [cb, userdata](int type, const char* message, const char* default_value,
                     char* out_value, int out_cap) {
        return cb(type, message, default_value, out_value, out_cap, userdata);
      });
}

int mbSelectOption(mbView* v, const char* css_selector, const char* value) {
  if (!v || !v->impl || !css_selector)
    return 0;
  return v->impl->SelectOption(css_selector, value) ? 1 : 0;
}

void mbSendMouseEvent(mbView* v, const mbMouseEvent* e) {
  EngineScope engine_scope;
  if (!v || !v->impl || !e ||
      e->struct_size < static_cast<int>(sizeof(mbMouseEvent)))
    return;
  v->impl->SendMouseEvent(e->type, e->x, e->y, e->button, e->click_count,
                          e->modifiers);
}

int mbSendWheelEvent(mbView* v, const mbWheelEvent* e) {
  EngineScope engine_scope;
  if (!v || !v->impl || !e ||
      e->struct_size < static_cast<int>(sizeof(mbWheelEvent)) || e->phase != 0)
    return 0;
  return v->impl->SendWheelEx(e->x, e->y, e->delta_x, e->delta_y,
                              e->precise != 0, e->modifiers)
             ? 1
             : 0;
}

void mbSendMouseMove(mbView* v, int x, int y) {
  EngineScope engine_scope;
  if (v && v->impl)
    v->impl->SendMouseMove(x, y);
}

void mbSendWheel(mbView* v, int x, int y, int deltaX, int deltaY, int modifiers) {
  EngineScope engine_scope;
  if (v && v->impl)
    v->impl->SendWheel(x, y, deltaX, deltaY, modifiers);
}

void mbSetDeviceScaleFactor(mbView* v, float scale) {
  if (v && v->impl)
    v->impl->SetDeviceScaleFactor(scale);
}

void mbSetZoomFactor(mbView* v, float factor) {
  if (v && v->impl)
    v->impl->SetZoomFactor(factor);
}

int mbExecuteEditCommand(mbView* v, const char* command) {
  return (v && v->impl && v->impl->ExecuteEditCommand(command)) ? 1 : 0;
}

int mbExecuteEditCommandValue(mbView* v, const char* command, const char* value) {
  return (v && v->impl && v->impl->ExecuteEditCommandValue(command, value)) ? 1 : 0;
}

float mbGetZoomFactor(mbView* v) {
  return (v && v->impl) ? v->impl->GetZoomFactor() : 1.0f;
}

void mbEmulateDevice(mbView* v, int width, int height, float deviceScaleFactor,
                     int mobile) {
  if (v && v->impl)
    v->impl->EmulateDevice(width, height, deviceScaleFactor, mobile != 0);
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
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
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

void mbEmulateMedia(mbView* v, const char* feature, const char* value) {
  if (v && v->impl)
    v->impl->EmulateMedia(feature, value);
}

void mbEmulateMediaType(mbView* v, const char* media_type) {
  if (v && v->impl)
    v->impl->EmulateMediaType(media_type);
}

void mbSetFocus(mbView* v, int focused) {
  if (v && v->impl)
    v->impl->SetFocus(focused != 0);
}

void mbSetVisibility(mbView* v, int visible) {
  if (v && v->impl)
    v->impl->SetVisible(visible != 0);
}

void mbJsBindFunction(mbView* v, const char* name, mbJsNativeFn fn,
                      void* userdata) {
  if (!v || !v->impl || !name || !fn)
    return;
  // Wrap the C callback + userdata in a std::function instead of passing the
  // C-linkage fn-ptr through the host's C++ typedef and calling it through that
  // punned type (the two differ in C/C++ language linkage — UB). Mirrors
  // mbSetJsDialogCallback. The lambda calls `fn` as exactly its declared C type.
  v->impl->BindJsFunction(
      name, [fn, userdata](int argc, const char** argv, const int* argtypes,
                           int* out_type) -> const char* {
        return fn(userdata, argc, argv, argtypes, out_type);
      });
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
  EngineScope engine_scope;
  if (v && v->impl)
    v->impl->SendText(utf8_text);
}

void mbSendKey(mbView* v, const char* key_name) {
  EngineScope engine_scope;
  if (v && v->impl)
    v->impl->SendKey(key_name);
}

void mbSendKeyEx(mbView* v, const char* key, int modifiers) {
  EngineScope engine_scope;
  if (v && v->impl)
    v->impl->SendKeyEx(key, modifiers);
}

void mbSendKeyUp(mbView* v, int windows_key_code) {
  EngineScope engine_scope;
  if (v && v->impl)
    v->impl->SendKeyUp(windows_key_code);
}

void mbSendIme(mbView* v, const char* composing, const char* committed) {
  EngineScope engine_scope;
  if (v && v->impl)
    v->impl->SendIme(composing, committed);
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
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
}

int mbGetCookie(mbView* v, const char* url, const char* name, char* out,
                int out_cap) {
  if (!v || !v->impl || !url || !name)
    return -1;
  std::string result;
  if (!v->impl->GetCookieValue(url, name, &result))
    return -1;  // cookie not present
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
}

int mbGetAllCookies(mbView* v, char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->GetAllCookies();
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
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

int mbSaveIndexedDB(const char* path) {
  // Process-wide: snapshot every in-memory IndexedDB database to `path`.
  return (path && mb::MbSaveIndexedDB(path)) ? 1 : 0;
}

int mbLoadIndexedDB(const char* path) {
  // Process-wide: restore IndexedDB databases from `path` (call before open()).
  return (path && mb::MbLoadIndexedDB(path)) ? 1 : 0;
}

int mbSaveOPFS(const char* path) {
  // Process-wide: snapshot the whole OPFS tree (all origins/buckets) to `path`.
  return (path && mb::MbSaveOPFS(path)) ? 1 : 0;
}

int mbLoadOPFS(const char* path) {
  // Process-wide: restore the OPFS tree from `path` (merges onto the live tree).
  return (path && mb::MbLoadOPFS(path)) ? 1 : 0;
}

int mbGetRequestLog(char* out, int out_cap) {
  // Process-wide: newline-separated subresource URLs the loader has fetched.
  std::string result = mb::MbGetRequestLog();
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
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

void mbBlockResourceType(const char* type, int blocked) {
  if (type)
    mb::MbSetResourceTypeBlocked(type, blocked != 0);
}

void mbSetRequestCallback(mbRequestCallback cb, void* userdata) {
  if (cb) {
    mb::MbSetRequestHook([cb, userdata](const std::string& url, const std::string&,
                                        const std::string&, const std::string&) {
      return cb(url.c_str(), userdata);  // URL-only basic callback
    });
  } else {
    mb::MbSetRequestHook({});
  }
}

void mbSetRequestCallbackEx(mbRequestCallbackEx cb, void* userdata) {
  if (cb) {
    mb::MbSetRequestHook(
        [cb, userdata](const std::string& url, const std::string& method,
                       const std::string& headers, const std::string& body) {
          return cb(url.c_str(), method.c_str(), headers.c_str(), body.data(),
                    static_cast<int>(body.size()), userdata);
        });
  } else {
    mb::MbSetRequestHook({});
  }
}

// The opaque mbResponse handle is a transient view of the loader's per-response state
// (URL + status + mutable body), valid only for the duration of one callback.
struct mbResponse {
  const std::string& url;
  int* status;  // mutable: mbResponseSetStatus rewrites the delivered HTTP status
  std::string* headers;  // mutable: mbResponseSetHeader injects/overrides header lines
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
        [](const std::string& url, int* status, std::string* headers,
           std::string* body) {
          mbResponse r{url, status, headers, body};
          if (g_response_cb)
            g_response_cb(&r, g_response_ud);
        });
  } else {
    mb::MbSetResponseHook({});
  }
}

namespace {
mbNotificationCallback g_notification_cb = nullptr;
void* g_notification_ud = nullptr;
}  // namespace

void mbOnNotificationShown(mbNotificationCallback cb, void* userdata) {
  g_notification_cb = cb;
  g_notification_ud = userdata;
  if (cb) {
    mb::MbSetNotificationHook(
        [](const std::string& title, const std::string& body,
           const std::string& tag, const std::string& icon) {
          if (g_notification_cb)
            g_notification_cb(g_notification_ud, title.c_str(), body.c_str(),
                              tag.c_str(), icon.c_str());
        });
  } else {
    mb::MbSetNotificationHook({});
  }
}

const char* mbResponseURL(mbResponse* r) {
  return r ? r->url.c_str() : "";
}

int mbResponseStatus(mbResponse* r) {
  return (r && r->status) ? *r->status : 0;
}

void mbResponseSetStatus(mbResponse* r, int status) {
  if (r && r->status && status > 0)
    *r->status = status;  // rewrite the HTTP status the page will see
}

const char* mbResponseHeaders(mbResponse* r) {
  return (r && r->headers) ? r->headers->c_str() : "";
}

// Inject or override a response header line (case-insensitive name). Any existing line with
// the same name is removed, then "Name: value\r\n" is appended. For subresource/fetch/XHR
// loads the page's fetch Response.headers / XHR getResponseHeader see it; setting
// "Content-Type" also changes the delivered MIME.
void mbResponseSetHeader(mbResponse* r, const char* name, const char* value) {
  if (!r || !r->headers || !name || !*name || !value)
    return;
  std::string lname(name);
  for (char& c : lname)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  // Rebuild the block without any existing line of the same name.
  std::string out;
  std::string line;
  std::string buf = *r->headers;
  buf.push_back('\n');  // flush sentinel
  for (char c : buf) {
    if (c == '\n') {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      std::string::size_type colon = line.find(':');
      std::string ln;
      if (colon != std::string::npos) {
        ln = line.substr(0, colon);
        for (char& cc : ln)
          cc = static_cast<char>(std::tolower(static_cast<unsigned char>(cc)));
      }
      if (!line.empty() && ln != lname)
        out += line + "\r\n";
      line.clear();
    } else {
      line.push_back(c);
    }
  }
  out += std::string(name) + ": " + value + "\r\n";
  *r->headers = std::move(out);
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

// Dynamic request mock: a callback decides per-URL whether to serve a COMPUTED response
// (no fetch). The opaque mbRequestMock is a transient view of the loader's pending response
// slots; mbRequestMockResponse fills it.
struct mbRequestMock {
  std::string* body;
  std::string* content_type;
  int* status;
};

void mbRequestMockResponse(mbRequestMock* m, const char* body, int len,
                           const char* content_type, int status) {
  if (!m)
    return;
  if (m->body)
    m->body->assign(body ? body : "", (body && len > 0) ? static_cast<size_t>(len) : 0);
  if (m->content_type)
    *m->content_type = content_type ? content_type : "text/html";
  if (m->status)
    *m->status = status > 0 ? status : 200;
}

namespace {
mbRequestMockCallback g_request_mock_cb = nullptr;
void* g_request_mock_ud = nullptr;
}  // namespace

void mbOnRequestMock(mbView* v, mbRequestMockCallback cb, void* userdata) {
  if (!v || !v->impl)
    return;
  if (cb)
    v->impl->SetRequestMockCallback(
        [cb, userdata](const std::string& url, std::string* body,
                          std::string* ct, int* status) -> bool {
          mbRequestMock m{body, ct, status};
          return cb(url.c_str(), &m, userdata) != 0;
        });
  else
    v->impl->SetRequestMockCallback({});
}

void mbSetRequestMockCallback(mbRequestMockCallback cb, void* userdata) {
  g_request_mock_cb = cb;
  g_request_mock_ud = userdata;
  if (cb) {
    mb::MbSetRequestMockHook([](const std::string& url, std::string* body,
                                std::string* ct, int* status) -> bool {
      if (!g_request_mock_cb)
        return false;
      mbRequestMock m{body, ct, status};
      return g_request_mock_cb(url.c_str(), &m, g_request_mock_ud) != 0;
    });
  } else {
    mb::MbSetRequestMockHook({});
  }
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
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
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
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
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

void mbSetGeolocation(double latitude, double longitude, double accuracy) {
  mb::MbSetGeolocation(latitude, longitude, accuracy);
}

void mbSetOnline(int online) {
  mb::MbSetOnline(online != 0);
}

void mbClearGeolocation(void) {
  mb::MbClearGeolocation();
}

void mbSetClipboard(const char* utf8_text) {
  mb::MbSetClipboardText(utf8_text ? utf8_text : "");
}

int mbGetClipboard(char* out, int out_cap) {
  std::string text = mb::MbGetClipboardText();
  CopyToBuffer(text, out, out_cap);
  return static_cast<int>(std::min<size_t>(text.size(), INT_MAX));
}

int mbSaveLocalStorage(mbView* v, char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->SaveLocalStorage();
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
}

void mbLoadLocalStorage(mbView* v, const char* json) {
  if (v && v->impl)
    v->impl->LoadLocalStorage(json);
}

int mbDrainConsole(mbView* v, char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->DrainConsole();
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
}

void mbOnConsoleMessage(mbView* v, mbConsoleCallback cb, void* userdata) {
  if (!v || !v->impl)
    return;
  if (cb)
    v->impl->SetConsoleCallback(
        [v, cb, userdata](const std::string& level, const std::string& message,
                          const std::string& /*source*/, int /*line*/,
                          const std::string& /*stack*/) {
          cb(v, userdata, level.c_str(), message.c_str());
        });
  else
    v->impl->SetConsoleCallback({});
}

void mbOnConsoleMessageEx(mbView* v, mbConsoleCallbackEx cb, void* userdata) {
  if (!v || !v->impl)
    return;
  if (cb)
    v->impl->SetConsoleCallback(
        [v, cb, userdata](const std::string& level, const std::string& message,
                          const std::string& source, int line,
                          const std::string& stack) {
          cb(v, userdata, level.c_str(), message.c_str(), source.c_str(), line,
             stack.c_str());
        });
  else
    v->impl->SetConsoleCallback({});
}

int mbGetURL(mbView* v, char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->GetURL();
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
}

int mbGetTitle(mbView* v, char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->GetTitle();
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
}

int mbGetText(mbView* v, char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->GetText();
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
}

int mbGetHTML(mbView* v, char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->GetHTML();
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
}

int mbGetAXTree(mbView* v, char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->GetAXTree();
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
}

int mbFindText(mbView* v, const char* text, int match_case) {
  if (!v || !v->impl)
    return 0;
  bool active = false;
  return v->impl->FindText(text, match_case != 0, /*forward=*/true, &active);
}

int mbFindNext(mbView* v, int forward) {
  if (!v || !v->impl)
    return 0;
  return v->impl->FindNext(forward != 0) ? 1 : 0;
}

int mbGetFindActiveRect(mbView* v, int* x, int* y, int* w, int* h) {
  if (!v || !v->impl)
    return 0;
  return v->impl->GetFindActiveRect(x, y, w, h) ? 1 : 0;
}

void mbStopFind(mbView* v) {
  if (v && v->impl)
    v->impl->StopFind();
}

int mbGetTextForSelector(mbView* v, const char* css_selector, char* out,
                         int out_cap) {
  if (!v || !v->impl || !css_selector)
    return -1;
  std::string result;
  if (!v->impl->GetTextForSelector(css_selector, &result))
    return -1;  // no element matched
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
}

int mbGetAllTextForSelector(mbView* v, const char* css_selector, char* out,
                            int out_cap) {
  if (!v || !v->impl || !css_selector)
    return -1;
  std::string result;
  if (!v->impl->GetAllTextForSelector(css_selector, &result))
    return -1;  // invalid selector
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
}

int mbSetHtmlForSelector(mbView* v, const char* css_selector, const char* html) {
  if (!v || !v->impl || !css_selector)
    return 0;
  return v->impl->SetHtmlForSelector(css_selector, html) ? 1 : 0;
}

int mbGetHtmlForSelector(mbView* v, const char* css_selector, char* out,
                         int out_cap) {
  if (!v || !v->impl || !css_selector)
    return -1;
  std::string result;
  if (!v->impl->GetHtmlForSelector(css_selector, &result))
    return -1;  // no element matched
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
}

int mbGetAllValueForSelector(mbView* v, const char* css_selector, char* out,
                             int out_cap) {
  if (!v || !v->impl || !css_selector)
    return -1;
  std::string result;
  if (!v->impl->GetAllValueForSelector(css_selector, &result))
    return -1;  // invalid selector
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
}

int mbGetAllAttributeForSelector(mbView* v, const char* css_selector,
                                 const char* attr, char* out, int out_cap) {
  if (!v || !v->impl || !css_selector || !attr)
    return -1;
  std::string result;
  if (!v->impl->GetAllAttributeForSelector(css_selector, attr, &result))
    return -1;  // invalid selector
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
}

int mbGetAttribute(mbView* v, const char* css_selector, const char* attr,
                   char* out, int out_cap) {
  if (!v || !v->impl || !css_selector || !attr)
    return -1;
  std::string result;
  if (!v->impl->GetAttribute(css_selector, attr, &result))
    return -1;  // no element matched, or attribute absent
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
}

int mbSetAttribute(mbView* v, const char* css_selector, const char* attr,
                   const char* value) {
  if (!v || !v->impl || !css_selector || !attr)
    return 0;
  return v->impl->SetAttribute(css_selector, attr, value) ? 1 : 0;
}

int mbGetValueForSelector(mbView* v, const char* css_selector, char* out,
                          int out_cap) {
  if (!v || !v->impl || !css_selector)
    return -1;
  std::string result;
  if (!v->impl->GetValueForSelector(css_selector, &result))
    return -1;  // no element matched, or no value property
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
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
  if (!v || !v->impl || !css_selector || !property)
    return -1;
  std::string result;
  if (!v->impl->GetComputedStyle(css_selector, property, &result))
    return -1;  // no element matched
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
}

void mbReload(mbView* v) {
  if (v && v->impl)
    v->impl->Reload();
}

void mbStopLoading(mbView* v) {
  if (v && v->impl)
    v->impl->StopLoading();
}

int mbGetHttpStatus(mbView* v) {
  return (v && v->impl) ? v->impl->GetHttpStatus() : 0;
}

int mbGetResponseHeaders(mbView* v, char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  const std::string& result = v->impl->GetResponseHeaders();
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
}

int mbGetLastError(mbView* v, char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  const std::string& result = v->impl->GetLastError();
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
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
  EngineScope engine_scope;
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->EvalIsolated(utf8_script);
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
}

int mbEvalJS(mbView* v, const char* utf8_script, char* out, int out_cap) {
  EngineScope engine_scope;
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->EvalToString(utf8_script);
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
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
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
}

int mbFillSelectorInFrame(mbView* v, int frame_index, const char* css_selector,
                          const char* utf8_text) {
  if (!v || !v->impl || !css_selector)
    return 0;
  return v->impl->FillSelectorInFrame(frame_index, css_selector, utf8_text) ? 1
                                                                            : 0;
}

int mbGetTextForSelectorInFrame(mbView* v, int frame_index,
                                const char* css_selector, char* out,
                                int out_cap) {
  if (!v || !v->impl)
    return -1;
  std::string result;
  if (!v->impl->GetTextForSelectorInFrame(frame_index, css_selector, &result))
    return -1;  // no element matched (or out-of-range / remote frame)
  CopyToBuffer(result, out, out_cap);
  return static_cast<int>(std::min<size_t>(result.size(), INT_MAX));
}

int mbEvalJSEx(mbView* v, const char* utf8_script, char* out_value,
               int value_cap, char* out_type, int type_cap) {
  EngineScope engine_scope;
  if (!v || !v->impl)
    return 0;
  std::string type;
  std::string value = v->impl->EvalWithType(utf8_script, &type);
  CopyToBuffer(value, out_value, value_cap);  // value is arbitrary page content
  CopyToBuffer(type, out_type, type_cap);      // type is ASCII, but stay uniform
  return static_cast<int>(std::min<size_t>(value.size(), INT_MAX));
}

int mbPaintToBitmap(mbView* v, void* out_bgra, int width, int height, int stride) {
  EngineScope engine_scope;
  if (!v || !v->impl || !out_bgra)
    return 0;
  return v->impl->PaintToBitmap(out_bgra, width, height, stride) ? 1 : 0;
}

int mbViewIsDirty(mbView* v) {
  if (!v || !v->impl)
    return 0;
  return v->impl->IsDirty() ? 1 : 0;
}

int mbRepaintToBitmap(mbView* v, void* out_bgra, int width, int height, int stride) {
  EngineScope engine_scope;
  if (!v || !v->impl || !out_bgra)
    return 0;
  // Fast INTERACTIVE paint (no one-shot lifecycle settle / task-queue drain) — for a
  // host that repaints continuously (a windowed browser blitting at ~60fps). Use
  // mbPaintToBitmap for a one-shot screenshot of freshly-loaded content.
  return v->impl->PaintToBitmap(out_bgra, width, height, stride, /*settle=*/false) ? 1
                                                                                   : 0;
}

int mbSavePng(mbView* v, const char* path, int width, int height) {
  EngineScope engine_scope;
  if (!v || !v->impl || !path)
    return 0;
  return v->impl->SavePng(path, width, height) ? 1 : 0;
}

int mbEncodePng(mbView* v, int width, int height, const unsigned char** out_data) {
  EngineScope engine_scope;
  if (!v || !v->impl || width <= 0 || height <= 0)
    return 0;
  if (!v->impl->EncodePng(width, height))
    return 0;
  const std::vector<uint8_t>& data = v->impl->EncodedData();
  if (out_data)
    *out_data = data.data();  // valid until the next mbEncodePng / mbDestroyView
  return static_cast<int>(std::min<size_t>(data.size(), INT_MAX));
}

int mbSavePdf(mbView* v, const char* path) {
  EngineScope engine_scope;
  if (!v || !v->impl || !path)
    return 0;
  return v->impl->SavePdf(path) ? 1 : 0;
}

void mbSetPrintBackground(mbView* v, int enabled) {
  if (v && v->impl)
    v->impl->SetPrintBackground(enabled != 0);
}

int mbSavePdfEx(mbView* v, const char* path, double width_pt, double height_pt,
                int landscape, double scale, double margin_pt) {
  EngineScope engine_scope;
  if (!v || !v->impl || !path)
    return 0;
  return v->impl->SavePdfEx(path, width_pt, height_pt, landscape != 0, scale,
                            margin_pt)
             ? 1
             : 0;
}

int mbSavePngRect(mbView* v, const char* path, int x, int y, int w, int h) {
  EngineScope engine_scope;
  if (!v || !v->impl || !path)
    return 0;
  return v->impl->SavePngRect(path, x, y, w, h) ? 1 : 0;
}

int mbSaveElementPng(mbView* v, const char* css_selector, const char* path) {
  EngineScope engine_scope;
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
  EngineScope engine_scope;
  if (!v || !v->impl || !out_bgra)
    return 0;
  return v->impl->PaintRectToBitmap(out_bgra, x, y, w, h, stride) ? 1 : 0;
}

}  // extern "C"
