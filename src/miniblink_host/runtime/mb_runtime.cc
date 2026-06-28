// mb_runtime.cc — process-wide Blink bring-up (production path).
//
// Unlike blink's ScopedUnittestsEnvironmentSetup (which replays the low-level steps
// Partitions::Initialize / InitializeWtf / ThreadState attach / *_names::Init by hand),
// we call the PUBLIC entry point blink::CreateMainThreadAndInitialize(), which performs
// all of those internally. Then CreateMainThreadIsolate() for V8/cppgc.

#include "miniblink_host/runtime/mb_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <vector>

#include "miniblink_host/platform/mb_platform.h"

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/i18n/icu_util.h"
#include "base/run_loop.h"
#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/memory/discardable_memory_allocator.h"
#include "base/message_loop/message_pump.h"
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
#include "third_party/blink/public/web/blink.h"
#include "v8/include/v8-isolate.h"
#include "miniblink_host/runtime/mb_script_watchdog.h"

namespace mb {

namespace {
MbRuntime* g_runtime = nullptr;

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

MbRuntime* MbRuntime::Get() {
  return g_runtime;
}

// static
bool MbRuntime::Initialize() {
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
  MB_STEP("2 CommandLine::Init");
  base::CommandLine::Init(0, nullptr);
  MB_STEP("3 InitializeICU");
  base::i18n::InitializeICU();  // needs icudtl.dat next to the module (vendor it)
  MB_STEP("4 FeatureList");
  if (!base::FeatureList::GetInstance())
    base::FeatureList::SetInstance(std::make_unique<base::FeatureList>());

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
  MB_STEP("9 CreateMainThreadScheduler");
  main_thread_scheduler_ = blink::scheduler::WebThreadScheduler::
      CreateMainThreadScheduler(
          base::MessagePump::Create(base::MessagePumpType::UI));

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
  isolate_ = v8::Isolate::GetCurrent();

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

void MbRuntime::PumpOnce() {
  base::RunLoop().RunUntilIdle();
}

void MbRuntime::SetScriptTimeoutMs(int ms) {
  if (script_watchdog_)
    script_watchdog_->SetTimeoutMs(ms);
}

// static
scoped_refptr<base::SingleThreadTaskRunner> MbRuntime::ServiceTaskRunner() {
  if (g_runtime && g_runtime->io_thread_)
    return g_runtime->io_thread_->task_runner();
  return nullptr;
}

}  // namespace mb
