// mb_shared_worker — run SharedWorkers in-process.
//
// `new SharedWorker(url)` reaches the renderer's SharedWorkerClientHolder, which requests
// a mojom::SharedWorkerConnector from the frame's BrowserInterfaceBroker and calls
// Connect(). In the real renderer the browser process receives that, creates the worker,
// and drives WebSharedWorker::CreateAndStart back in the renderer. We have no browser, so
// MbSharedWorkerConnector does both halves in-process: it synthesizes the browser-fetched
// script (mb_worker_script) and starts the worker, then delivers the page's MessagePort to
// the worker's `onconnect`.
//
// The connector receiver MUST be bound on the MAIN thread (CreateAndStart is main-thread
// only), even though the frame broker runs on the service thread — so the broker posts the
// receiver here via BindSharedWorkerConnector.

#ifndef MINIBLINK_HOST_WORKER_MB_SHARED_WORKER_H_
#define MINIBLINK_HOST_WORKER_MB_SHARED_WORKER_H_

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/mojom/worker/shared_worker_connector.mojom-blink.h"

namespace mb {

// Bind a SharedWorkerConnector receiver to an in-process connector. MUST be called on the
// main thread (the frame broker, on the service thread, PostTasks here).
void BindSharedWorkerConnector(
    mojo::PendingReceiver<blink::mojom::blink::SharedWorkerConnector> receiver);

}  // namespace mb

#endif  // MINIBLINK_HOST_WORKER_MB_SHARED_WORKER_H_
