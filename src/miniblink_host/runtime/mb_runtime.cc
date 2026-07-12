// mb_runtime.cc — process-wide Blink bring-up (production path).
//
// Unlike blink's ScopedUnittestsEnvironmentSetup (which replays the low-level steps
// Partitions::Initialize / InitializeWtf / ThreadState attach / *_names::Init by hand),
// we call the PUBLIC entry point blink::CreateMainThreadAndInitialize(), which performs
// all of those internally. Then CreateMainThreadIsolate() for V8/cppgc.

#include "miniblink_host/runtime/mb_runtime.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <vector>

#include "miniblink_host/platform/mb_platform.h"

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/debug/stack_trace.h"
#include "base/feature_list.h"
#include "base/i18n/icu_util.h"
#include "third_party/blink/renderer/platform/fonts/font_cache.h"
#include "third_party/blink/renderer/platform/wtf/text/atomic_string.h"
#include "base/run_loop.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_MAC)
#include <malloc/malloc.h>
#endif

#include "base/memory/memory_pressure_listener.h"
#include "base/task/single_thread_task_runner.h"
#include "v8/include/v8-isolate.h"

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/memory/discardable_memory_allocator.h"
#include "base/message_loop/message_pump.h"
#if BUILDFLAG(IS_MAC)
#include "base/message_loop/message_pump_apple.h"
#else
#include "base/message_loop/message_pump.h"
#include "base/message_loop/message_pump_type.h"
#endif
#include "base/message_loop/message_pump_type.h"
#include "base/memory/discardable_memory.h"
#include "base/path_service.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/threading/thread.h"
#include "mojo/core/embedder/scoped_ipc_support.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/base/resource/resource_scale_factor.h"
#include "gin/public/v8_snapshot_file_type.h"
#include "gin/v8_initializer.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/public/cpp/bindings/binder_map.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/scheduler/web_thread_scheduler.h"
#include "third_party/blink/public/platform/web_connection_type.h"
#include "third_party/blink/public/platform/web_network_state_notifier.h"
#include "third_party/blink/public/platform/web_runtime_features.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/blink.h"
#include "third_party/blink/public/web/web_security_policy.h"
#include "third_party/blink/renderer/platform/graphics/image_decoding_store.h"
#include "third_party/skia/include/core/SkGraphics.h"
#include "url/url_util.h"
#include "v8/include/v8-initialization.h"
#include "v8/include/v8-isolate.h"
#include "miniblink_host/loader/mb_url_loader.h"
#include "miniblink_host/runtime/mb_script_watchdog.h"

namespace mb {

namespace {
MbRuntime* g_runtime = nullptr;

// JS old-generation heap cap (mbSetJsHeapLimit): recorded before init, applied
// as a V8 flag ahead of blink::Initialize (isolate-creation parameter).
size_t g_pending_js_heap_limit_bytes = 0;

// Trivial in-process discardable memory: plain heap, never actually discarded.
// (Image/gradient/shader caches require an allocator instance to exist.)
class MbDiscardableMemory : public base::DiscardableMemory {
 public:
  explicit MbDiscardableMemory(size_t size) : data_(size) {}
  bool Lock() override { return true; }  // data always valid (never purged)
  void Unlock() override {}
  void* data() const override { return const_cast<uint8_t*>(data_.data()); }
  void DiscardForTesting() override {}
  base::trace_event::MemoryAllocatorDump* CreateMemoryAllocatorDump(
      const char*,
      base::trace_event::ProcessMemoryDump*) const override {
    return nullptr;
  }

