#include "miniblink_host/frame/mb_lock_manager.h"

#include <stdint.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/pending_associated_remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

namespace mb {
namespace {

using blink::mojom::blink::LockHandle;
using blink::mojom::blink::LockManager;
using blink::mojom::blink::LockMode;
using blink::mojom::blink::LockRequest;
using WaitMode = blink::mojom::blink::LockManager::WaitMode;

// LockHandle is an empty interface — the page just holds the remote; dropping it (when the
// lock's callback promise settles) closes the pipe and releases the lock. One shared inert
// impl backs every handle.
class MbLockHandle : public LockHandle {};

MbLockHandle* SharedLockHandleImpl() {
  static MbLockHandle* p = new MbLockHandle();
  return p;
}

// Process-wide lock state for ONE origin. navigator.locks is partitioned by origin AND shared
// across all same-origin contexts (a lock held by one frame/worker blocks another of the SAME
// origin; different origins are isolated). This lives forever in the per-origin registry below,
// so the held-lock disconnect handlers (which post releases back here) always have a valid target.
// Previously the lock state lived per-LockManager-instance (one per bind), so same-origin contexts
// never shared locks and cross-origin isolation was accidental.
class OriginLockState {
 public:
  void RequestLock(const blink::String& name,
                   LockMode mode,
                   WaitMode wait,
                   mojo::PendingAssociatedRemote<LockRequest> request) {
    mojo::AssociatedRemote<LockRequest> req(std::move(request));
    if (wait == WaitMode::PREEMPT)
      ReleaseAllForName(name);
    // Grant immediately only if no held lock conflicts AND no same-name request is
    // already waiting ahead of this one (FIFO per resource).
    if (IsGrantable(name, mode) && !HasWaiterForName(name)) {
      Grant(name, mode, std::move(req));
    } else if (wait == WaitMode::NO_WAIT) {
      req->Failed();
    } else {
      auto w = std::make_unique<Waiter>();
      const uint64_t id = next_id_++;
      w->id = id;
      w->name = name;
      w->mode = mode;
      w->request = std::move(req);
      // An aborted (AbortSignal) or torn-down request must not linger in the queue
      // and get "granted" into a dead pipe. Drop it on disconnect; post the removal
      // so we don't destroy the remote inside its own disconnect callback.
      w->request.set_disconnect_handler(base::BindOnce(
          [](base::WeakPtr<OriginLockState> self, uint64_t id) {
            if (!self)
              return;
            base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
                FROM_HERE,
                base::BindOnce(&OriginLockState::OnWaiterGone, self, id));
          },
          weak_factory_.GetWeakPtr(), id));
      queue_.push_back(std::move(w));
    }
  }

  void QueryState(
      LockManager::QueryStateCallback callback) {
    blink::Vector<blink::mojom::blink::LockInfoPtr> requested;
    blink::Vector<blink::mojom::blink::LockInfoPtr> held;
    for (const auto& h : held_) {
      held.push_back(blink::mojom::blink::LockInfo::New(h->name, h->mode,
                                                        blink::String()));
    }
    for (const auto& w : queue_) {
      requested.push_back(blink::mojom::blink::LockInfo::New(w->name, w->mode,
                                                             blink::String()));
    }
    std::move(callback).Run(std::move(requested), std::move(held));
  }

 private:
  struct Held {
    uint64_t id = 0;
    blink::String name;
    LockMode mode = LockMode::SHARED;
    std::unique_ptr<mojo::AssociatedReceiver<LockHandle>> handle;
  };
  struct Waiter {
    uint64_t id = 0;
    blink::String name;
    LockMode mode = LockMode::SHARED;
    mojo::AssociatedRemote<LockRequest> request;
  };

  bool IsGrantable(const blink::String& name, LockMode mode) const {
    for (const auto& h : held_) {
      if (h->name != name)
        continue;
      // Exclusive conflicts with anything; shared only with exclusive.
      if (mode == LockMode::EXCLUSIVE || h->mode == LockMode::EXCLUSIVE)
        return false;
    }
    return true;
  }

  // FIFO per resource: a request can't jump ahead of an already-queued same-name
  // request even if it's compatible with the held locks (otherwise a steady stream
  // of shared requests starves a queued exclusive).
  bool HasWaiterForName(const blink::String& name) const {
    for (const auto& w : queue_) {
      if (w->name == name)
        return true;
    }
    return false;
  }

