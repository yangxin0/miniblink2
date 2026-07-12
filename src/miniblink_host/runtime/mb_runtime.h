// mb_runtime — process-wide Blink bring-up for miniblink2.
//
// Mirrors the init sequence in blink's ScopedUnittestsEnvironmentSetup
// (vendor/reference/testing_platform_support.{h,cc}) but as a real, persistent
// runtime rather than a test fixture. This is what mbInitialize()/mbShutdown()
// drive under the C ABI.
//
// Init order (single-threaded "simple environment" path):
//   1. base::FeatureList + AtExitManager + base::test/real task environment
//   2. discardable memory allocator
//   3. construct MbPlatform, set as Platform::Current
//   4. mojo::BinderMap (EMPTY for P1 static render — no browser-side interfaces)
//   5. blink::CreateMainThreadAndInitialize(platform, &binder_map)
//   6. blink::CreateMainThreadIsolate()
// Teardown reverses this.

#ifndef MINIBLINK_HOST_RUNTIME_MB_RUNTIME_H_
#define MINIBLINK_HOST_RUNTIME_MB_RUNTIME_H_

#include <memory>

#include "base/memory/scoped_refptr.h"
#include "base/task/single_thread_task_runner.h"

namespace base {
class AtExitManager;
class DiscardableMemoryAllocator;
class Thread;
}  // namespace base
namespace mojo {
namespace core {
class ScopedIPCSupport;
}
}  // namespace mojo
namespace v8 {
class Isolate;
}  // namespace v8
namespace blink {
namespace scheduler {
class WebThreadScheduler;
}
}  // namespace blink

namespace mb {

class MbPlatform;
class MbScriptWatchdog;

// Owns the global engine state. Single instance per process (mbInitialize).
class MbRuntime {
 public:
  static MbRuntime* Get();        // null until Initialize()
  static bool Initialize();       // returns false if already initialized / failed
  static void Shutdown();
  // True once Shutdown() has begun (cleared by a later Initialize()). After this
  // the host promises no further engine-loop pumping, so a worker parked on an
  // engine-sequence rendezvous must abandon instead of polling forever. Readable
  // from any thread.
  static bool ShutdownStarted();

  // Drain the main-thread task runner once (loading, parsing, lifecycle steps).
  // Run ready main-thread work. budget_seconds > 0 bounds the slice: the
  // drain stops at the deadline even if the queue is still busy (remaining
  // work runs on the next pump). 0 = drain to idle (the historic behavior).
  void PumpOnce(double budget_seconds = 0);
  // Release as much memory as possible: critical memory-pressure broadcast
  // (blink caches, decoded images, font caches listen) + a V8 low-memory
  // notification (full GC), then a drain so listeners run now.
  void PurgeMemory();
  // Log a coarse memory summary (V8 heap + malloc footprint) via fprintf,
  // for diagnosing what a purge did.
  void LogMemoryUsage();

  MbPlatform* platform() { return platform_.get(); }

  // Max wall-clock for a single main-thread task before its (runaway synchronous)
  // JS is terminated, so a `while(true){}` page can't hang the process. <= 0
  // disables (the default). Process-global. See mb_script_watchdog.h.
  void SetScriptTimeoutMs(int ms);

  // ---- Memory budget knobs (IMPROVEMENT.md item 45) -------------------------
  // Cap the decoded-image cache (blink ImageDecodingStore); 0 restores the
  // engine default (32 MB). Callable any time after Initialize.
  void SetImageCacheSize(size_t bytes);
  // Cap skia's glyph/font cache; 0 restores the engine default (2 MB).
  void SetFontCacheSize(size_t bytes);
  // Cap each V8 isolate's old-generation heap. MUST be called BEFORE
  // Initialize (it is an isolate-creation parameter, applied as a V8 flag);
  // after init it is refused with a logged warning. 0 = engine default.
  static void SetJsHeapLimit(size_t bytes);

  // Task runner of the IO/service thread (mb-io). In-process mojo services that
  // must answer [Sync] calls the main thread makes (e.g. BlobRegistry) bind their
  // receivers here, so the call is serviced off-thread instead of deadlocking.
  // Null before Initialize().
  static scoped_refptr<base::SingleThreadTaskRunner> ServiceTaskRunner();

 private:
  MbRuntime();
  ~MbRuntime();

  std::unique_ptr<base::AtExitManager> at_exit_;
  std::unique_ptr<base::DiscardableMemoryAllocator> discardable_;
  std::unique_ptr<base::Thread> io_thread_;
  std::unique_ptr<mojo::core::ScopedIPCSupport> ipc_support_;
  std::unique_ptr<blink::scheduler::WebThreadScheduler> main_thread_scheduler_;
  std::unique_ptr<MbPlatform> platform_;
  std::unique_ptr<MbScriptWatchdog> script_watchdog_;
  v8::Isolate* isolate_ = nullptr;  // owned by Blink main thread
  bool ok_ = false;
};

}  // namespace mb

#endif  // MINIBLINK_HOST_RUNTIME_MB_RUNTIME_H_
