#include "miniblink_host/frame/mb_lock_manager.h"

#include <stdint.h>

#include <memory>
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

// LockHandle is an empty interface — the page just holds the remote; dropping it (when the
// lock's callback promise settles) closes the pipe and releases the lock. One shared inert
// impl backs every handle.
class MbLockHandle : public LockHandle {};

MbLockHandle* SharedLockHandleImpl() {
  static MbLockHandle* p = new MbLockHandle();
  return p;
}

class MbLockManager : public LockManager {
 public:
  MbLockManager() = default;

  void RequestLock(const blink::String& name,
                   LockMode mode,
                   WaitMode wait,
                   mojo::PendingAssociatedRemote<LockRequest> request) override {
    mojo::AssociatedRemote<LockRequest> req(std::move(request));
    if (wait == WaitMode::PREEMPT)
      ReleaseAllForName(name);
    if (IsGrantable(name, mode)) {
      Grant(name, mode, std::move(req));
    } else if (wait == WaitMode::NO_WAIT) {
      req->Failed();
    } else {
      auto w = std::make_unique<Waiter>();
      w->name = name;
      w->mode = mode;
      w->request = std::move(req);
      queue_.push_back(std::move(w));
    }
  }

  void QueryState(QueryStateCallback callback) override {
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
        [](base::WeakPtr<MbLockManager> self, uint64_t id) {
          if (!self)
            return;
          base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
              FROM_HERE, base::BindOnce(&MbLockManager::OnReleased, self, id));
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

  void ProcessQueue() {
    // Grant any waiter that is now grantable, in FIFO order. Re-scan after each
    // grant since granting a shared lock can unblock following shared waiters.
    bool granted_any = true;
    while (granted_any) {
      granted_any = false;
      for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (IsGrantable((*it)->name, (*it)->mode)) {
          Grant((*it)->name, (*it)->mode, std::move((*it)->request));
          queue_.erase(it);
          granted_any = true;
          break;
        }
      }
    }
  }

  std::vector<std::unique_ptr<Held>> held_;
  std::vector<std::unique_ptr<Waiter>> queue_;
  uint64_t next_id_ = 1;
  base::WeakPtrFactory<MbLockManager> weak_factory_{this};
};

}  // namespace

void BindLockManager(
    mojo::PendingReceiver<blink::mojom::blink::LockManager> receiver) {
  mojo::MakeSelfOwnedReceiver(std::make_unique<MbLockManager>(),
                              std::move(receiver));
}

}  // namespace mb
