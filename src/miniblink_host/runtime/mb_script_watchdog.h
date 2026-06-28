// mb_script_watchdog — kill a runaway synchronous script before it hangs the process.
//
// A single-process embedder shares ONE main thread with the page, so a synchronous
// infinite loop in page JS (e.g. `while(true){}`) blocks everything forever with no
// recovery. This watchdog guards that case: a main-thread base::TaskObserver arms a
// per-task deadline, and a separate monitor thread calls v8::Isolate::TerminateExecution()
// (documented thread-safe) if any ONE task overruns the timeout. Because the deadline is
// re-armed on every task, slow ASYNC work (many short tasks waiting on the network) is
// unaffected — only a single task that never returns to the loop is terminated. After a
// termination the very next task boundary calls CancelTerminateExecution(), so the isolate
// is immediately usable again (the embedder can load the next page).
//
// Opt-in: SetTimeoutMs(0) (the default) disables it. A scraper hitting untrusted pages
// sets e.g. 5000–10000 ms.

#ifndef MINIBLINK_HOST_RUNTIME_MB_SCRIPT_WATCHDOG_H_
#define MINIBLINK_HOST_RUNTIME_MB_SCRIPT_WATCHDOG_H_

#include <atomic>
#include <cstdint>

#include "base/task/task_observer.h"
#include "base/threading/platform_thread.h"

namespace v8 {
class Isolate;
}  // namespace v8

namespace mb {

class MbScriptWatchdog : public base::TaskObserver,
                         public base::PlatformThread::Delegate {
 public:
  MbScriptWatchdog();
  ~MbScriptWatchdog() override;

  // Install the TaskObserver on the CURRENT (main) thread and start the monitor
  // thread. Call once, on the main thread, after the isolate exists.
  void Start(v8::Isolate* isolate);
  void Stop();

  // Max wall-clock a single main-thread task may run before its JS is terminated.
  // <= 0 disables (the default). Process-global; safe to call from the main thread.
  void SetTimeoutMs(int ms);
  int timeout_ms() const { return timeout_ms_.load(std::memory_order_relaxed); }

  // base::TaskObserver (main thread).
  void WillProcessTask(const base::PendingTask&,
                       bool was_blocked_or_low_priority) override;
  void DidProcessTask(const base::PendingTask&) override;

  // base::PlatformThread::Delegate (monitor thread).
  void ThreadMain() override;

 private:
  v8::Isolate* isolate_ = nullptr;
  std::atomic<int> timeout_ms_{0};        // 0 = disabled
  std::atomic<int64_t> deadline_us_{0};   // 0 = no task armed; else TimeTicks deadline
  // Monotonic per-task counter, bumped (before the deadline) on every WillProcessTask.
  // The monitor captures it with the deadline it claims and re-checks it around the
  // terminate, so a task that armed AFTER the claim is never killed by a stale claim.
  std::atomic<uint64_t> task_generation_{0};
  std::atomic<bool> terminated_{false};   // monitor fired a TerminateExecution
  std::atomic<bool> running_{false};
  base::PlatformThreadHandle thread_;
  bool observer_added_ = false;
  int task_depth_ = 0;  // main-thread task nesting; only the outermost arms/disarms
};

}  // namespace mb

#endif  // MINIBLINK_HOST_RUNTIME_MB_SCRIPT_WATCHDOG_H_
