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
  const int t = timeout_ms_.load(std::memory_order_relaxed);
  deadline_us_.store(t > 0 ? NowUs() + int64_t{t} * 1000 : 0,
                     std::memory_order_relaxed);
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
    int64_t d = deadline_us_.load(std::memory_order_relaxed);
    if (d == 0 || NowUs() <= d)
      continue;
    // Claim this exact deadline: the CAS fails if the task already ended
    // (deadline -> 0) or a new task armed a different deadline, so we terminate
    // ONLY the same task we observed overrunning — never a fresh one.
    if (deadline_us_.compare_exchange_strong(d, 0, std::memory_order_acq_rel)) {
      terminated_.store(true, std::memory_order_release);
      isolate_->TerminateExecution();  // thread-safe per v8-isolate.h
    }
  }
}

}  // namespace mb