 private:
  std::vector<uint8_t> data_;
};

class MbDiscardableAllocator : public base::DiscardableMemoryAllocator {
 public:
  std::unique_ptr<base::DiscardableMemory> AllocateLockedDiscardableMemory(
      size_t size) override {
    return std::make_unique<MbDiscardableMemory>(size);
  }
  size_t GetBytesAllocated() const override { return 0; }
  void ReleaseFreeMemory() override {}
};
}  // namespace

namespace {
// Set when Shutdown() begins; polled (off-thread) by the worker rendezvous
// loops in mb_url_loader.cc / mb_worker_script.cc as their release-of-last-
// resort once the host stops pumping the engine loop.
std::atomic<bool> g_shutdown_started{false};
}  // namespace

MbRuntime* MbRuntime::Get() {
  return g_runtime;
}

// static
bool MbRuntime::Initialize() {
  g_shutdown_started.store(false, std::memory_order_release);
  if (g_runtime)
    return true;
  g_runtime = new MbRuntime();
  if (!g_runtime->ok_) {
    delete g_runtime;
    g_runtime = nullptr;
    return false;
  }
  return true;
}

// static
void MbRuntime::Shutdown() {
  // The Blink/V8/mojo global init done in the ctor (mojo::core::Init, the V8 context
  // snapshot, blink::Platform::InitializeBlink, blink::Initialize + the main-thread
  // V8 isolate and its cppgc heap, the thread pool) is ONE-TIME per process and has
  // no clean teardown — a 2nd blink::Initialize builds a 2nd isolate that crashes
  // ThreadState::AttachToIsolate, and mojo::core::Init double-inits. So the engine
  // stays resident for the process lifetime and a later mbInitialize reuses it
  // (Initialize() is idempotent); process-global memory is reclaimed by the OS at exit.
  //
  // Previously this did `delete g_runtime; g_runtime = nullptr;`, which (a) left the
  // installed base::DiscardableMemoryAllocator / blink Platform pointers dangling at
  // freed memory and (b) made any re-init re-run the one-time globals and crash. Both
  // are gone now: Shutdown is a safe no-op. (Matches the single-process Chromium model,
  // where process-global singletons are intentionally leaked rather than destroyed.)
  //
  // The one observable effect: mark shutdown as started, releasing any worker
  // thread parked on an engine-sequence rendezvous (the host has just promised
  // it will not pump the engine loop again).
  g_shutdown_started.store(true, std::memory_order_release);
}

// static
bool MbRuntime::ShutdownStarted() {
  return g_shutdown_started.load(std::memory_order_acquire);
}

MbRuntime::MbRuntime() {
#define MB_STEP(msg)                                       \
  do {                                                     \
    if (std::getenv("MB_VERBOSE"))                         \
      std::fprintf(stderr, "[mb_runtime] " msg "\n");      \
  } while (0)
  // 1. base bootstrap.
  MB_STEP("1 AtExitManager");
  at_exit_ = std::make_unique<base::AtExitManager>();
  // Print a symbolized backtrace on a fatal signal (SIGSEGV/SIGABRT). Opt-in via
  // MB_STACK_DUMP so default runs are unaffected; invaluable for diagnosing native
  // crashes (e.g. media-pipeline bring-up) when an attached debugger isn't available.
  if (std::getenv("MB_STACK_DUMP"))
    base::debug::EnableInProcessStackDumping();
  MB_STEP("2 CommandLine::Init");
  base::CommandLine::Init(0, nullptr);
  // Pin base::DIR_ASSETS to this module's own directory before anything reads
  // it (ICU data, the resource paks, and the V8 snapshots all resolve through
  // it). Unbundled this matches the existing DIR_MODULE fallback; but inside a
  // macOS .app bundle DIR_ASSETS resolves to FrameworkBundlePath()/Resources —
  // for an embedder with no framework bundle that is "<App>.app/Resources",
  // which a sealed bundle cannot even contain. The module directory IS the
  // vendoring contract (the runtime files ship next to the library).
  MB_STEP("2a PathService::Override(DIR_ASSETS)");
  {
    base::FilePath module_path;
    if (base::PathService::Get(base::FILE_MODULE, &module_path))
      base::PathService::Override(base::DIR_ASSETS, module_path.DirName());
  }
  MB_STEP("3 InitializeICU");
  base::i18n::InitializeICU();  // needs icudtl.dat next to the module (vendor it)
  MB_STEP("4 FeatureList");
  if (!base::FeatureList::GetInstance())
    base::FeatureList::SetInstance(std::make_unique<base::FeatureList>());

  // Custom URL schemes (item 48), part 1: the url:: registries. Must precede
  // any URL parsing of these schemes (and any registry lock), so this sits at
  // the very front of bring-up. Standard = host/path/origin semantics; secure
  // so app:// documents are secure contexts (they carry host-vetted content).
  MB_STEP("4x custom url schemes");
  for (const std::string& scheme : MbCustomSchemes()) {
    url::AddStandardScheme(scheme, url::SCHEME_WITH_HOST);
    url::AddSecureScheme(scheme);
  }

  // JS heap cap (item 45): an isolate-creation parameter, so it must be a V8
  // flag set before blink::Initialize creates the main-thread isolate.
  if (g_pending_js_heap_limit_bytes) {
    const size_t mb_limit = (g_pending_js_heap_limit_bytes + 1024 * 1024 - 1) /
                            (1024 * 1024);
    const std::string flag =
        "--max-old-space-size=" + std::to_string(mb_limit);
    v8::V8::SetFlagsFromString(flag.c_str(), flag.size());
  }

  // Discardable memory: image/gradient/shader caches allocate it; without an
  // allocator instance, rendering DCHECKs. Simple in-process allocator (never discards).
  MB_STEP("4a DiscardableMemoryAllocator");
  discardable_ = std::make_unique<MbDiscardableAllocator>();
  base::DiscardableMemoryAllocator::SetInstance(discardable_.get());

  // Resource bundle (UA stylesheet + blink resources) — needed before any document
  // is styled, or glyphs/UA defaults are missing. Pak vendored next to the binary.
  MB_STEP("4b ResourceBundle");
  if (!ui::ResourceBundle::HasSharedInstance()) {
    base::FilePath dir;
    if (base::PathService::Get(base::DIR_ASSETS, &dir) ||
        base::PathService::Get(base::DIR_MODULE, &dir)) {
      ui::ResourceBundle::InitSharedInstanceWithPakPath(
          dir.AppendASCII("blink_resources.pak"));
      // Extra packs: media-controls CSS (needed to lay out <video>/<audio>).
      ui::ResourceBundle::GetSharedInstance().AddDataPackFromPath(
          dir.AppendASCII("media_controls_resources_100_percent.pak"),
          ui::k100Percent);
    }
  }

  // 2. mojo + an IO thread for handle watching (mojo::SimpleWatcher / DataPipeBytesConsumer
  //    need it; required for streaming response bodies through data pipes -> fetch()).
  MB_STEP("5 mojo::core::Init + IPC support");
  mojo::core::Init();
  io_thread_ = std::make_unique<base::Thread>("mb-io");
  io_thread_->StartWithOptions(
      base::Thread::Options(base::MessagePumpType::IO, 0));
  ipc_support_ = std::make_unique<mojo::core::ScopedIPCSupport>(
      io_thread_->task_runner(),
      mojo::core::ScopedIPCSupport::ShutdownPolicy::FAST);

  // Blink's main-thread init posts to the thread pool; it must exist + be started.
  MB_STEP("6 ThreadPool start");
  if (!base::ThreadPoolInstance::Get())
    base::ThreadPoolInstance::CreateAndStartWithDefaultParams("miniblink");

  // Blink core init reads the V8 *context* snapshot (V8ContextSnapshotImpl); the
  // file type must be loaded first or gin DCHECKs. File = v8_context_snapshot.*.bin.
  MB_STEP("7 LoadV8Snapshot(kWithAdditionalContext)");
  gin::V8Initializer::LoadV8Snapshot(
      gin::V8SnapshotFileType::kWithAdditionalContext);

  // Low-level Blink init: Partitions/WTF/heap/ThreadState (cppgc main thread).
  MB_STEP("8 Platform::InitializeBlink");
  blink::Platform::InitializeBlink(/*stack_start_marker=*/std::nullopt);


  // The REAL main-thread scheduler (owns the message pump). The simple
  // CreateMainThreadAndInitialize path yields a degenerate scheduler whose
  // CreateAgentGroupScheduler() returns null -> WebViewImpl ctor DCHECKs.
  //
  // The pump must be MessagePumpNSRunLoop, NOT MessagePump::Create(UI): on the
  // main thread Create(UI) yields MessagePumpNSApplication, whose nested runs
  // drain the HOST's NSApp event queue (nextEventMatchingMask + sendEvent).
  // Every mbUpdate/PumpOnce then dispatched the embedder's pending input
  // events from inside the engine call — a host wheel/mouse event arriving
  // re-entrantly like that was dropped by the embedder's re-entrancy guard
  // (scroll stalls). The NSRunLoop pump services Chromium tasks through
  // CFRunLoop sources only and never touches the application event queue;
  // input stays where it belongs, in the host's own event loop. Its
  // signalable quit source also ends the old "nested pump only quits
  // reliably inside a running NSApp" constraint.
  MB_STEP("9 CreateMainThreadScheduler");
#if BUILDFLAG(IS_MAC)
  main_thread_scheduler_ = blink::scheduler::WebThreadScheduler::
      CreateMainThreadScheduler(std::make_unique<base::MessagePumpNSRunLoop>());
#else
  // Windows: the default UI pump services Chromium tasks via its message
  // window; it does not drain the host's queue re-entrantly the way the mac
  // NSApplication pump did, so the NSRunLoop workaround is mac-only.
  main_thread_scheduler_ =
      blink::scheduler::WebThreadScheduler::CreateMainThreadScheduler(
          base::MessagePump::Create(base::MessagePumpType::UI));
#endif

  // Full init with our platform + real scheduler. Empty BinderMap (no browser ifaces).
  // CRITICAL: blink::Initialize() CREATES the main-thread isolate internally
  // (V8Initializer::InitializeMainThread, consuming ThreadState's cppgc heap and
  // Enter()-ing the isolate). Do NOT also call blink::CreateMainThreadIsolate() — a 2nd
  // call builds a 2nd isolate with a fresh heap and crashes ThreadState::AttachToIsolate
  // with a cpp_heap mismatch. Grab the now-current isolate instead.
  MB_STEP("10 blink::Initialize (creates isolate)");
  platform_ = std::make_unique<MbPlatform>();
  mojo::BinderMap binder_map;
  blink::Initialize(platform_.get(), &binder_map, main_thread_scheduler_.get());

#if BUILDFLAG(IS_WIN)
  // AFTER blink::Initialize: AtomicString needs the static string tables.
  // Chrome's browser flips these renderer prefs; with no browser, text
  // rasterizes bi-level and FontCache::SystemFontPlatformData dereferences a
  // null system family (crash on any system-ui use). Grayscale AA (no LCD
  // subpixel) keeps screenshots deterministic across monitors.
  blink::FontCache::SetAntialiasedTextEnabled(true);
  blink::FontCache::SetLCDTextEnabled(false);
  blink::FontCache::SetMenuFontMetrics(blink::AtomicString("Segoe UI"), 12);
  blink::FontCache::SetSmallCaptionFontMetrics(blink::AtomicString("Segoe UI"),
                                               12);
  blink::FontCache::SetStatusFontMetrics(blink::AtomicString("Segoe UI"), 12);
#endif
  isolate_ = v8::Isolate::GetCurrent();

  // Disable casting / second-screen APIs we have no backend for. A page's JS call
  // (e.g. YouTube's Cast button -> PresentationRequest.reconnect()) creates a
  // ScriptPromiseResolver, then sends a mojo request to a PresentationService that is
  // never bound, so the promise stays pending forever; GC of the still-undetached
  // resolver trips a DCHECK ("ScriptPromiseResolverBase was not properly detached")
  // and FATAL-crashes. Disabling the feature removes navigator.presentation so the
  // page never creates the leaked resolver. RemotePlayback (the <video> Cast path)
  // has the same hazard.
  blink::WebRuntimeFeatures::EnableFeatureFromString("Presentation", false);
  blink::WebRuntimeFeatures::EnableFeatureFromString("RemotePlayback", false);

  // Custom URL schemes, part 2: blink's own registries (available only after
  // blink::Initialize). Fetch/XHR to the scheme works; the loader serves it
  // from the interception stack (MbFindMock).
  for (const std::string& scheme : MbCustomSchemes()) {
    blink::WebSecurityPolicy::RegisterURLSchemeAsSupportingFetchAPI(
        blink::WebString::FromUtf8(scheme));
  }

  // Initialize network state, or pages reading navigator.onLine/connection DCHECK
  // (network_state_notifier requires connection_initialized).
  blink::WebNetworkStateNotifier::SetOnLine(true);
  blink::WebNetworkStateNotifier::SetWebConnection(
      blink::kWebConnectionTypeEthernet, /*max_bandwidth_mbps=*/100.0);

  // Runaway-script guard: a TaskObserver + monitor thread that terminates a single
  // main-thread task whose JS overruns the configured timeout (disabled by default;
  // opt in via mbSetScriptTimeout). Without it a `while(true){}` page hangs the
  // single-process embedder forever.
  if (isolate_) {
    script_watchdog_ = std::make_unique<MbScriptWatchdog>();
    script_watchdog_->Start(isolate_);
  }

  ok_ = (isolate_ != nullptr);
  MB_STEP("12 done");
#undef MB_STEP
}

MbRuntime::~MbRuntime() {
  // Stop the watchdog (joins its thread + removes the TaskObserver) before the
  // scheduler/message loop is torn down.
  if (script_watchdog_)
    script_watchdog_->Stop();
  // The MainThreadSchedulerImpl dtor DCHECKs Shutdown() was called.
  if (main_thread_scheduler_)
    main_thread_scheduler_->Shutdown();
  // TODO(mb): fuller teardown (isolate dispose, blink shutdown) when it matters.
}

void MbRuntime::PumpOnce(double budget_seconds) {
  if (budget_seconds <= 0) {
    base::RunLoop().RunUntilIdle();
    return;
  }
  // Bounded slice: quit at idle (fast path) OR at the deadline, whichever
  // comes first. The delayed quit is a plain task; delayed-task due times are
  // checked between immediate tasks, so a busy queue still honors it. A quit
  // on an already-finished RunLoop is a no-op (weak-bound), so the leftover
  // delayed task from the idle path is harmless.
  base::RunLoop loop;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, loop.QuitClosure(), base::Seconds(budget_seconds));
  loop.RunUntilIdle();
}

