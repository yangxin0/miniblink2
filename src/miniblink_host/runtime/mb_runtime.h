// mb_runtime — process-wide Blink bring-up for miniblink-modern.
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

// Owns the global engine state. Single instance per process (mbInitialize).
class MbRuntime {
 public:
  static MbRuntime* Get();        // null until Initialize()
  static bool Initialize();       // returns false if already initialized / failed
  static void Shutdown();

  // Drain the main-thread task runner once (loading, parsing, lifecycle steps).
  void PumpOnce();

  MbPlatform* platform() { return platform_.get(); }

 private:
  MbRuntime();
  ~MbRuntime();

  std::unique_ptr<base::AtExitManager> at_exit_;
  std::unique_ptr<base::DiscardableMemoryAllocator> discardable_;
  std::unique_ptr<base::Thread> io_thread_;
  std::unique_ptr<mojo::core::ScopedIPCSupport> ipc_support_;
  std::unique_ptr<blink::scheduler::WebThreadScheduler> main_thread_scheduler_;
  std::unique_ptr<MbPlatform> platform_;
  v8::Isolate* isolate_ = nullptr;  // owned by Blink main thread
  bool ok_ = false;
};

}  // namespace mb

#endif  // MINIBLINK_HOST_RUNTIME_MB_RUNTIME_H_
