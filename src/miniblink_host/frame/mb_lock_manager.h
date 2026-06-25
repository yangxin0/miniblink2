// mb_lock_manager — an in-process Web Locks (navigator.locks) backend.
//
// blink requests a mojom::LockManager from the frame's BrowserInterfaceBroker. Without
// one, navigator.locks.request() never resolves. This grants locks with real exclusive/
// shared serialization: a held lock is released when its LockHandle pipe closes (which
// blink does when the request's callback promise settles), and queued waiters are then
// processed in order. Process-wide so locks coordinate across this renderer's contexts.

#ifndef MINIBLINK_HOST_FRAME_MB_LOCK_MANAGER_H_
#define MINIBLINK_HOST_FRAME_MB_LOCK_MANAGER_H_

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/mojom/locks/lock_manager.mojom-blink.h"

namespace mb {

// Bind a LockManager receiver to the in-process backend (self-owned). Safe to call on the
// broker's service thread (no [Sync] methods).
void BindLockManager(
    mojo::PendingReceiver<blink::mojom::blink::LockManager> receiver);

}  // namespace mb

#endif  // MINIBLINK_HOST_FRAME_MB_LOCK_MANAGER_H_