void MbRuntime::PurgeMemory() {
  base::MemoryPressureListener::NotifyMemoryPressure(
      base::MEMORY_PRESSURE_LEVEL_CRITICAL);
  if (isolate_) {
    isolate_->MemoryPressureNotification(v8::MemoryPressureLevel::kCritical);
    isolate_->LowMemoryNotification();
  }
  base::RunLoop().RunUntilIdle();  // run the pressure listeners now
}

void MbRuntime::LogMemoryUsage() {
  if (isolate_) {
    v8::HeapStatistics hs;
    isolate_->GetHeapStatistics(&hs);
    std::fprintf(stderr,
                 "[mb] v8 heap: used %zu KB / total %zu KB, external %zu KB\n",
                 hs.used_heap_size() / 1024, hs.total_heap_size() / 1024,
                 hs.external_memory() / 1024);
  }
#if BUILDFLAG(IS_MAC)
  malloc_statistics_t st{};
  malloc_zone_statistics(nullptr, &st);
  std::fprintf(stderr, "[mb] malloc: in use %zu KB, allocated %zu KB\n",
               st.size_in_use / 1024, st.size_allocated / 1024);
#endif
}

void MbRuntime::SetScriptTimeoutMs(int ms) {
  if (script_watchdog_)
    script_watchdog_->SetTimeoutMs(ms);
}

