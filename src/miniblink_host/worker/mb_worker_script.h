// mb_worker_script — synthesize the "browser-fetched" top-level worker script for blink.
//
// Both dedicated and shared workers in the real renderer receive their main script from
// the browser as a WorkerMainScriptLoadParameters (a response head + the bytes streamed
// over a data pipe + a URLLoaderClient endpoint). We have no browser, so this fetches the
// script in-process (MbFetchUrl: file/http(s)/data:) and builds those params, including a
// self-owned delivery object that writes the body and signals completion.

#ifndef MINIBLINK_HOST_WORKER_MB_WORKER_SCRIPT_H_
#define MINIBLINK_HOST_WORKER_MB_WORKER_SCRIPT_H_

#include <memory>
#include <string>

#include "base/memory/scoped_refptr.h"
#include "third_party/blink/public/common/loader/worker_main_script_load_parameters.h"

namespace mb {

class MbLoaderViewContext;

// Fetch `url` and build the worker main-script load parameters. Returns nullptr if the
// fetch fails (the caller should then report a script-load failure to blink). On success,
// a delivery object is spawned that streams the bytes and self-destructs when consumed.
// `view_context` retains the creating view's session and engine runner across nested
// workers. The whole interception/fetch/response-hook phase runs synchronously on that
// engine runner even when a nested worker requests its script from a worker thread.
std::unique_ptr<blink::WorkerMainScriptLoadParameters> MakeWorkerMainScriptParams(
    const std::string& url,
    scoped_refptr<MbLoaderViewContext> view_context = nullptr);

// The MbWorkerScriptWaiterCountForTesting rendezvous diagnostic this TU
// defines is declared in test/mb_test_seams.h (included by the .cc).

}  // namespace mb

#endif  // MINIBLINK_HOST_WORKER_MB_WORKER_SCRIPT_H_
