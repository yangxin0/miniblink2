// miniblink2.cc — extern "C" implementation of the miniblink2 public API (mb* ABI). Pure forwarding to the C++ host;
// no Blink types cross this boundary.

#include "miniblink2/automation.h"
#include "miniblink2/webview.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/logging.h"
#include "miniblink_host/frame/mb_frame_broker.h"
#include "miniblink_host/frame/mb_indexeddb.h"
#include "miniblink_host/frame/mb_local_frame_host.h"
#include "miniblink_host/frame/mb_opfs.h"
#include "build/build_config.h"
#include "miniblink_host/devtools/mb_devtools_server.h"
#include "miniblink_host/frame/mb_notification_service.h"
#include "miniblink_host/loader/mb_url_loader.h"
#include "miniblink_host/platform/mb_platform.h"
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
// Mac-only: the Windows component build resolves the Rust allocator inside the
// base DLL and these internal symbols are not importable (lld: undefined).
#if BUILDFLAG(IS_MAC)
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
#endif

// Opaque handle: wraps the C++ view.
struct mbView {
  std::unique_ptr<mb::MbWebView> impl;
};

namespace {
// Reverse map from an engine view (mb::MbWebView*, the token the loader carries as
// host_ctx) to its opaque C handle (mbView*). Lets process-wide loader callbacks —
// e.g. the response hook — report WHICH view a load belongs to. Maintained at
// mbCreateView*/mbDestroyView; main-thread only, like the views themselves.
std::map<const mb::MbWebView*, mbView*>& ViewHandleRegistry() {
  static auto* m = new std::map<const mb::MbWebView*, mbView*>();
  return *m;
}
void RegisterViewHandle(mbView* v) {
  if (v && v->impl)
    ViewHandleRegistry()[v->impl.get()] = v;
}
void UnregisterViewHandle(mbView* v) {
  if (v && v->impl)
    ViewHandleRegistry().erase(v->impl.get());
}
mbView* ViewHandleForCtx(const void* host_ctx) {
  if (!host_ctx)
    return nullptr;
  auto& reg = ViewHandleRegistry();
  auto it = reg.find(static_cast<const mb::MbWebView*>(host_ctx));
  return it == reg.end() ? nullptr : it->second;
}
}  // namespace