  void Grant(const blink::String& name,
             LockMode mode,
             mojo::AssociatedRemote<LockRequest> request) {
    auto held = std::make_unique<Held>();
    const uint64_t id = next_id_++;
    held->id = id;
    held->name = name;
    held->mode = mode;
    // The handle endpoint must be left UNassociated (pending) so it associates with the
    // LockRequest's pipe when sent via Granted() — a dedicated endpoint would DCHECK.
    mojo::PendingAssociatedRemote<LockHandle> handle_remote;
    held->handle = std::make_unique<mojo::AssociatedReceiver<LockHandle>>(
        SharedLockHandleImpl(),
        handle_remote.InitWithNewEndpointAndPassReceiver());
    // The page drops the handle when the lock's callback promise settles. Don't
    // delete the receiver inside its own disconnect handler — post the release.
    held->handle->set_disconnect_handler(base::BindOnce(
        [](base::WeakPtr<OriginLockState> self, uint64_t id) {
          if (!self)
            return;
          base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
              FROM_HERE, base::BindOnce(&OriginLockState::OnReleased, self, id));
        },
        weak_factory_.GetWeakPtr(), id));
    held_.push_back(std::move(held));
    request->Granted(std::move(handle_remote));
  }

  void OnReleased(uint64_t id) {
    for (auto it = held_.begin(); it != held_.end(); ++it) {
      if ((*it)->id == id) {
        held_.erase(it);
        break;
      }
    }
    ProcessQueue();
  }

  void ReleaseAllForName(const blink::String& name) {
    for (auto it = held_.begin(); it != held_.end();) {
      if ((*it)->name == name)
        it = held_.erase(it);
      else
        ++it;
    }
  }

  // A queued request's pipe disconnected (aborted/torn down) before it was granted.
  void OnWaiterGone(uint64_t id) {
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
      if ((*it)->id == id) {
        queue_.erase(it);
        break;
      }
    }
    // Removing a blocking (e.g. exclusive) waiter can unblock same-name waiters
    // queued behind it.
    ProcessQueue();
  }

  void ProcessQueue() {
    // Grant grantable waiters in FIFO order. Re-scan after each grant since granting
    // a shared lock can unblock following shared waiters. Within a pass, once a
    // request for a name is found non-grantable, skip all later same-name requests so
    // a compatible request can't jump ahead of an earlier blocked one (FIFO per
    // resource — keeps a queued exclusive from being starved by later shared ones).
    bool granted_any = true;
    while (granted_any) {
      granted_any = false;
      blink::Vector<blink::String> blocked;
      for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        const blink::String& name = (*it)->name;
        if (blocked.Contains(name))
          continue;
        if (IsGrantable(name, (*it)->mode)) {
          Grant(name, (*it)->mode, std::move((*it)->request));
          queue_.erase(it);
          granted_any = true;
          break;  // restart the scan; Grant mutated held_
        }
        blocked.push_back(name);
      }
    }
  }

  std::vector<std::unique_ptr<Held>> held_;
  std::vector<std::unique_ptr<Waiter>> queue_;
  uint64_t next_id_ = 1;
  base::WeakPtrFactory<OriginLockState> weak_factory_{this};
};

// origin scope -> its shared lock state. Process-wide; entries live for the process (locks are
// transient but the per-origin state is cheap and the disconnect handlers depend on its stability).
std::map<std::string, std::unique_ptr<OriginLockState>>& LockStates() {
  static auto* m =
      new std::map<std::string, std::unique_ptr<OriginLockState>>();
  return *m;
}
OriginLockState* LockStateFor(const std::string& scope) {
  auto& m = LockStates();
  auto it = m.find(scope);
  if (it == m.end())
    it = m.emplace(scope, std::make_unique<OriginLockState>()).first;
  return it->second.get();
}

// Per-context navigator.locks receiver: resolves its origin once and forwards to the shared
// per-origin state, so same-origin contexts contend on one lock namespace and origins are isolated.
class MbLockManager : public LockManager {
 public:
  explicit MbLockManager(std::string scope) : scope_(std::move(scope)) {}

  void RequestLock(const blink::String& name,
                   LockMode mode,
                   WaitMode wait,
                   mojo::PendingAssociatedRemote<LockRequest> request) override {
    LockStateFor(scope_)->RequestLock(name, mode, wait, std::move(request));
  }
  void QueryState(QueryStateCallback callback) override {
    LockStateFor(scope_)->QueryState(std::move(callback));
  }

 private:
  std::string scope_;
};

}  // namespace

void BindLockManager(
    mojo::PendingReceiver<blink::mojom::blink::LockManager> receiver,
    const std::string& scope) {
  // `scope` is the storage scope the caller already partitions by: a frame's origin (default
  // navigator.locks) or an (origin, bucket) key (a storage bucket's locks). Same scope -> shared
  // lock namespace; different scopes isolated.
  mojo::MakeSelfOwnedReceiver(std::make_unique<MbLockManager>(scope),
                              std::move(receiver));
}

}  // namespace mb
