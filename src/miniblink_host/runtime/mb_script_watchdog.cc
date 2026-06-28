// mb_script_watchdog — see header.

#include "miniblink_host/runtime/mb_script_watchdog.h"

#include "base/task/current_thread.h"
#include "base/time/time.h"
#include "v8/include/v8-isolate.h"

namespace mb {
namespace {
int64_t NowUs() {
  return base::TimeTicks::Now().since_origin().InMicroseconds();
}
}  // namespace

MbScriptWatchdog::MbScriptWatchdog() = default;

MbScriptWatchdog::~MbScriptWatchdog() {
  Stop();
}

void MbScriptWatchdog::Start(v8::Isolate* isolate) {
  if (running_.load(std::memory_order_relaxed))
    return;
  isolate_ = isolate;
  running_.store(true, std::memory_order_relaxed);
  base::CurrentThread::Get()->AddTaskObserver(this);
  observer_added_ = true;
  base::PlatformThread::Create(0, this, &thread_);
}

void MbScriptWatchdog::Stop() {
  if (!running_.exchange(false, std::memory_order_relaxed))
    return;
  if (!thread_.is_null())
    base::PlatformThread::Join(thread_);
  if (observer_added_) {
    base::CurrentThread::Get()->RemoveTaskObserver(this);
    observer_added_ = false;
  }
}

void MbScriptWatchdog::SetTimeoutMs(int ms) {
  timeout_ms_.store(ms < 0 ? 0 : ms, std::memory_order_relaxed);
}

void MbScriptWatchdog::WillProcessTask(const base::PendingTask&, bool) {
  // Clear any termination the monitor raced in just as the previous task ended,
  // before this task's JS runs (else it would be spuriously terminated).
  if (terminated_.exchange(false, std::memory_order_acq_rel))
    isolate_->CancelTerminateExecution();
  ++task_depth_;  // main thread only; tracks nested run-loops
  // Bump the generation BEFORE arming the deadline: a successful deadline CAS in the
  // monitor then implies the generation it reads belongs to the task that armed that
  // deadline (a newer task would have changed the deadline and failed the CAS).
  task_generation_.fetch_add(1, std::memory_order_release);
  const int t = timeout_ms_.load(std::memory_order_relaxed);
  deadline_us_.store(t > 0 ? NowUs() + int64_t{t} * 1000 : 0,
                     std::memory_order_release);
}

void MbScriptWatchdog::DidProcessTask(const base::PendingTask&) {
  if (task_depth_ > 0)
    --task_depth_;
  // Only DISARM at the outermost level: a nested task (a run-loop pumped inside an outer
  // task) clearing the deadline would leave the rest of the outer task unguarded. Inner
  // tasks re-armed the deadline in WillProcessTask, so the outer task stays covered.
  if (task_depth_ == 0)
    deadline_us_.store(0, std::memory_order_relaxed);
  if (terminated_.exchange(false, std::memory_order_acq_rel))
    isolate_->CancelTerminateExecution();
}

void MbScriptWatchdog::ThreadMain() {
  base::PlatformThread::SetName("mb-script-watchdog");
  while (running_.load(std::memory_order_relaxed)) {
    base::PlatformThread::Sleep(base::Milliseconds(25));
    int64_t d = deadline_us_.load(std::memory_order_acquire);
    if (d == 0 || NowUs() <= d)
      continue;
    // Capture the generation of the task that armed `d` BEFORE claiming it. A new/ended
    // task would have changed the deadline (failing the CAS below), so on a successful
    // CAS this generation belongs to the task we observed overrunning.
    const uint64_t g = task_generation_.load(std::memory_order_acquire);
    // Claim this exact deadline: the CAS fails if the task already ended
    // (deadline -> 0) or a new task armed a different deadline, so we terminate
    // ONLY the same task we observed overrunning — never a fresh one.
    if (deadline_us_.compare_exchange_strong(d, 0, std::memory_order_acq_rel)) {
      // Guard the claim->terminate window: between the CAS and the actual kill the main
      // thread can finish the overrunning task and start a new one (which bumps the
      // generation and arms its own deadline). Re-verify the generation is still ours
      // both BEFORE and AFTER TerminateExecution — if a fresh task armed, do not kill it
      // (its first JS execution would otherwise be terminated by this stale claim).
      if (task_generation_.load(std::memory_order_acquire) != g)
        continue;  // a new task already started; the overrun task is gone
      terminated_.store(true, std::memory_order_release);
      isolate_->TerminateExecution();  // thread-safe per v8-isolate.h
      if (task_generation_.load(std::memory_order_acquire) != g) {
        // A new task armed around the terminate; undo so its first JS isn't killed.
        isolate_->CancelTerminateExecution();
        terminated_.store(false, std::memory_order_release);
      }
    }
  }
}

}  // namespace mb
