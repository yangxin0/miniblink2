// mb_dedicated_worker_host — drives a dedicated Worker's thread in-process.
//
// STEP 2 of worker bring-up (see PROGRESS.md "Workers"). The real renderer asks the
// browser process to create a DedicatedWorkerHost and stream the top-level script; we
// have no browser, so this factory client SYNTHESIZES that handshake locally:
//   - OnWorkerHostCreated(broker, host, origin): the frame interface broker + an empty
//     DedicatedWorkerHost receiver + the script origin.
//   - fetch the script (MbFetchUrl) and feed it back via OnScriptLoadStarted as a
//     WorkerMainScriptLoadParameters (a 200 response head + the script bytes over a data
//     pipe + a URLLoaderClient endpoint we drive to OnComplete) — which spins the worker
//     thread and runs the script.
//
// Replaces the earlier inert stub (which returned a host that never started the worker).

#ifndef MINIBLINK_HOST_WORKER_MB_DEDICATED_WORKER_HOST_H_
#define MINIBLINK_HOST_WORKER_MB_DEDICATED_WORKER_HOST_H_

#include <memory>

namespace blink {
class WebDedicatedWorker;
class WebDedicatedWorkerHostFactoryClient;
}  // namespace blink

namespace mb {

// Build the factory client blink uses to start `worker`. Returned to blink from
// MbPlatform::CreateDedicatedWorkerHostFactoryClient.
std::unique_ptr<blink::WebDedicatedWorkerHostFactoryClient>
MakeDedicatedWorkerHostFactoryClient(blink::WebDedicatedWorker* worker);

}  // namespace mb

#endif  // MINIBLINK_HOST_WORKER_MB_DEDICATED_WORKER_HOST_H_