void MbRuntime::SetImageCacheSize(size_t bytes) {
  // 0 restores blink's default (kDefaultMaxTotalSizeOfHeapEntries, 32 MB —
  // there is no getter to capture, so the default is mirrored here).
  blink::ImageDecodingStore::Instance().SetCacheLimitInBytes(
      bytes ? bytes : 32 * 1024 * 1024);
}

void MbRuntime::SetFontCacheSize(size_t bytes) {
  // 0 restores skia's default (SK_DEFAULT_FONT_CACHE_LIMIT, 2 MB).
  SkGraphics::SetFontCacheLimit(bytes ? bytes : 2 * 1024 * 1024);
}

// static
void MbRuntime::SetJsHeapLimit(size_t bytes) {
  if (g_runtime) {
    std::fprintf(stderr,
                 "[mb] mbSetJsHeapLimit ignored: the JS heap limit is an "
                 "isolate-creation parameter — call it BEFORE mbInitialize\n");
    return;
  }
  g_pending_js_heap_limit_bytes = bytes;
}

// static
scoped_refptr<base::SingleThreadTaskRunner> MbRuntime::ServiceTaskRunner() {
  if (g_runtime && g_runtime->io_thread_)
    return g_runtime->io_thread_->task_runner();
  return nullptr;
}

}  // namespace mb