// Creation-time view config (mbCreateViewConfig / item 36): a plain collector.
// Tri-state ints (-1 = unset -> engine default); strings carry a *_set flag so
// an explicit "" (e.g. clear the UA override) is distinguishable from unset.
struct mbViewConfig {
  mbSession* session = nullptr;
  int compositing = -1;
  int transparent = -1;
  float device_scale = 0;  // 0 = unset
  int enable_js = -1;
  int load_images = -1;
  int dark_mode = -1;
  std::string ua;
  bool ua_set = false;
  std::string locale;
  bool locale_set = false;
  std::string ff_standard, ff_fixed, ff_serif, ff_sans;
  bool ff_set = false;
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

const char* mbVersion(void) {
  return MB_VERSION;  // derived from MB_VERSION_MAJOR/MINOR/PATCH — one source
}

int mbApiVersion(void) {
  return MB_API_LEVEL;  // == mbApiLevel(); the original name for the additive level
}

int mbApiLevel(void) {
  return MB_API_LEVEL;
}

int mbAbiEpoch(void) {
  return MB_ABI_EPOCH;
}

int mbCheckCompat(int abi_epoch, int api_level) {
  // Same breaking-ABI epoch AND an engine additive level at least the host's.
  return (abi_epoch == MB_ABI_EPOCH && api_level >= 0 &&
          MB_API_LEVEL >= api_level)
             ? 1
             : 0;
}

int mbHasFeature(const char* feature) {
  if (!feature)
    return 0;
  static const struct {
    const char* name;
    int present;
  } kCaps[] = {
      {"webgpu",
#if defined(MINIBLINK_ENABLE_WEBGPU)
       1
#else
       0
#endif
      },
      {"wasm",
#if defined(MINIBLINK_ENABLE_WASM)
       1
#else
       0
#endif
      },
      {"webrtc",
#if defined(MINIBLINK_ENABLE_WEBRTC)
       1
#else
       0
#endif
      },
  };
  for (const auto& c : kCaps) {
    if (std::strcmp(feature, c.name) == 0)
      return c.present;
  }
  return 0;
}

const char* mbChromiumVersion(void) {
  // Injected by BUILD.gn from //chrome/VERSION (tracks the donor tree).
#if defined(MB_CHROMIUM_VERSION)
  return MB_CHROMIUM_VERSION;
#else
  return "unknown";
#endif
}

namespace {
mbLogCallback g_log_cb = nullptr;
void* g_log_ud = nullptr;

// base logging hook: severity is logging::LOGGING_* (0 info .. 3 fatal); `str`
// holds the whole formatted line, `message_start` where the payload begins.
bool MbLogHandler(int severity, const char* /*file*/, int /*line*/,
                  size_t message_start, const std::string& str) {
  mbLogCallback cb = g_log_cb;
  if (!cb)
    return false;  // sink cleared between check and dispatch: default handling
  std::string msg = str.substr(message_start);
  while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
    msg.pop_back();
  int level = severity < 0 ? 0 : (severity > 3 ? 3 : severity);
  cb(g_log_ud, level, msg.c_str());
  return true;  // consumed: keep it out of stderr
}
}  // namespace

void mbOnLogMessage(mbLogCallback cb, void* userdata) {
  g_log_ud = userdata;
  g_log_cb = cb;
  logging::SetLogMessageHandler(cb ? &MbLogHandler : nullptr);
}

int mbInitialize(void) {
  return mb::MbRuntime::Initialize() ? 1 : 0;
}

void mbRegisterCustomScheme(const char* scheme) {
  if (scheme && *scheme)
    mb::MbRegisterCustomScheme(scheme);
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

void mbSetImageCacheSize(size_t bytes) {
  if (auto* rt = mb::MbRuntime::Get())
    rt->SetImageCacheSize(bytes);
}

void mbSetFontCacheSize(size_t bytes) {
  if (auto* rt = mb::MbRuntime::Get())
    rt->SetFontCacheSize(bytes);
}

void mbSetJsHeapLimit(size_t bytes) {
  mb::MbRuntime::SetJsHeapLimit(bytes);
}

// ---- Pixel-format utilities (pure buffer math; no engine state) --------------
namespace {
inline int MbEffectiveStride(int width, int stride) {
  return stride > 0 ? stride : width * 4;
}
}  // namespace

void mbConvertToStraightAlpha(void* pixels, int width, int height, int stride) {
  if (!pixels || width <= 0 || height <= 0)
    return;
  const int row_bytes = MbEffectiveStride(width, stride);
  for (int y = 0; y < height; ++y) {
    uint8_t* row = static_cast<uint8_t*>(pixels) + static_cast<size_t>(y) * row_bytes;
    for (int x = 0; x < width; ++x) {
      uint8_t* px = row + x * 4;
      const unsigned a = px[3];
      if (a == 0 || a == 255)
        continue;
      // straight = premul * 255 / a, rounded; premul channels never exceed a.
      px[0] = static_cast<uint8_t>((px[0] * 255u + a / 2) / a);
      px[1] = static_cast<uint8_t>((px[1] * 255u + a / 2) / a);
      px[2] = static_cast<uint8_t>((px[2] * 255u + a / 2) / a);
    }
  }
}

void mbConvertToPremultipliedAlpha(void* pixels, int width, int height,
                                   int stride) {
  if (!pixels || width <= 0 || height <= 0)
    return;
  const int row_bytes = MbEffectiveStride(width, stride);
  for (int y = 0; y < height; ++y) {
    uint8_t* row = static_cast<uint8_t*>(pixels) + static_cast<size_t>(y) * row_bytes;
    for (int x = 0; x < width; ++x) {
      uint8_t* px = row + x * 4;
      const unsigned a = px[3];
      if (a == 255)
        continue;
      px[0] = static_cast<uint8_t>((px[0] * a + 127) / 255);
      px[1] = static_cast<uint8_t>((px[1] * a + 127) / 255);
      px[2] = static_cast<uint8_t>((px[2] * a + 127) / 255);
    }
  }
}

void mbSwapRedBlueChannels(void* pixels, int width, int height, int stride) {
  if (!pixels || width <= 0 || height <= 0)
    return;
  const int row_bytes = MbEffectiveStride(width, stride);
  for (int y = 0; y < height; ++y) {
    uint8_t* row = static_cast<uint8_t*>(pixels) + static_cast<size_t>(y) * row_bytes;
    for (int x = 0; x < width; ++x) {
      uint8_t* px = row + x * 4;
      const uint8_t b = px[0];
      px[0] = px[2];
      px[2] = b;
    }
  }
}

void mbUpdateAt(double frame_time_seconds) {
  mb::MbSetHostFrameTime(frame_time_seconds);
  mbUpdate();
}

void mbViewSetFrameTime(mbView* v, double frame_time_seconds) {
  if (v && v->impl)
    v->impl->SetFrameTime(frame_time_seconds);
}

int mbDevToolsAttach(mbView* v, mbDevToolsMessageCallback cb, void* userdata) {
  EngineScope engine_scope;
  if (!v || !v->impl || !cb)
    return 0;
  return v->impl->AttachDevTools(
             [v, cb, userdata](const std::string& msg) {
               cb(v, userdata, msg.c_str(), static_cast<int>(msg.size()));
             })
             ? 1
             : 0;
}

void mbDevToolsSend(mbView* v, const char* json, int len) {
  EngineScope engine_scope;
  if (v && v->impl)
    v->impl->SendDevTools(json, len);
}

void mbDevToolsDetach(mbView* v) {
  EngineScope engine_scope;
  if (v && v->impl)
    v->impl->DetachDevTools();
}

int mbDevToolsStartServer(int port) {
  EngineScope engine_scope;
  return mb::MbDevToolsServerStart(port) ? 1 : 0;
}

void mbDevToolsStopServer(void) {
  EngineScope engine_scope;
  mb::MbDevToolsServerStop();
}

int mbDevToolsServerPort(void) {
  return mb::MbDevToolsServerPort();
}

void mbOnSelectPopup(mbView* v, mbSelectPopupCallback cb, void* userdata) {
  EngineScope engine_scope;
  if (!v || !v->impl)
    return;
  if (!cb) {
    v->impl->SetSelectPopupCallback({});
    return;
  }
  v->impl->SetSelectPopupCallback([v, cb,
                                   userdata](const mb::MbSelectPopupData& d) {
    std::vector<mbSelectPopupItem> items;
    items.reserve(d.items.size());
    for (const auto& it : d.items)
      items.push_back(mbSelectPopupItem{it.label.c_str(), it.type,
                                        it.enabled ? 1 : 0,
                                        it.checked ? 1 : 0});
    mbSelectPopupInfo info{};
    info.struct_size = static_cast<int>(sizeof(mbSelectPopupInfo));
    info.x = d.x;
    info.y = d.y;
    info.width = d.width;
    info.height = d.height;
    info.font_size = d.font_size;
    info.selected_index = d.selected_index;
    info.right_aligned = d.right_aligned ? 1 : 0;
    info.allow_multiple = d.allow_multiple ? 1 : 0;
    info.item_count = static_cast<int>(items.size());
    info.items = items.empty() ? nullptr : items.data();
    cb(v, userdata, &info);
  });
}

int mbSelectPopupCommit(mbView* v, const int* indices, int count) {
  EngineScope engine_scope;
  if (!v || !v->impl)
    return 0;
  return v->impl->CommitSelectPopup(indices, count);
}

void mbSelectPopupCancel(mbView* v) {
  EngineScope engine_scope;
  if (v && v->impl)
    v->impl->CancelSelectPopup();
}

void mbOnDevToolsPaused(mbView* v, mbDevToolsPausedCallback cb,
                        void* userdata) {
  EngineScope engine_scope;
  if (!v || !v->impl)
    return;
  if (cb) {
    v->impl->SetDevToolsPausedCallback([v, cb, userdata](bool paused) {
      cb(v, userdata, paused ? 1 : 0);
    });
  } else {
    v->impl->SetDevToolsPausedCallback({});
  }
}

void mbSetFontFallbackCallback(mbFontFallbackCallback cb, void* userdata) {
  if (!cb) {
    mb::MbSetFontFallbackHook({});
    return;
  }
  mb::MbSetFontFallbackHook(
      [cb, userdata](uint32_t codepoint, int weight, bool italic) {
        char family[256] = {0};
        if (!cb(userdata, codepoint, weight, italic ? 1 : 0, family,
                static_cast<int>(sizeof(family))))
          return std::string();
        family[sizeof(family) - 1] = '\0';
        return std::string(family);
      });
}

void mbSetFontFamilies(mbView* v, const char* standard, const char* fixed,
                       const char* serif, const char* sans_serif) {
  EngineScope engine_scope;
  if (v && v->impl)
    v->impl->SetFontFamilies(standard, fixed, serif, sans_serif);
}

int mbAddFontData(const void* data, int len, char* out_family, int family_cap) {
  if (!data || len <= 0)
    return 0;
  std::string family;
  if (!mb::MbAddFontData(data, static_cast<size_t>(len), &family))
    return 0;
  CopyToBuffer(family, out_family, family_cap);
  return 1;
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

int mbWaitForNetworkIdleEx(mbView* v, int idle_ms, int timeout_ms) {
  EngineScope engine_scope;
  if (!v || !v->impl)
    return 0;
  return v->impl->WaitForNetworkIdleEx(idle_ms, timeout_ms) ? 1 : 0;
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

namespace {
// The impl frees its C handle through this deleter when the impl is destroyed
// (every view/config has released it) — see MbSession::set_host_handle_deleter.
void DeleteSessionHandle(mbSession* h) {
  delete h;
}
}  // namespace

mbSession* mbCreateSessionEx(const char* name, const char* persist_path,
                             mbSessionCreateStatus* out_status) {
  EngineScope engine_scope;
  if (!mb::MbRuntime::Get()) {
    if (out_status)
      *out_status = MB_SESSION_ERROR;
    return nullptr;
  }
  const bool persistent = persist_path && *persist_path;
  // Preserve the historical friendly fallback for ephemeral handles, but do
  // not hide a missing persistent name: it must be rejected rather than alias
  // the literal profile named "unnamed".
  const std::string session_name =
      name && *name ? std::string(name)
                    : (persistent ? std::string() : std::string("unnamed"));
  mb::MbSessionCreateResult create_result = mb::MbSessionCreateResult::kError;
  auto s = std::make_unique<mbSession>();
  s->impl = mb::MbSession::Create(session_name,
                                  persistent ? persist_path : "",
                                  &create_result);
  if (!s->impl) {
    if (out_status) {
      *out_status = create_result == mb::MbSessionCreateResult::kInvalidName
                        ? MB_SESSION_INVALID_NAME
                        : MB_SESSION_ERROR;
    }
    return nullptr;
  }
  if (out_status)
    *out_status = MB_SESSION_OK;
  s->impl->set_host_handle(s.get());
  s->impl->set_host_handle_deleter(&DeleteSessionHandle);
  return s.release();
}

mbSession* mbCreateSession(const char* name, const char* persist_path) {
  EngineScope engine_scope;
  auto s = std::make_unique<mbSession>();
  s->impl = mb::MbSession::CreateLegacy(name && *name ? name : "unnamed",
                                        persist_path ? persist_path : "");
  if (!s->impl)
    return nullptr;
  s->impl->set_host_handle(s.get());
  s->impl->set_host_handle_deleter(&DeleteSessionHandle);
  return s.release();
}

void mbDestroySession(mbSession* s) {
  if (!s || !s->impl)
    return;
  // Detach the owner handle but DON'T free it here: a view may still be bound to
  // this session (and mbViewGetSession can still hand the handle back), and a
  // view-config may still reference it. The impl is reference-counted; it — and
  // the C handle, via the registered deleter — is destroyed only when the last
  // view and config release it. Freeing `s` now, with the impl still live, is the
  // use-after-free this avoids.
  s->impl->Detach();
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

int mbSessionFlushEx(mbSession* s) {
  EngineScope engine_scope;
  if (!s || !s->impl)
    return 0;
  return s->impl->FlushToDisk() ? 1 : 0;
}

int mbSessionGetName(mbSession* s, char* out, int out_cap) {
  if (!s || !s->impl)
    return 0;
  const std::string& name = s->impl->name();
  CopyToBuffer(name, out, out_cap);
  return static_cast<int>(std::min<size_t>(name.size(), INT_MAX));
}

int mbSessionIsPersistent(mbSession* s) {
  return (s && s->impl && s->impl->persistent()) ? 1 : 0;
}

int mbSessionGetPersistPath(mbSession* s, char* out, int out_cap) {
  if (!s || !s->impl)
    return 0;
  const std::string& dir = s->impl->persist_dir();
  CopyToBuffer(dir, out, out_cap);
  return static_cast<int>(std::min<size_t>(dir.size(), INT_MAX));
}

mbView* mbCreateViewInSession(int width, int height, mbSession* session) {
  EngineScope engine_scope;
  if (!mb::MbRuntime::Get())
    return nullptr;  // must mbInitialize() first
  auto view = std::make_unique<mbView>();
  view->impl = mb::MbWebView::Create(
      width, height, /*opener=*/nullptr, /*compositing_override=*/-1,
      session && session->impl ? session->impl : nullptr);
  if (!view->impl)
    return nullptr;
  RegisterViewHandle(view.get());
  return view.release();
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
  RegisterViewHandle(view.get());
  return view.release();
}

mbViewConfig* mbCreateViewConfig(void) {
  return new mbViewConfig();
}

void mbDestroyViewConfig(mbViewConfig* c) {
  if (!c)
    return;
  // Release the ref this config held on its session's impl (see
  // mbViewConfigSetSession) so a session captured only by a config can tear down
  // once the config is gone.
  if (c->session && c->session->impl)
    c->session->impl->Release();
  delete c;
}

void mbViewConfigSetSession(mbViewConfig* c, mbSession* session) {
  if (!c || c->session == session)
    return;
  // Hold a ref on the impl while the config references it: a config keeps the
  // session (and its C handle) alive until the config is consumed/destroyed, so a
  // mbDestroySession between config setup and mbCreateViewWithConfig can't free
  // the session out from under c->session.
  if (session && session->impl)
    session->impl->AddRef();
  if (c->session && c->session->impl)
    c->session->impl->Release();
  c->session = session;
}

void mbViewConfigSetCompositing(mbViewConfig* c, int on) {
  if (c)
    c->compositing = on ? 1 : 0;
}

void mbViewConfigSetTransparentBackground(mbViewConfig* c, int transparent) {
  if (c)
    c->transparent = transparent ? 1 : 0;
}

void mbViewConfigSetDeviceScaleFactor(mbViewConfig* c, float scale) {
  if (c)
    c->device_scale = scale;
}

void mbViewConfigSetEnableJavascript(mbViewConfig* c, int enabled) {
  if (c)
    c->enable_js = enabled ? 1 : 0;
}

void mbViewConfigSetLoadImages(mbViewConfig* c, int enabled) {
  if (c)
    c->load_images = enabled ? 1 : 0;
}

void mbViewConfigSetDarkMode(mbViewConfig* c, int dark) {
  if (c)
    c->dark_mode = dark ? 1 : 0;
}

void mbViewConfigSetUserAgent(mbViewConfig* c, const char* utf8_ua) {
  if (!c)
    return;
  c->ua = utf8_ua ? utf8_ua : "";
  c->ua_set = true;
}

void mbViewConfigSetLocale(mbViewConfig* c, const char* utf8_languages) {
  if (!c)
    return;
  c->locale = utf8_languages ? utf8_languages : "";
  c->locale_set = true;
}

void mbViewConfigSetFontFamilies(mbViewConfig* c, const char* standard,
                                 const char* fixed, const char* serif,
                                 const char* sans_serif) {
  if (!c)
    return;
  c->ff_standard = standard ? standard : "";
  c->ff_fixed = fixed ? fixed : "";
  c->ff_serif = serif ? serif : "";
  c->ff_sans = sans_serif ? sans_serif : "";
  c->ff_set = true;
}

mbView* mbCreateViewWithConfig(int width, int height, const mbViewConfig* c) {
  EngineScope engine_scope;
  if (!mb::MbRuntime::Get())
    return nullptr;  // must mbInitialize() first
  auto view = std::make_unique<mbView>();
  view->impl = mb::MbWebView::Create(width, height, /*opener=*/nullptr,
                                     c ? c->compositing : -1,
                                     c && c->session && c->session->impl
                                         ? c->session->impl
                                         : nullptr);
  if (!view->impl)
    return nullptr;
  if (c) {
    // Apply the collected choices before any document exists — the whole point
    // of the config path (no "call before first load" ordering to get wrong).
    if (c->transparent >= 0)
      view->impl->SetTransparentBackground(c->transparent != 0);
    if (c->device_scale > 0)
      view->impl->SetDeviceScaleFactor(c->device_scale);
    if (c->enable_js >= 0)
      view->impl->SetEnableJavascript(c->enable_js != 0);
    if (c->load_images >= 0)
      view->impl->SetLoadImages(c->load_images != 0);
    if (c->dark_mode >= 0)
      view->impl->SetDarkMode(c->dark_mode != 0);
    if (c->ua_set)
      view->impl->SetUserAgent(c->ua.c_str());
    if (c->locale_set)
      view->impl->SetLocale(c->locale.c_str());
    if (c->ff_set)
      view->impl->SetFontFamilies(c->ff_standard.c_str(), c->ff_fixed.c_str(),
                                  c->ff_serif.c_str(), c->ff_sans.c_str());
  }
  RegisterViewHandle(view.get());
  return view.release();
}

void mbDestroyView(mbView* v) {
  EngineScope engine_scope;
  UnregisterViewHandle(v);
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

void* mbViewGetIOSurface(mbView* v) {
  if (!v || !v->impl)
    return nullptr;
  return v->impl->CompositorIOSurface();
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

void mbLoadHTMLEx(mbView* v, const char* utf8_html, const char* base_url,
                  int add_to_history) {
  EngineScope engine_scope;
  if (v && v->impl)
    v->impl->LoadHTMLEx(utf8_html, base_url, add_to_history != 0);
}

void mbLoadURL(mbView* v, const char* utf8_url) {
  EngineScope engine_scope;
  if (v && v->impl)
    v->impl->LoadURL(utf8_url);
}

mbNavigationId mbNavigate(mbView* v, const char* utf8_url) {
  EngineScope engine_scope;
  if (!v || !v->impl)
    return 0;
  return v->impl->Navigate(utf8_url);
}

mbNavigationId mbNavigateEx(mbView* v, const char* utf8_url,
                            const mbNavigationOptions* opts) {
  EngineScope engine_scope;
  if (!v || !v->impl)
    return 0;
  std::string method, body, content_type;
  // Read each field only when the caller's declared prefix reaches it. This
  // makes future appended fields safe and lets a deliberately shorter prefix
  // retain the fields it does provide instead of silently becoming a GET.
  auto has_field = [&](size_t offset, size_t size) {
    return opts && opts->struct_size >= 0 &&
           static_cast<size_t>(opts->struct_size) >= offset + size;
  };
  if (has_field(offsetof(mbNavigationOptions, method),
                sizeof(opts->method))) {
    if (opts->method)
      method = opts->method;
  }
  if (has_field(offsetof(mbNavigationOptions, body_len),
                sizeof(opts->body_len))) {
    if (opts->body && opts->body_len)
      body.assign(static_cast<const char*>(opts->body), opts->body_len);
  }
  if (has_field(offsetof(mbNavigationOptions, content_type),
                sizeof(opts->content_type))) {
    if (opts->content_type)
      content_type = opts->content_type;
  }
  return v->impl->NavigateEx(utf8_url ? utf8_url : "", method, body, content_type);
}

int mbCancelNavigation(mbView* v, mbNavigationId id) {
  EngineScope engine_scope;
  if (!v || !v->impl)
    return 0;
  return v->impl->CancelNavigation(id) ? 1 : 0;
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

void mbOnNavigationEvent(mbView* v, mbNavigationEventCallback cb,
                         void* userdata) {
  if (!v || !v->impl)
    return;
  if (cb) {
    v->impl->SetNavigationEventCallback(
        [v, cb, userdata](const mb::MbWebView::NavigationEventData& data) {
          mbNavigationEvent event = {};
          event.struct_size = sizeof(event);
          event.navigation_id = data.navigation_id;
          event.phase = static_cast<int>(data.phase);
          event.outcome = static_cast<int>(data.outcome);
          event.requested_url = data.requested_url.c_str();
          event.url = data.url.c_str();
          event.http_status = data.http_status;
          event.error_domain = data.error_domain.c_str();
          event.error_code = data.error_code;
          event.description = data.description.c_str();
          cb(v, userdata, &event);
        });
  } else {
    v->impl->SetNavigationEventCallback({});
  }
}

void mbOnNavigationStarted(mbView* v, mbNavigationStartedCallback cb,
                           void* userdata) {
  if (!v || !v->impl)
    return;
  if (cb)
    v->impl->SetNavigationStartedCallback(
        [v, cb, userdata](const std::string& url) { cb(v, userdata, url.c_str()); });
  else
    v->impl->SetNavigationStartedCallback({});
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

void mbOnFailLoadingEx(mbView* v, mbFailLoadingCallbackEx cb, void* userdata) {
  if (!v || !v->impl)
    return;
  if (cb)
    v->impl->SetFailLoadingCallbackEx(
        [v, cb, userdata](const std::string& url, const std::string& domain,
                          int code, const std::string& description) {
          cb(v, userdata, url.c_str(), domain.c_str(), code,
             description.c_str());
        });
  else
    v->impl->SetFailLoadingCallbackEx({});
}

void mbOnDOMContentLoaded(mbView* v, mbDOMContentLoadedCallback cb, void* userdata) {
  if (!v || !v->impl)
    return;
  if (cb)
    v->impl->SetDOMContentLoadedCallback([v, cb, userdata]() { cb(v, userdata); });
  else
    v->impl->SetDOMContentLoadedCallback({});
}

void mbOnWindowObjectReady(mbView* v, mbWindowObjectReadyCallback cb,
                           void* userdata) {
  if (!v || !v->impl)
    return;
  if (cb)
    v->impl->SetWindowObjectReadyCallback([v, cb, userdata]() { cb(v, userdata); });
  else
    v->impl->SetWindowObjectReadyCallback({});
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

void mbOnDownloadStream(mbView* v, mbDownloadBeginCallback begin_cb,
                        mbDownloadDataCallback data_cb,
                        mbDownloadFinishCallback finish_cb, void* userdata) {
  if (!v || !v->impl)
    return;
  if (!begin_cb && !data_cb && !finish_cb) {
    v->impl->SetDownloadStreamCallbacks({}, {}, {});
    return;
  }
  v->impl->SetDownloadStreamCallbacks(
      [v, begin_cb, userdata](unsigned id, const std::string& url,
                              const std::string& mime,
                              const std::string& filename,
                              long long expected) {
        if (begin_cb)
          begin_cb(v, userdata, id, url.c_str(), mime.c_str(),
                   filename.c_str(), expected);
      },
      [v, data_cb, userdata](unsigned id, const char* data, size_t len,
                             long long received, long long expected) {
        if (data_cb)
          data_cb(v, userdata, id, data,
                  static_cast<int>(std::min<size_t>(len, INT_MAX)), received,
                  expected);
      },
      [v, finish_cb, userdata](unsigned id, bool success) {
        if (finish_cb)
          finish_cb(v, userdata, id, success ? 1 : 0);
      });
}

unsigned int mbDownloadURLStream(mbView* v, const char* url) {
  EngineScope engine_scope;
  if (!v || !v->impl)
    return 0;
  return v->impl->DownloadURLStream(url);
}

void mbCancelDownload(mbView* v, unsigned int id) {
  if (v && v->impl)
    v->impl->CancelDownload(id);
}

void mbOnCursorChanged(mbView* v, mbCursorChangedCallback cb, void* userdata) {
  if (!v || !v->impl)
    return;
  if (cb)
    v->impl->SetCursorChangedCallback(
        [v, cb, userdata](int cursor) { cb(v, userdata, cursor); });
  else
    v->impl->SetCursorChangedCallback({});
}

void mbOnTooltipChanged(mbView* v, mbTooltipChangedCallback cb, void* userdata) {
  if (!v || !v->impl)
    return;
  if (cb)
    v->impl->SetTooltipChangedCallback(
        [v, cb, userdata](const std::string& text) {
          cb(v, userdata, text.c_str());
        });
  else
    v->impl->SetTooltipChangedCallback({});
}

void mbOnRequestClose(mbView* v, mbRequestCloseCallback cb, void* userdata) {
  if (!v || !v->impl)
    return;
  if (cb)
    v->impl->SetRequestCloseCallback([v, cb, userdata]() { cb(v, userdata); });
  else
    v->impl->SetRequestCloseCallback({});
}

int mbHasInputFocus(mbView* v) {
  return (v && v->impl && v->impl->HasInputFocus()) ? 1 : 0;
}

void mbOnHistoryChanged(mbView* v, mbHistoryChangedCallback cb, void* userdata) {
  if (!v || !v->impl)
    return;
  if (cb)
    v->impl->SetHistoryChangedCallback([v, cb, userdata](bool back, bool fwd) {
      cb(v, userdata, back ? 1 : 0, fwd ? 1 : 0);
    });
  else
    v->impl->SetHistoryChangedCallback({});
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

void mbOnCreateChildView(mbView* v, mbCreateChildViewCallback cb,
                         void* userdata) {
  if (!v || !v->impl)
    return;
  if (!cb) {
    v->impl->SetCreateChildViewCallback({});
    return;
  }
  v->impl->SetCreateChildViewCallback(
      [v, cb, userdata](const std::string& url, const std::string& name,
                        bool is_popup, int x, int y, int w,
                        int h) -> mb::MbWebView* {
        // window.open features may leave the size unspecified (0): default to
        // the parent's viewport so the child renders sensibly until the host
        // resizes it.
        int cw = w, ch = h;
        if (cw <= 0 || ch <= 0) {
          int pw = 0, ph = 0;
          v->impl->GetViewSize(&pw, &ph);
          if (cw <= 0)
            cw = pw > 0 ? pw : 800;
          if (ch <= 0)
            ch = ph > 0 ? ph : 600;
        }
        auto child = std::make_unique<mbView>();
        child->impl = mb::MbWebView::Create(cw, ch, v->impl.get());
        if (!child->impl)
          return nullptr;
        // Create() structurally binds opener children to the parent's profile
        // before Blink creates their Page (cookies, storage, workers).
        // Register the reverse handle so loader callbacks (e.g. the response hook)
        // can report THIS child view — without it an adopted popup's responses
        // would surface a null view. mbDestroyView unregisters it.
        RegisterViewHandle(child.get());
        mbView* handle = child.release();
        if (!cb(v, userdata, handle, url.c_str(), name.c_str(),
                is_popup ? 1 : 0, x, y, w, h)) {
          // Declined: window.open sees null. Blink never adopted this view,
          // so sever its opener wiring first (a dangling openee corrupts the
          // opener page's script state on Close). And we are INSIDE the
          // opener's JS (window.open is synchronous), so closing the WebView
          // here would re-enter blink mid-stack — defer the teardown to the
          // next engine-off-the-stack moment instead.
          mbDefer(
              [](void* ud) {
                mbView* declined = static_cast<mbView*>(ud);
                // Sever the opener link off the opener's window.open stack,
                // then tear the never-adopted view down.
                declined->impl->DisownOpener();
                mbDestroyView(declined);
              },
              handle);
          return nullptr;
        }
        return handle->impl.get();  // adopted: host owns `handle`
      });
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

// Typed-event ABI compatibility: a typed event's struct_size is the caller's
// sizeof, and the contract (webview.h) is that fields may only be APPENDED — the
// existing prefix never moves. So the guard must accept any struct at least as
// large as the STABLE PREFIX we actually read, NOT the engine's current full
// sizeof. Using sizeof(current) would reject every older client the moment we
// append a field (their smaller struct_size < the grown sizeof), silently
// killing all input from them. offsetof(last-read field)+sizeof pins the check
// to the prefix these functions read today; a future appended field must guard
// its own read with a wider offsetof check, never by raising this minimum.
constexpr int kMbMouseEventMinSize =
    static_cast<int>(offsetof(mbMouseEvent, modifiers) + sizeof(int));
constexpr int kMbWheelEventMinSize =
    static_cast<int>(offsetof(mbWheelEvent, modifiers) + sizeof(int));
constexpr int kMbKeyEventMinSize =
    static_cast<int>(offsetof(mbKeyEvent, is_system_key) + sizeof(int));

void mbSendMouseEvent(mbView* v, const mbMouseEvent* e) {
  EngineScope engine_scope;
  if (!v || !v->impl || !e || e->struct_size < kMbMouseEventMinSize)
    return;
  v->impl->SendMouseEvent(e->type, e->x, e->y, e->button, e->click_count,
                          e->modifiers);
}

int mbSendWheelEvent(mbView* v, const mbWheelEvent* e) {
  EngineScope engine_scope;
  if (!v || !v->impl || !e || e->struct_size < kMbWheelEventMinSize ||
      e->phase != 0)
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

void mbSetEnableJavascript(mbView* v, int enabled) {
  if (v && v->impl)
    v->impl->SetEnableJavascript(enabled != 0);
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

void mbSendKeyEvent(mbView* v, const mbKeyEvent* e) {
  EngineScope engine_scope;
  if (!v || !v->impl || !e || e->struct_size < kMbKeyEventMinSize)
    return;
  v->impl->SendKeyEvent(e->type, e->modifiers, e->windows_key_code,
                        e->native_key_code, e->text, e->unmodified_text,
                        e->is_keypad != 0, e->is_auto_repeat != 0,
                        e->is_system_key != 0);
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
  // Legacy no-view API: the empty key selects the implicit default session.
  return (path && mb::MbSaveCookies(path)) ? 1 : 0;
}

int mbLoadCookies(const char* path) {
  // Legacy no-view API: merge into the implicit default session only.
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
  mb::MbSetRequestMutateHook({});  // one slot across the three request hooks
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
  mb::MbSetRequestMutateHook({});  // one slot across the three request hooks
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

// The opaque mbRequest handle: a transient view of one request plus its
// mutation record, valid only for the duration of the callback.
struct mbRequest {
  const std::string& url;
  const std::string& method;
  const std::string& headers;
  const std::string& body;
  mb::MbRequestMutation* mutation;
};

const char* mbRequestURL(mbRequest* r) {
  return r ? r->url.c_str() : "";
}

const char* mbRequestMethod(mbRequest* r) {
  return r ? r->method.c_str() : "";
}

const char* mbRequestHeaders(mbRequest* r) {
  return r ? r->headers.c_str() : "";
}

const char* mbRequestBody(mbRequest* r, int* out_len) {
  if (out_len)
    *out_len = r ? static_cast<int>(r->body.size()) : 0;
  return r ? r->body.data() : "";
}

void mbRequestSetUrl(mbRequest* r, const char* url) {
  if (r && r->mutation && url)
    r->mutation->set_url = url;
}

void mbRequestSetHeader(mbRequest* r, const char* name, const char* value) {
  if (!r || !r->mutation || !name || !*name)
    return;
  if (!r->mutation->add_headers.empty())
    r->mutation->add_headers += "\n";
  r->mutation->add_headers += std::string(name) + ": " + (value ? value : "");
}

void mbRequestBlock(mbRequest* r) {
  if (r && r->mutation)
    r->mutation->block = true;
}

void mbRequestPinPublicKey(mbRequest* r, const char* pins) {
  if (r && r->mutation)
    r->mutation->pin_pubkey = pins ? pins : "";
}

void mbSetRequestHook(mbRequestHookCallback cb, void* userdata) {
  mb::MbSetRequestHook({});  // one slot across the three request hooks
  if (cb) {
    mb::MbSetRequestMutateHook(
        [cb, userdata](const std::string& url, const std::string& method,
                       const std::string& headers, const std::string& body,
                       mb::MbRequestMutation* mutation) {
          mbRequest handle{url, method, headers, body, mutation};
          cb(&handle, userdata);
        });
  } else {
    mb::MbSetRequestMutateHook({});
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
// One hook slot, two possible C entry points: the original (response, userdata) and the
// level-2 Ex (response, view, userdata). Setting either clears the other.
mbResponseCallback g_response_cb = nullptr;
mbResponseCallbackEx g_response_cb_ex = nullptr;
void* g_response_ud = nullptr;

void UpdateResponseHook() {
  if (g_response_cb || g_response_cb_ex) {
    mb::MbSetResponseHook(
        [](const void* host_ctx, const std::string& url, int* status,
           std::string* headers, std::string* body) {
          mbResponse r{url, status, headers, body};
          if (g_response_cb_ex)
            g_response_cb_ex(&r, ViewHandleForCtx(host_ctx), g_response_ud);
          else if (g_response_cb)
            g_response_cb(&r, g_response_ud);
        });
  } else {
    mb::MbSetResponseHook({});
  }
}
}  // namespace

void mbSetResponseCallback(mbResponseCallback cb, void* userdata) {
  g_response_cb = cb;
  g_response_cb_ex = nullptr;  // one slot: plain replaces Ex
  g_response_ud = userdata;
  UpdateResponseHook();
}

void mbSetResponseCallbackEx(mbResponseCallbackEx cb, void* userdata) {
  g_response_cb_ex = cb;
  g_response_cb = nullptr;  // one slot: Ex replaces plain
  g_response_ud = userdata;
  UpdateResponseHook();
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

void mbRegisterImageSource(const char* id, const void* bgra, int width,
                           int height, int stride) {
  EngineScope engine_scope;  // the update broadcast runs page JS
  if (!id || !bgra)
    return;
  if (mb::MbSetImageSource(id, bgra, width, height, stride))
    mb::MbBroadcastImageSourceUpdate(id);
}

void mbUnregisterImageSource(const char* id) {
  if (id)
    mb::MbRemoveImageSource(id);
}

int mbRegisterImageSourceBuffer(const char* id, const void* bgra, int width,
                                int height, int stride,
                                mbImageSourceReleaseCallback release,
                                void* userdata) {
  EngineScope engine_scope;  // the update broadcast runs page JS
  if (!id || !bgra)
    return 0;
  if (!mb::MbSetImageSourceBuffer(id, bgra, width, height, stride, release,
                                  userdata))
    return 0;  // refused: the engine did NOT take the buffer
  mb::MbBroadcastImageSourceUpdate(id);
  return 1;
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

void mbSetRequestHeaderForHost(const char* host_filter, const char* name,
                               const char* value) {
  if (host_filter && name)
    mb::MbAddRequestHeaderForHost(host_filter, name, value ? value : "");
}

void mbSetRequestHeaderForOrigin(const char* origin, const char* name,
                                 const char* value) {
  if (origin && name)
    mb::MbAddRequestHeaderForOrigin(origin, name, value ? value : "");
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

void mbSetClipboardHandler(mbClipboardReadCallback read_cb,
                           mbClipboardWriteCallback write_cb, void* userdata) {
  std::function<std::string()> read;
  std::function<void(const std::string&)> write;
  if (read_cb) {
    read = [read_cb, userdata]() -> std::string {
      char buf[4096];
      int n = read_cb(userdata, buf, static_cast<int>(sizeof(buf)));
      if (n <= 0)
        return std::string();
      if (n < static_cast<int>(sizeof(buf)))
        return std::string(buf, static_cast<size_t>(n));
      // Truncated: retry with a buffer sized to the reported full length.
      std::vector<char> big(static_cast<size_t>(n) + 1);
      int m = read_cb(userdata, big.data(), n + 1);
      if (m <= 0)
        return std::string();
      return std::string(big.data(), static_cast<size_t>(std::min(m, n)));
    };
  }
  if (write_cb) {
    write = [write_cb, userdata](const std::string& text) {
      write_cb(userdata, text.c_str());
    };
  }
  mb::MbSetClipboardHandler(std::move(read), std::move(write));
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
                          int /*column*/, const std::string& /*category*/,
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
                          const std::string& source, int line, int /*column*/,
                          const std::string& /*category*/,
                          const std::string& stack) {
          cb(v, userdata, level.c_str(), message.c_str(), source.c_str(), line,
             stack.c_str());
        });
  else
    v->impl->SetConsoleCallback({});
}

void mbOnConsoleMessage2(mbView* v, mbConsoleCallback2 cb, void* userdata) {
  if (!v || !v->impl)
    return;
  if (cb)
    v->impl->SetConsoleCallback(
        [v, cb, userdata](const std::string& level, const std::string& message,
                          const std::string& source, int line, int column,
                          const std::string& category,
                          const std::string& stack) {
          mbConsoleMessageInfo info;
          info.struct_size = static_cast<int>(sizeof(mbConsoleMessageInfo));
          info.level = level.c_str();
          info.message = message.c_str();
          info.source = source.c_str();
          info.line = line;
          info.column = column;
          info.category = category.c_str();
          info.stack = stack.c_str();
          cb(v, userdata, &info);
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

int mbGoToOffset(mbView* v, int offset) {
  EngineScope engine_scope;
  return (v && v->impl && v->impl->GoToOffset(offset)) ? 1 : 0;
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

void mbOnFrameLoadEvent(mbView* v, mbFrameLoadCallback cb, void* userdata) {
  if (!v || !v->impl)
    return;
  if (cb)
    v->impl->SetFrameLoadCallback(
        [v, cb, userdata](uint64_t frame_id, bool is_main_frame, int phase,
                          const std::string& url) {
          cb(v, userdata, frame_id, is_main_frame ? 1 : 0, phase, url.c_str());
        });
  else
    v->impl->SetFrameLoadCallback({});
}

int mbGetFrameIds(mbView* v, uint64_t* out, int cap) {
  if (!v || !v->impl)
    return 0;
  std::vector<uint64_t> ids = v->impl->GetFrameIds();
  if (out) {
    const int n = std::min<int>(cap, static_cast<int>(ids.size()));
    for (int i = 0; i < n; ++i)
      out[i] = ids[i];
  }
  return static_cast<int>(std::min<size_t>(ids.size(), INT_MAX));
}

int mbEvalJSInFrameById(mbView* v, uint64_t frame_id, const char* utf8_script,
                        char* out, int out_cap) {
  if (!v || !v->impl)
    return 0;
  std::string result = v->impl->EvalInFrameById(frame_id, utf8_script);
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

void mbViewSetDirty(mbView* v) {
  if (v && v->impl)
    v->impl->SetDirty();
}

int mbViewGetDirtyRect(mbView* v, int* x, int* y, int* w, int* h) {
  if (!v || !v->impl)
    return 0;
  v->impl->GetLastPaintDamage(x, y, w, h);
  return 1;
}

void mbSetForceRepaint(mbView* v, int enabled) {
  if (v && v->impl)
    v->impl->SetForceRepaint(enabled != 0);
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
