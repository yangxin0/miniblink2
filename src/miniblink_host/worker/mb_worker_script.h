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

#include "third_party/blink/public/common/loader/worker_main_script_load_parameters.h"

namespace mb {

// Fetch `url` and build the worker main-script load parameters. Returns nullptr if the
// fetch fails (the caller should then report a script-load failure to blink). On success,
// a delivery object is spawned that streams the bytes and self-destructs when consumed.
// `host_ctx` (the creating document's MbWebView, may be null) scopes the fetch to that
// view's per-view request-mock hook and session cookie jar; null falls back to the
// process-wide hook and default jar.
std::unique_ptr<blink::WorkerMainScriptLoadParameters> MakeWorkerMainScriptParams(
    const std::string& url, const void* host_ctx = nullptr);

}  // namespace mb

#endif  // MINIBLINK_HOST_WORKER_MB_WORKER_SCRIPT_H_
